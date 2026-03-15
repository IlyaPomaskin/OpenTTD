/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_sprite.cpp Sprite atlas packer and GPU upload for OpenGL ES. */

#include "../stdafx.h"
#include "gles_sprite.h"
#include "../debug.h"
#include "../gfx_func.h"
#include "../spritecache.h"
#include "../spritecache_internal.h"
#include "../spriteloader/grf.hpp"
#include "../table/sprites.h"
#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>

#include "../safeguards.h"

void GLESSpriteAtlas::Init()
{
	/* Query maximum texture size, clamp to 4096 for reasonable atlas pages.
	 * Also query GL_MAX_3D_TEXTURE_SIZE because some emulators (goldfish)
	 * incorrectly validate glTexImage3D for GL_TEXTURE_2D_ARRAY against it. */
	GLint max_size = 2048, max_3d_size = 2048;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_3d_size);
	this->atlas_size = static_cast<uint16_t>(std::min({max_size, max_3d_size, (GLint)4096}));

	/* Tightly-packed rows for all texture uploads (critical for GL_RED/R8
	 * where row byte count may not be a multiple of the default alignment 4). */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* Create double-buffered PBOs for async sprite upload. */
	GLuint pbos[2];
	glGenBuffers(2, pbos);
	for (int i = 0; i < 2; i++) {
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[i]);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, PBO_SIZE, nullptr, GL_STREAM_DRAW);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	this->pbo_current.pbo = pbos[0];
	this->pbo_inflight.pbo = pbos[1];
	/* Create 1x1 semi-transparent black placeholder sprite for missing atlas entries. */
	{
		uint8_t pink_rgba[4] = { 0, 0, 0, 128 };
		uint8_t pink_m[1] = { 0 };
		GLESSpriteRegion region;
		if (PackRegion(this->colour_pages, false, 1, 1, region)) {
			glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, region.x, region.y, region.atlas_idx,
			                1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pink_rgba);
			this->placeholder_entry.colour = region;
			this->placeholder_entry.has_remap = false;
			this->placeholder_entry.palette_only = false;
			if (PackRegion(this->remap_pages, true, 1, 1, this->placeholder_entry.remap)) {
				glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
				glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
				                this->placeholder_entry.remap.x, this->placeholder_entry.remap.y,
				                this->placeholder_entry.remap.atlas_idx,
				                1, 1, 1, GL_RED, GL_UNSIGNED_BYTE, pink_m);
			}
			this->placeholder_ready = true;
			Debug(driver, 0, "GLES: Placeholder sprite created at atlas ({},{}) page {}",
			      region.x, region.y, region.atlas_idx);
		}
	}

	Debug(driver, 0, "GLES: Atlas page size {}x{} (max_tex={} max_3d={}), PBO async upload: 2x{}KB budget={}ms",
	      this->atlas_size, this->atlas_size, max_size, max_3d_size, PBO_SIZE / 1024, PBO_TIME_BUDGET_US / 1000);
}

void GLESSpriteAtlas::Destroy()
{
	if (this->colour_array_tex != 0) glDeleteTextures(1, &this->colour_array_tex);
	if (this->remap_array_tex != 0) glDeleteTextures(1, &this->remap_array_tex);
	this->colour_array_tex = 0;
	this->remap_array_tex = 0;
	this->colour_pages.clear();
	this->remap_pages.clear();
	this->sprites.clear();

	GLuint pbos[2] = { this->pbo_current.pbo, this->pbo_inflight.pbo };
	if (pbos[0] != 0 || pbos[1] != 0) glDeleteBuffers(2, pbos);
	if (this->pbo_inflight.fence != nullptr) glDeleteSync(this->pbo_inflight.fence);
	this->pbo_current = {};
	this->pbo_inflight = {};
	this->pbo_inflight_keys.clear();
}

void GLESSpriteAtlas::ProcessPendingClear()
{
	/* Check if sprite reload measurement is complete (no new sprites since last check). */
	if (this->measuring_reload && _gles_perf.gpu_sprites_new == 0 && this->sprites_after_clear > 0) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - this->clear_time).count();
		Debug(driver, 0, "GLES: Reload complete: {} sprites in {}ms",
		      this->sprites_after_clear, elapsed);
		this->EstimateAtlasPages();
		this->measuring_reload = false;
	}

	if (!this->clear_pending.exchange(false)) return;
	this->ClearSprites();
}

void GLESSpriteAtlas::ClearSprites()
{
	/* Delete GPU textures and reset atlas layout.
	 * Keep upload_queue — it may already contain data for the new map
	 * (enqueued from the game thread between RequestClear and this call).
	 * ProcessPBOUploads will upload them on demand. */
	if (this->colour_array_tex != 0) glDeleteTextures(1, &this->colour_array_tex);
	if (this->remap_array_tex != 0) glDeleteTextures(1, &this->remap_array_tex);
	this->colour_array_tex = 0;
	this->remap_array_tex = 0;

	size_t old_pages = this->colour_pages.size() + this->remap_pages.size();
	size_t old_sprites = this->sprites.size();

	this->colour_pages.clear();
	this->remap_pages.clear();
	this->sprites.clear();

	/* Clear PBO inflight state — sprites no longer valid after atlas clear.
	 * Keep PBO buffers themselves (reuse across clears). */
	if (this->pbo_inflight.fence != nullptr) {
		glDeleteSync(this->pbo_inflight.fence);
		this->pbo_inflight.fence = nullptr;
	}
	this->pbo_inflight.entries.clear();
	this->pbo_inflight.used_bytes = 0;
	this->pbo_current.entries.clear();
	this->pbo_current.used_bytes = 0;
	this->pbo_inflight_keys.clear();

	this->placeholder_ready = false;

	this->clear_time = std::chrono::steady_clock::now();
	this->sprites_after_clear = 0;
	this->measuring_reload = true;

	/* Signal game thread to clear known_sprites.
	 * Sprites still in upload_queue will be re-added on next Enqueue. */
	this->known_clear_pending.store(true);

	Debug(driver, 0, "GLES: Atlas ClearSprites: deleted {} pages, {} gpu entries, {} queued",
	      old_pages, old_sprites, this->upload_queue.size());

	/* Rebuild GL-thread sprite file copies (GRF files may have changed). */
	this->BuildGLSpriteFiles();
}

/* ---- PBO async upload pipeline ---- */

void GLESSpriteAtlas::PBOCheckInflight()
{
	if (this->pbo_inflight.fence == nullptr) return;

	GLenum result = glClientWaitSync(this->pbo_inflight.fence, 0, 0);
	if (result == GL_TIMEOUT_EXPIRED) {
		_gles_perf.pbo_fence_waits++;
		return; /* Not ready yet, try next frame. */
	}

	/* Fence signaled (ALREADY_SIGNALED or CONDITION_SATISFIED) — sprites are uploaded. */
	glDeleteSync(this->pbo_inflight.fence);
	this->pbo_inflight.fence = nullptr;

	for (const auto &pe : this->pbo_inflight.entries) {
		this->sprites[pe.key] = pe.entry;
		_gles_perf.gpu_sprites_new++;

		if (this->measuring_reload) {
			this->sprites_after_clear++;
		}
	}

	size_t count = this->pbo_inflight.entries.size();
	_gles_perf.pbo_batches_completed++;
	_gles_perf.pbo_sprites_uploaded += count;
	this->pbo_inflight.entries.clear();
	this->pbo_inflight.used_bytes = 0;
	this->pbo_inflight_keys.clear();

	if (count > 0) {
		Debug(driver, 2, "GLES: PBO inflight batch completed: {} sprites", count);
	}
}

void GLESSpriteAtlas::PBOFillBatch(std::chrono::steady_clock::time_point t_start)
{
	auto t0 = std::chrono::steady_clock::now();

	/* Drain upload queue in one swap (O(1), no per-sprite locking). */
	std::vector<GLESUploadRequest> batch;
	{
		std::lock_guard<std::mutex> lock(this->queue_mutex);
		batch = std::move(this->upload_queue);
	}

	if (batch.empty()) return;

	/* Sort by visual priority so the most visible sprites upload first.
	 * Ranges: landscape (sprites.h), water (sprites.h), trees (tree_land.h
	 * _tree_layout_sprite 0x628-0x7d3), buildings (town_land.h 0x58d-0x627),
	 * roads (sprites.h), rails (sprites.h). */
	auto sprite_priority = [](SpriteID s) -> int {
		if (s >= SPR_FLAT_BARE_LAND && s <= SPR_FLAT_SNOW_DESERT_TILE + 18) return 0;  // landscape
		if (s >= SPR_FOUNDATION_BASE && s <= SPR_SHADOW_CELL) return 0;                // foundations
		if (s >= SPR_HEDGE_BUSHES && s <= SPR_FARMLAND_HAYPACKS + 18) return 0;        // hedges + farmland
		if (s >= SPR_FLAT_WATER_TILE && s <= SPR_FLAT_WATER_TILE + 18) return 1;       // water tiles
		if (s >= SPR_SHORE_BASE && s < SPR_SHORE_BASE + 18) return 1;                  // shore
		if (s >= SPR_SHIP_DEPOT_SE_FRONT && s <= SPR_BUOY) return 1;                   // ship depots + buoy
		if (s >= 1576 && s <= 2003) return 2;                                           // trees (tree_land.h)
		if (s >= 1421 && s <= 1575) return 3;                                           // town buildings (town_land.h)
		if (s >= SPR_ROAD_PAVED_STRAIGHT_Y && s <= SPR_ROAD_DEPOT + 18) return 4;     // roads
		if (s >= SPR_RAIL_SINGLE_X && s <= SPR_TRACK_FENCE_SLOPE_NW) return 5;        // rails + signals + fences
		return 6;
	};
	std::stable_sort(batch.begin(), batch.end(), [&sprite_priority](const GLESUploadRequest &a, const GLESUploadRequest &b) {
		return sprite_priority(static_cast<SpriteID>(a.key >> 4)) < sprite_priority(static_cast<SpriteID>(b.key >> 4));
	});

	/* Two-pass approach: PackRegion first (may call AllocPage → glTexImage3D),
	 * then map PBO and write pixels. This avoids the GLES bug where
	 * glTexImage3D(..., nullptr) with a bound PBO reads from PBO offset 0
	 * instead of creating an empty texture, corrupting atlas layers. */

	/* --- Pass 1: pack atlas regions (no PBO bound) --- */
	struct PreparedSprite {
		size_t batch_idx;
		PBOPendingEntry pe;
		size_t npixels;
	};
	std::vector<PreparedSprite> prepared;
	size_t total_bytes = 0;
	int skipped_uploaded = 0, skipped_full = 0;
	int fail_colour = 0, fail_remap = 0;
	int n_palette_only = 0, n_no_remap = 0;
	size_t colour_pages_before = this->colour_pages.size();
	size_t remap_pages_before = this->remap_pages.size();
	bool time_exceeded = false;

	for (size_t bi = 0; bi < batch.size(); bi++) {
		/* Check time budget after each sprite. */
		if (!prepared.empty()) {
			auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - t_start).count();
			if (elapsed_us >= PBO_TIME_BUDGET_US) {
				time_exceeded = true;
				/* Put remaining sprites back into queue. */
				std::lock_guard<std::mutex> lock(this->queue_mutex);
				for (size_t oi = bi; oi < batch.size(); oi++) {
					this->upload_queue.push_back(std::move(batch[oi]));
				}
				break;
			}
		}

		auto &req = batch[bi];

		if (this->sprites.count(req.key)) {
			skipped_uploaded++;
			continue;
		}

		size_t npixels = static_cast<size_t>(req.width) * req.height;
		if (req.pixels.size() != npixels) {
			Debug(driver, 0, "GLES: PBO PIXEL COUNT MISMATCH sprite={} key={:#x} {}x{}={} but pixels.size={}",
			      static_cast<SpriteID>(req.key >> 4), req.key, req.width, req.height, npixels, req.pixels.size());
			continue;
		}
		size_t need = npixels * 4 + (req.has_remap ? npixels : 0);
		if (total_bytes + need > PBO_SIZE) {
			skipped_full++;
			/* Put overflow back into queue for next round. */
			std::lock_guard<std::mutex> lock(this->queue_mutex);
			for (size_t oi = bi; oi < batch.size(); oi++) {
				this->upload_queue.push_back(std::move(batch[oi]));
			}
			break;
		}

		PBOPendingEntry pe;
		pe.key = req.key;
		pe.width = req.width;
		pe.height = req.height;
		pe.has_remap = req.has_remap;
		pe.entry.has_remap = req.has_remap;
		pe.entry.palette_only = req.has_remap && !req.has_rgb;

		if (pe.entry.palette_only) n_palette_only++;
		if (!req.has_remap) n_no_remap++;

		/* Pack atlas regions NOW — no PBO bound, so AllocPage is safe. */
		if (!PackRegion(this->colour_pages, false, req.width, req.height, pe.entry.colour)) {
			fail_colour++;
			Debug(driver, 0, "GLES: PBO PackRegion FAIL colour sprite={} key={:#x} {}x{}",
			      static_cast<SpriteID>(req.key >> 4), req.key, req.width, req.height);
			continue;
		}

		pe.colour_offset = total_bytes;
		total_bytes += npixels * 4;

		if (req.has_remap) {
			if (!PackRegion(this->remap_pages, true, req.width, req.height, pe.entry.remap)) {
				fail_remap++;
				Debug(driver, 0, "GLES: PBO PackRegion FAIL remap sprite={} key={:#x} {}x{}",
				      static_cast<SpriteID>(req.key >> 4), req.key, req.width, req.height);
				continue;
			}
			pe.remap_offset = total_bytes;
			total_bytes += npixels;
		} else {
			pe.entry.remap = pe.entry.colour;
			pe.remap_offset = 0;
		}

		prepared.push_back({bi, pe, npixels});
	}

	_gles_perf.pbo_candidates += batch.size();
	_gles_perf.pbo_already_uploaded += skipped_uploaded;
	_gles_perf.pbo_pbo_full += skipped_full;
	_gles_perf.pbo_pack_colour_fail += fail_colour;
	_gles_perf.pbo_pack_remap_fail += fail_remap;
	_gles_perf.pbo_palette_only += n_palette_only;
	_gles_perf.pbo_no_remap += n_no_remap;
	_gles_perf.pbo_alloc_pages += (this->colour_pages.size() - colour_pages_before)
	                             + (this->remap_pages.size() - remap_pages_before);

	if (prepared.empty()) return;

	/* --- Pass 2: map PBO and write pixel data (no PackRegion/AllocPage calls) --- */
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->pbo_current.pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, PBO_SIZE, nullptr, GL_STREAM_DRAW); /* Orphan. */
	void *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, PBO_SIZE,
	                                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
	if (mapped == nullptr) {
		Debug(driver, 0, "GLES: PBO glMapBufferRange failed!");
		_gles_perf.pbo_map_fail++;
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		return;
	}

	uint8_t *ptr = static_cast<uint8_t *>(mapped);

	for (auto &prep : prepared) {
		const auto &pixels = batch[prep.batch_idx].pixels;

		/* Write RGBA. */
		uint8_t *rgba = ptr + prep.pe.colour_offset;
		for (size_t i = 0; i < prep.npixels; i++) {
			rgba[i * 4 + 0] = pixels[i].r;
			rgba[i * 4 + 1] = pixels[i].g;
			rgba[i * 4 + 2] = pixels[i].b;
			rgba[i * 4 + 3] = pixels[i].a;
		}

		/* Write remap. */
		if (prep.pe.has_remap) {
			uint8_t *m = ptr + prep.pe.remap_offset;
			for (size_t i = 0; i < prep.npixels; i++) {
				m[i] = pixels[i].m;
			}
		}

		this->pbo_current.entries.push_back(prep.pe);
	}

	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	this->pbo_current.used_bytes = total_bytes;

	auto t1 = std::chrono::steady_clock::now();
	_gles_perf.pbo_fill_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

void GLESSpriteAtlas::PBOSubmitBatch()
{
	auto t0 = std::chrono::steady_clock::now();

	if (this->pbo_current.entries.empty()) {
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		return;
	}

	/* PBO is already bound from FillBatch. Issue async glTexSubImage3D calls. */
	for (const auto &pe : this->pbo_current.entries) {
		/* Validate atlas region before GL call. */
		if (pe.entry.colour.atlas_idx >= this->colour_pages.size()) {
			Debug(driver, 0, "GLES: PBO SUBMIT BAD colour atlas_idx={} >= pages={} sprite={} key={:#x}",
			      pe.entry.colour.atlas_idx, this->colour_pages.size(),
			      static_cast<SpriteID>(pe.key >> 4), pe.key);
			continue;
		}

		glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
		                pe.entry.colour.x, pe.entry.colour.y, pe.entry.colour.atlas_idx,
		                pe.width, pe.height, 1,
		                GL_RGBA, GL_UNSIGNED_BYTE,
		                reinterpret_cast<const void *>(static_cast<uintptr_t>(pe.colour_offset)));

		GLenum err = glGetError();
		if (err != GL_NO_ERROR) {
			Debug(driver, 0, "GLES: PBO glTexSubImage3D colour ERROR {:#x} sprite={} key={:#x} pos=({},{},{}) size={}x{} offset={}",
			      err, static_cast<SpriteID>(pe.key >> 4), pe.key,
			      pe.entry.colour.x, pe.entry.colour.y, pe.entry.colour.atlas_idx,
			      pe.width, pe.height, pe.colour_offset);
		}

		if (pe.has_remap) {
			if (pe.entry.remap.atlas_idx >= this->remap_pages.size()) {
				Debug(driver, 0, "GLES: PBO SUBMIT BAD remap atlas_idx={} >= pages={} sprite={} key={:#x}",
				      pe.entry.remap.atlas_idx, this->remap_pages.size(),
				      static_cast<SpriteID>(pe.key >> 4), pe.key);
				continue;
			}

			glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
			                pe.entry.remap.x, pe.entry.remap.y, pe.entry.remap.atlas_idx,
			                pe.width, pe.height, 1,
			                GL_RED, GL_UNSIGNED_BYTE,
			                reinterpret_cast<const void *>(static_cast<uintptr_t>(pe.remap_offset)));

			err = glGetError();
			if (err != GL_NO_ERROR) {
				Debug(driver, 0, "GLES: PBO glTexSubImage3D remap ERROR {:#x} sprite={} key={:#x} pos=({},{},{}) size={}x{} offset={}",
				      err, static_cast<SpriteID>(pe.key >> 4), pe.key,
				      pe.entry.remap.x, pe.entry.remap.y, pe.entry.remap.atlas_idx,
				      pe.width, pe.height, pe.remap_offset);
			}
		}
	}

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	_gles_perf.pbo_batches_submitted++;
	_gles_perf.pbo_bytes_uploaded += this->pbo_current.used_bytes;

	/* Register sprites immediately — GL command stream is serialized, so
	 * glTexSubImage3D DMA will complete before any subsequent draw calls
	 * that reference these sprites. No fence needed. */
	for (const auto &pe : this->pbo_current.entries) {
		/* Check for dimension mismatch between PBO entry and atlas region. */
		if (pe.width != pe.entry.colour.w || pe.height != pe.entry.colour.h) {
			Debug(driver, 0, "GLES: PBO DIM MISMATCH sprite={} key={:#x} pbo={}x{} atlas={}x{}",
			      static_cast<SpriteID>(pe.key >> 4), pe.key,
			      pe.width, pe.height, pe.entry.colour.w, pe.entry.colour.h);
		}
		/* Check if this overwrites an existing entry with different coords. */
		auto prev = this->sprites.find(pe.key);
		if (prev != this->sprites.end()) {
			const auto &old = prev->second;
			if (old.colour.x != pe.entry.colour.x || old.colour.y != pe.entry.colour.y ||
			    old.colour.atlas_idx != pe.entry.colour.atlas_idx ||
			    old.colour.w != pe.entry.colour.w || old.colour.h != pe.entry.colour.h) {
				Debug(driver, 0, "GLES: PBO OVERWRITE sprite={} key={:#x} old=({},{} {}x{} p{}) new=({},{} {}x{} p{})",
				      static_cast<SpriteID>(pe.key >> 4), pe.key,
				      old.colour.x, old.colour.y, old.colour.w, old.colour.h, old.colour.atlas_idx,
				      pe.entry.colour.x, pe.entry.colour.y, pe.entry.colour.w, pe.entry.colour.h, pe.entry.colour.atlas_idx);
			}
		}
		this->sprites[pe.key] = pe.entry;
		_gles_perf.gpu_sprites_new++;
		_gles_perf.pbo_sprites_uploaded++;
		if (this->measuring_reload) {
			this->sprites_after_clear++;
		}
	}

	this->pbo_current.entries.clear();
	this->pbo_current.used_bytes = 0;

	auto t1 = std::chrono::steady_clock::now();
	_gles_perf.pbo_submit_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

void GLESSpriteAtlas::ProcessPBOUploads()
{
	/* Skip if PBOs not initialized (e.g. context lost and not yet recovered). */
	if (this->pbo_current.pbo == 0) {
		Debug(driver, 0, "GLES: ProcessPBOUploads SKIP: pbo=0");
		return;
	}

	/* Check previous inflight batch fence (from double-buffer era, kept for safety). */
	PBOCheckInflight();

	/* Re-create placeholder after atlas clear (textures were deleted). */
	if (!this->placeholder_ready && this->colour_array_tex == 0) {
		/* Trigger atlas creation by packing a 1x1 region, then write placeholder pixel. */
		GLESSpriteRegion region;
		if (PackRegion(this->colour_pages, false, 1, 1, region)) {
			uint8_t pink_rgba[4] = { 0, 0, 0, 128 };
			glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, region.x, region.y, region.atlas_idx,
			                1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pink_rgba);
			this->placeholder_entry.colour = region;
			this->placeholder_entry.has_remap = false;
			this->placeholder_entry.palette_only = false;
			if (PackRegion(this->remap_pages, true, 1, 1, this->placeholder_entry.remap)) {
				uint8_t zero_m[1] = { 0 };
				glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
				glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
				                this->placeholder_entry.remap.x, this->placeholder_entry.remap.y,
				                this->placeholder_entry.remap.atlas_idx,
				                1, 1, 1, GL_RED, GL_UNSIGNED_BYTE, zero_m);
			}
			this->placeholder_ready = true;
		}
	}

	/* Time-budgeted upload: process sprites until ~15ms elapsed, then yield to render. */
	auto t_upload_start = std::chrono::steady_clock::now();
	int rounds = 0;
	size_t sprites_before = this->sprites.size();
	for (;;) {
		auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t_upload_start).count();
		if (rounds > 0 && elapsed_us >= PBO_TIME_BUDGET_US) break;

		PBOFillBatch(t_upload_start);
		if (this->pbo_current.entries.empty()) break;
		PBOSubmitBatch();
		rounds++;
	}
	_gles_perf.pbo_rounds += rounds;

	/* One-time atlas overlap check after first batch of uploads completes. */
	static bool overlap_checked = false;
	if (!overlap_checked && this->sprites.size() > 100 && this->sprites.size() > sprites_before) {
		overlap_checked = true;
		/* Build list of colour regions and check for overlaps. */
		struct RegionInfo { GLESSpriteID key; uint16_t x, y, w, h, page; };
		std::vector<RegionInfo> regions;
		for (const auto &[key, entry] : this->sprites) {
			regions.push_back({key, entry.colour.x, entry.colour.y,
			                   entry.colour.w, entry.colour.h, entry.colour.atlas_idx});
		}
		int overlaps = 0;
		for (size_t i = 0; i < regions.size() && overlaps < 20; i++) {
			for (size_t j = i + 1; j < regions.size() && overlaps < 20; j++) {
				const auto &a = regions[i];
				const auto &b = regions[j];
				if (a.page != b.page) continue;
				/* Skip 1x1 placeholder. */
				if ((a.w == 1 && a.h == 1) || (b.w == 1 && b.h == 1)) continue;
				/* AABB overlap test. */
				if (a.x < b.x + b.w && a.x + a.w > b.x &&
				    a.y < b.y + b.h && a.y + a.h > b.y) {
					Debug(driver, 0, "GLES: ATLAS OVERLAP! sprite_a={} ({},{} {}x{} p{}) sprite_b={} ({},{} {}x{} p{})",
					      static_cast<SpriteID>(a.key >> 4), a.x, a.y, a.w, a.h, a.page,
					      static_cast<SpriteID>(b.key >> 4), b.x, b.y, b.w, b.h, b.page);
					overlaps++;
				}
			}
		}
		Debug(driver, 0, "GLES: Atlas overlap check: {} sprites, {} overlaps found", regions.size(), overlaps);
	}
}

GLESAtlasPage &GLESSpriteAtlas::AllocPage(std::vector<GLESAtlasPage> &pages, bool luminance)
{
	GLESAtlasPage page;
	page.width = this->atlas_size;
	page.height = this->atlas_size;
	page.cursor_x = 0;
	page.cursor_y = 0;
	page.row_height = 0;
	pages.push_back(page);

	GLuint &array_tex = luminance ? this->remap_array_tex : this->colour_array_tex;
	int new_depth = static_cast<int>(pages.size());

	/* Create new array texture with one more layer. */
	GLuint new_tex;
	glGenTextures(1, &new_tex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, new_tex);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (luminance) {
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, page.width, page.height, new_depth, 0,
		             GL_RED, GL_UNSIGNED_BYTE, nullptr);
	} else {
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, page.width, page.height, new_depth, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}

	/* Copy existing layers from old array texture via glCopyTexSubImage3D.
	 * Attach each old layer to a read FBO, then copy directly into the
	 * new array texture — no CPU readback, works with any format (R8/RGBA). */
	if (array_tex != 0 && new_depth > 1) {
		GLuint copy_fbo;
		glGenFramebuffers(1, &copy_fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbo);

		for (int i = 0; i < new_depth - 1; i++) {
			glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, array_tex, 0, i);
			GLenum fb_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
			if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
				Debug(driver, 0, "GLES: Atlas copy FBO incomplete: 0x{:04X} layer={} lum={}",
				      fb_status, i, luminance);
				continue;
			}
			glBindTexture(GL_TEXTURE_2D_ARRAY, new_tex);
			glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, 0, 0, page.width, page.height);
			GLenum err = glGetError();
			if (err != GL_NO_ERROR) {
				Debug(driver, 0, "GLES: Atlas glCopyTexSubImage3D error: 0x{:04X} layer={}", err, i);
			}
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &copy_fbo);
		glDeleteTextures(1, &array_tex);
	}

	array_tex = new_tex;

	Debug(driver, 0, "GLES: Allocated {} atlas layer {} ({}x{}) total_layers={}",
	      luminance ? "remap" : "colour", new_depth - 1, page.width, page.height, new_depth);

	return pages.back();
}

bool GLESSpriteAtlas::PackRegion(std::vector<GLESAtlasPage> &pages, bool luminance,
                                 uint16_t w, uint16_t h, GLESSpriteRegion &out)
{
	/* 1px padding to avoid texture bleeding. */
	uint16_t pw = w + 1;
	uint16_t ph = h + 1;

	/* Try to fit in an existing page. */
	for (uint16_t i = 0; i < pages.size(); i++) {
		GLESAtlasPage &page = pages[i];

		if (page.cursor_x + pw <= page.width && page.cursor_y + ph <= page.height) {
			/* Fits in current row. */
			out.atlas_idx = i;
			out.x = page.cursor_x;
			out.y = page.cursor_y;
			out.w = w;
			out.h = h;

			page.cursor_x += pw;
			page.row_height = std::max(page.row_height, ph);

			goto compute_uv;
		}

		/* Try next row. */
		uint16_t next_y = page.cursor_y + page.row_height;
		if (pw <= page.width && next_y + ph <= page.height) {
			out.atlas_idx = i;
			out.x = 0;
			out.y = next_y;
			out.w = w;
			out.h = h;

			page.cursor_x = pw;
			page.cursor_y = next_y;
			page.row_height = ph;

			goto compute_uv;
		}
	}

	/* Need a new page. */
	{
		GLESAtlasPage &page = AllocPage(pages, luminance);
		uint16_t idx = static_cast<uint16_t>(pages.size() - 1);

		if (pw > page.width || ph > page.height) {
			Debug(driver, 0, "GLES: Sprite {}x{} too large for atlas!", w, h);
			return false;
		}

		out.atlas_idx = idx;
		out.x = 0;
		out.y = 0;
		out.w = w;
		out.h = h;

		page.cursor_x = pw;
		page.row_height = ph;
	}

compute_uv:
	{
		float page_w = static_cast<float>(pages[out.atlas_idx].width);
		float page_h = static_cast<float>(pages[out.atlas_idx].height);
		out.u0 = static_cast<float>(out.x) / page_w;
		out.v0 = static_cast<float>(out.y) / page_h;
		out.u1 = static_cast<float>(out.x + out.w) / page_w;
		out.v1 = static_cast<float>(out.y + out.h) / page_h;
	}
	return true;
}

GLESSpriteID GLESSpriteAtlas::Upload(SpriteID sprite_id, ZoomLevel zoom,
                                      const SpriteLoader::CommonPixel *pixels,
                                      uint16_t width, uint16_t height,
                                      bool has_rgb, bool has_remap)
{
	auto t_upload0 = std::chrono::steady_clock::now();
	GLESSpriteID key = MakeGLESSpriteKey(sprite_id, zoom);

	size_t npixels = static_cast<size_t>(width) * height;

	/* Check for existing entry. */
	auto it = this->sprites.find(key);
	if (it != this->sprites.end()) {
		GLESSpriteEntry &existing = it->second;
		if (existing.colour.w == width && existing.colour.h == height &&
		    existing.has_remap == has_remap) {
			/* Same dimensions — re-upload texture data in-place. */
			this->upload_rgba_buf.resize(npixels * 4);
			for (size_t i = 0; i < npixels; i++) {
				this->upload_rgba_buf[i * 4 + 0] = pixels[i].r;
				this->upload_rgba_buf[i * 4 + 1] = pixels[i].g;
				this->upload_rgba_buf[i * 4 + 2] = pixels[i].b;
				this->upload_rgba_buf[i * 4 + 3] = pixels[i].a;
			}
			glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, existing.colour.x, existing.colour.y,
			                existing.colour.atlas_idx, width, height, 1,
			                GL_RGBA, GL_UNSIGNED_BYTE, this->upload_rgba_buf.data());

			if (has_remap) {
				this->upload_m_buf.resize(npixels);
				for (size_t i = 0; i < npixels; i++) {
					this->upload_m_buf[i] = pixels[i].m;
				}
				glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
				glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, existing.remap.x, existing.remap.y,
				                existing.remap.atlas_idx, width, height, 1,
				                GL_RED, GL_UNSIGNED_BYTE, this->upload_m_buf.data());
			}

			existing.palette_only = has_remap && !has_rgb;
			_gles_perf.gpu_sprites_reuploaded++;
			// Debug(driver, 0, "GLES: Atlas REUPLOAD sprite={} zoom={} key=0x{:x} {}x{}",
			//       sprite_id, static_cast<int>(zoom), key, width, height);

			return key;
		}
		/* Different dimensions or remap status changed — discard old entry, re-pack below. */
		Debug(driver, 2, "GLES: Atlas repack sprite={} zoom={} old={}x{} new={}x{}",
		      sprite_id, static_cast<int>(zoom),
		      existing.colour.w, existing.colour.h, width, height);
		this->sprites.erase(it);
		_gles_perf.gpu_sprites_repacked++;
	}

	GLESSpriteEntry entry;
	entry.has_remap = has_remap;
	entry.palette_only = has_remap && !has_rgb;

	/* Upload RGBA data. */
	{
		this->upload_rgba_buf.resize(npixels * 4);
		for (size_t i = 0; i < npixels; i++) {
			this->upload_rgba_buf[i * 4 + 0] = pixels[i].r;
			this->upload_rgba_buf[i * 4 + 1] = pixels[i].g;
			this->upload_rgba_buf[i * 4 + 2] = pixels[i].b;
			this->upload_rgba_buf[i * 4 + 3] = pixels[i].a;
		}

		if (!PackRegion(this->colour_pages, false, width, height, entry.colour)) {
			return key;
		}

		glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, entry.colour.x, entry.colour.y,
		                entry.colour.atlas_idx, width, height, 1,
		                GL_RGBA, GL_UNSIGNED_BYTE, this->upload_rgba_buf.data());
	}

	/* Upload M (remap) channel. */
	if (has_remap) {
		this->upload_m_buf.resize(npixels);
		for (size_t i = 0; i < npixels; i++) {
			this->upload_m_buf[i] = pixels[i].m;
		}

		if (!PackRegion(this->remap_pages, true, width, height, entry.remap)) {
			return key;
		}

		glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, entry.remap.x, entry.remap.y,
		                entry.remap.atlas_idx, width, height, 1,
		                GL_RED, GL_UNSIGNED_BYTE, this->upload_m_buf.data());
	} else {
		/* No remap data; fill with zeros in colour region. */
		entry.remap = entry.colour;
	}

	this->sprites[key] = entry;
	_gles_perf.gpu_sprites_new++;

	if (this->measuring_reload) {
		this->sprites_after_clear++;
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - this->clear_time).count();
		/* Log every 100 sprites and when reload settles. */
		if (this->sprites_after_clear % 100 == 0) {
			Debug(driver, 0, "GLES: Reload progress: {} sprites uploaded in {}ms",
			      this->sprites_after_clear, elapsed);
		}
	}

	_gles_perf.sprite_upload_count++;
	_gles_perf.sprite_upload_us += std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - t_upload0).count();

	return key;
}

GLESSpriteID GLESSpriteAtlas::Enqueue(SpriteID sprite_id, ZoomLevel zoom,
                                       const SpriteLoader::CommonPixel *pixels,
                                       uint16_t width, uint16_t height,
                                       bool has_rgb, bool has_remap)
{
	GLESSpriteID key = MakeGLESSpriteKey(sprite_id, zoom);

	/* Process deferred clear from GL thread. */
	if (this->known_clear_pending.exchange(false)) {
		this->known_sprites.clear();
	}

	/* Skip if sprite is already in the pipeline (queued/PBO/uploaded). */
	if (this->known_sprites.count(sprite_id)) {
		_gles_perf.sprite_cache_hits++;
		return key;
	}
	this->known_sprites.insert(sprite_id);

	auto t0 = std::chrono::steady_clock::now();

	size_t count = static_cast<size_t>(width) * height;

	GLESUploadRequest req;
	req.key = key;
	req.pixels.assign(pixels, pixels + count);
	req.width = width;
	req.height = height;
	req.has_rgb = has_rgb;
	req.has_remap = has_remap;

	{
		std::lock_guard<std::mutex> lock(this->queue_mutex);
		this->upload_queue.push_back(std::move(req));
	}

	auto t1 = std::chrono::steady_clock::now();
	_gles_perf.sprite_stage_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

	return key;
}

const GLESSpriteEntry *GLESSpriteAtlas::LookupOrUpload(GLESSpriteID key)
{
	_gles_perf.lookup_total++;

	/* Fast path: already uploaded. */
	auto it = this->sprites.find(key);
	if (it != this->sprites.end()) {
		_gles_perf.lookup_hits++;
		return &it->second;
	}

	/* Per-frame budget check. */
	if (this->loads_this_frame >= MAX_LOADS_PER_FRAME) {
		_gles_perf.gl_budget_skips++;
		_gles_perf.gpu_sprites_missing++;
		if (this->placeholder_ready) return &this->placeholder_entry;
		return nullptr;
	}

	/* Extract SpriteID from key. */
	SpriteID sprite_id = static_cast<SpriteID>(key >> 4);

	/* Try loading from memory-backed GRF. */
	if (this->LoadSpriteOnGLThread(sprite_id)) {
		this->loads_this_frame++;
		it = this->sprites.find(key);
		if (it != this->sprites.end()) return &it->second;
	} else {
		_gles_perf.gl_thread_fails++;
	}

	_gles_perf.gpu_sprites_missing++;
	if (this->placeholder_ready) return &this->placeholder_entry;
	return nullptr;
}

const GLESSpriteEntry *GLESSpriteAtlas::Lookup(GLESSpriteID key) const
{
	auto it = this->sprites.find(key);
	if (it == this->sprites.end()) return nullptr;
	return &it->second;
}

bool GLESSpriteAtlas::IsKnown(SpriteID id)
{
	/* Process deferred clear from GL thread (ClearSprites / AbandonGLObjects). */
	if (this->known_clear_pending.exchange(false)) {
		this->known_sprites.clear();
	}
	return this->known_sprites.count(id) != 0;
}

void GLESSpriteAtlas::CacheMeta(SpriteID id, int16_t w, int16_t h, int16_t xo, int16_t yo)
{
	this->meta_cache[id] = {w, h, xo, yo};
}

bool GLESSpriteAtlas::GetCachedMeta(SpriteID id, int16_t &w, int16_t &h, int16_t &xo, int16_t &yo) const
{
	auto it = this->meta_cache.find(id);
	if (it == this->meta_cache.end()) return false;
	w = it->second.width;
	h = it->second.height;
	xo = it->second.x_offs;
	yo = it->second.y_offs;
	return true;
}

static int ComputeOccupancy(const std::vector<GLESAtlasPage> &pages)
{
	if (pages.empty()) return 0;
	int64_t used = 0, total = 0;
	for (const auto &p : pages) {
		/* Shelf packer approximation: filled rows + partial current row. */
		used += static_cast<int64_t>(p.cursor_y) * p.width
		      + static_cast<int64_t>(p.cursor_x) * p.row_height;
		total += static_cast<int64_t>(p.width) * p.height;
	}
	return total > 0 ? static_cast<int>(used * 100 / total) : 0;
}

int GLESSpriteAtlas::GetColourOccupancyPercent() const { return ComputeOccupancy(this->colour_pages); }
int GLESSpriteAtlas::GetRemapOccupancyPercent() const { return ComputeOccupancy(this->remap_pages); }

/** Simulate shelf packing for a list of sprite dimensions.
 *  Returns {pages_needed, total_pixels_used}. */
static std::pair<int, int64_t> SimulatePacking(const std::vector<std::pair<uint16_t, uint16_t>> &sizes, uint16_t page_size)
{
	std::vector<GLESAtlasPage> pages;
	int64_t pixels = 0;

	for (const auto &[w, h] : sizes) {
		uint16_t pw = w + 1;
		uint16_t ph = h + 1;
		bool placed = false;

		for (auto &page : pages) {
			if (page.cursor_x + pw <= page.width && page.cursor_y + ph <= page.height) {
				page.cursor_x += pw;
				page.row_height = std::max(page.row_height, ph);
				placed = true;
				break;
			}
			uint16_t next_y = page.cursor_y + page.row_height;
			if (pw <= page.width && next_y + ph <= page.height) {
				page.cursor_x = pw;
				page.cursor_y = next_y;
				page.row_height = ph;
				placed = true;
				break;
			}
		}

		if (!placed) {
			if (pw > page_size || ph > page_size) continue;
			GLESAtlasPage p;
			p.width = page_size;
			p.height = page_size;
			p.cursor_x = pw;
			p.cursor_y = 0;
			p.row_height = ph;
			pages.push_back(p);
		}

		pixels += static_cast<int64_t>(w) * h;
	}

	return {static_cast<int>(pages.size()), pixels};
}

void GLESSpriteAtlas::EstimateAtlasPages() const
{
	auto t0 = std::chrono::steady_clock::now();

	std::vector<std::pair<uint16_t, uint16_t>> colour_sizes;
	std::vector<std::pair<uint16_t, uint16_t>> remap_sizes;
	int64_t staged_bytes = 0;

	{
		std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(this->queue_mutex));
		for (const auto &req : this->upload_queue) {
			colour_sizes.emplace_back(req.width, req.height);
			if (req.has_remap) remap_sizes.emplace_back(req.width, req.height);
			staged_bytes += static_cast<int64_t>(req.pixels.capacity()) * sizeof(SpriteLoader::CommonPixel);
		}
	}

	auto t_collect = std::chrono::steady_clock::now();

	/* Sort largest-first for better packing estimate. */
	auto cmp = [](const auto &a, const auto &b) {
		return std::max(a.first, a.second) > std::max(b.first, b.second);
	};
	std::sort(colour_sizes.begin(), colour_sizes.end(), cmp);
	std::sort(remap_sizes.begin(), remap_sizes.end(), cmp);

	auto t_sort = std::chrono::steady_clock::now();

	auto [col_pages, col_px] = SimulatePacking(colour_sizes, this->atlas_size);
	auto [rem_pages, rem_px] = SimulatePacking(remap_sizes, this->atlas_size);

	auto t_pack = std::chrono::steady_clock::now();

	int64_t page_area = static_cast<int64_t>(this->atlas_size) * this->atlas_size;
	int col_occ = (col_pages > 0 && page_area > 0) ? static_cast<int>(col_px * 100 / (page_area * col_pages)) : 0;
	int rem_occ = (rem_pages > 0 && page_area > 0) ? static_cast<int>(rem_px * 100 / (page_area * rem_pages)) : 0;

	int col_mb = static_cast<int>(page_area * col_pages * 4 / (1024 * 1024));
	int rem_mb = static_cast<int>(page_area * rem_pages * 1 / (1024 * 1024));

	int staged_mb = static_cast<int>(staged_bytes / (1024 * 1024));

	auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };

	Debug(driver, 0, "ATLAS_ESTIMATE: {} unique sprites ({} with remap) atlas={}x{}",
	      colour_sizes.size(), remap_sizes.size(), this->atlas_size, this->atlas_size);
	Debug(driver, 0, "ATLAS_ESTIMATE: colour: {} pages ({}% occ, {}MB) | remap: {} pages ({}% occ, {}MB) | gpu_total: {}MB | staged_ram: {}MB",
	      col_pages, col_occ, col_mb, rem_pages, rem_occ, rem_mb, col_mb + rem_mb, staged_mb);
	Debug(driver, 0, "ATLAS_ESTIMATE: timing: collect={}us sort={}us pack={}us total={}us",
	      us(t0, t_collect), us(t_collect, t_sort), us(t_sort, t_pack), us(t0, t_pack));
}

/**
 * Build GL-thread-owned memory-backed SpriteFile copies.
 * Must be called after BufferSpriteFilesToMemory() and before GL-thread sprite loading.
 */
void GLESSpriteAtlas::BuildGLSpriteFiles()
{
	this->gl_sprite_files.clear();
	for (const auto &f : GetCachedSpriteFiles()) {
		if (f->GetMemoryData() == nullptr) continue;
		auto gl_file = std::make_unique<SpriteFile>(
			f->GetMemoryData(), f->GetMemorySize(), f->GetFilename(),
			f->NeedsPaletteRemap(), f->GetContainerVersion(),
			f->GetContentBegin(), f->GetStartPos());
		this->gl_sprite_files[f.get()] = std::move(gl_file);
	}
	Debug(sprite, 0, "BuildGLSpriteFiles: {} files", this->gl_sprite_files.size());
}

/**
 * Decode a sprite from memory-backed GRF and upload directly to atlas.
 * GL thread only. Returns true on success.
 */
bool GLESSpriteAtlas::LoadSpriteOnGLThread(SpriteID sprite_id)
{
	SpriteCacheInfo info;
	if (!GetSpriteCacheInfo(sprite_id, info)) {
		Debug(sprite, 3, "GL LoadSprite {}: no cache info", sprite_id);
		return false;
	}
	if (info.type != SpriteType::Normal) {
		Debug(sprite, 3, "GL LoadSprite {}: type={}", sprite_id, static_cast<int>(info.type));
		return false;
	}

	/* Find GL-thread SpriteFile copy. */
	auto it = this->gl_sprite_files.find(info.file);
	if (it == this->gl_sprite_files.end()) {
		Debug(sprite, 0, "GL LoadSprite {}: no GL file for {}", sprite_id, info.file->GetFilename());
		return false;
	}
	SpriteFile &gl_file = *it->second;

	/* Decode sprite. */
	SpriteLoader::SpriteCollection sprite;
	ZoomLevels sprite_avail;
	ZoomLevels avail_8bpp;
	ZoomLevels avail_32bpp;

	SpriteLoaderGrf sprite_loader(gl_file.GetContainerVersion());
	sprite_avail = sprite_loader.LoadSprite(sprite, gl_file, info.file_pos, info.type, true, info.control_flags, avail_8bpp, avail_32bpp);
	if (sprite_avail.None()) {
		sprite_avail = sprite_loader.LoadSprite(sprite, gl_file, info.file_pos, info.type, false, info.control_flags, avail_8bpp, avail_32bpp);
	}
	if (sprite_avail.None()) {
		Debug(sprite, 0, "GL LoadSprite {}: decode failed, file={} pos={}", sprite_id, gl_file.GetFilename(), info.file_pos);
		return false;
	}

	/* Find best available zoom. Prefer base zoom, fall back to any. */
	ZoomLevel use_zoom = kGPUScaleBaseZoom;
	if (!sprite_avail.Test(use_zoom)) {
		for (ZoomLevel z = ZoomLevel::Min; z <= ZoomLevel::Max; z++) {
			if (sprite_avail.Test(z)) { use_zoom = z; break; }
		}
	}

	const auto &sl = sprite[use_zoom];
	if (sl.data == nullptr || sl.width == 0 || sl.height == 0) return false;

	uint16_t w = sl.width;
	uint16_t h = sl.height;
	size_t count = static_cast<size_t>(w) * h;

	/* Determine channels from sprite component flags (matches Blitter_Snapshot::Encode). */
	bool has_rgb = sl.colours.Test(SpriteComponent::RGB) || sl.colours.Test(SpriteComponent::Alpha);
	bool has_remap = sl.colours.Test(SpriteComponent::Palette);
	if (!has_rgb && !has_remap) {
		/* Fallback: scan pixel data. */
		for (size_t i = 0; i < count; i++) {
			if (sl.data[i].r || sl.data[i].g || sl.data[i].b) has_rgb = true;
			if (sl.data[i].m) has_remap = true;
			if (has_rgb && has_remap) break;
		}
		if (!has_rgb && !has_remap) has_rgb = true;
	}

	/* Upload directly. */
	this->Upload(sprite_id, kGPUScaleBaseZoom, sl.data, w, h, has_rgb, has_remap);

	/* Cache meta for fast-path. */
	const auto &root = sprite.Root();
	this->CacheMeta(sprite_id, root.width, root.height, root.x_offs, root.y_offs);

	_gles_perf.gl_thread_loads++;
	return true;
}
