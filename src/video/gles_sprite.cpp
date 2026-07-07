/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_sprite.cpp Sprite atlas packer and GL-thread on-demand sprite decode for OpenGL ES. */

#include "../stdafx.h"
#include "gles_sprite.h"
#include "gles_perf.h"
#include "../debug.h"
#include "../gfx_func.h"
#include "../spritecache.h"
#include "../spritecache_internal.h"
#include "../spriteloader/grf.hpp"
#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>

#include "../safeguards.h"

void GLESSpriteAtlas::AllocLayers()
{
	glGenTextures(1, &this->colour_array_tex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, this->colour_array_tex);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, this->atlas_size, this->atlas_size, MAX_ATLAS_LAYERS, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	this->colour_array_depth = MAX_ATLAS_LAYERS;

	glGenTextures(1, &this->remap_array_tex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, this->remap_array_tex);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, this->atlas_size, this->atlas_size, MAX_ATLAS_LAYERS, 0,
	             GL_RED, GL_UNSIGNED_BYTE, nullptr);
	this->remap_array_depth = MAX_ATLAS_LAYERS;

	Debug(driver, 0, "GLES: Allocated atlas array textures {}x{}x{} (colour+remap)",
	      this->atlas_size, this->atlas_size, MAX_ATLAS_LAYERS);
}

void GLESSpriteAtlas::Init()
{
	/* Query maximum texture size, clamp to 4096 for reasonable atlas pages.
	 * Also query GL_MAX_3D_TEXTURE_SIZE because some devices validate
	 * glTexImage3D for GL_TEXTURE_2D_ARRAY against it. */
	GLint max_size = 2048, max_3d_size = 2048;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_3d_size);
	this->atlas_size = static_cast<uint16_t>(std::min({max_size, max_3d_size, (GLint)4096}));

	/* Tightly-packed rows for all texture uploads (critical for GL_RED/R8
	 * where row byte count may not be a multiple of the default alignment 4). */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* Array textures are allocated up front at MAX_ATLAS_LAYERS depth, so packing
	 * a new page is pure bookkeeping (see AllocPage) — no per-layer GL realloc. */
	this->AllocLayers();

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

	Debug(driver, 0, "GLES: Atlas page size {}x{} (max_tex={} max_3d={}), max_layers={}",
	      this->atlas_size, this->atlas_size, max_size, max_3d_size, MAX_ATLAS_LAYERS);
}

void GLESSpriteAtlas::DeleteGLObjects()
{
	if (this->colour_array_tex != 0) glDeleteTextures(1, &this->colour_array_tex);
	if (this->remap_array_tex != 0) glDeleteTextures(1, &this->remap_array_tex);
	this->colour_array_tex = 0;
	this->remap_array_tex = 0;
	this->colour_array_depth = 0;
	this->remap_array_depth = 0;
	this->colour_pages.clear();
	this->remap_pages.clear();
	this->sprites.clear();
	this->placeholder_ready = false;
}

void GLESSpriteAtlas::Destroy()
{
	this->DeleteGLObjects();
}

int GLESSpriteAtlas::TickPostClearFrame(int new_sprites_this_frame)
{
	if (this->post_clear_frames < 0) return -2; /* not monitoring */
	int frame = this->post_clear_frames++;
	if (new_sprites_this_frame == 0) {
		if (++this->post_clear_zero_streak >= 5) {
			this->post_clear_frames = -1; /* stop monitoring */
			return -1; /* signal: stable */
		}
	} else {
		this->post_clear_zero_streak = 0;
	}
	return frame;
}

void GLESSpriteAtlas::ProcessPendingClear()
{
	if (!this->clear_pending.exchange(false)) return;
	this->ClearSprites();
}

void GLESSpriteAtlas::ClearSprites()
{
	size_t old_pages = this->colour_pages.size() + this->remap_pages.size();
	size_t old_sprites = this->sprites.size();

	/* Delete + realloc: no emulator gfxstream workaround needed on real devices.
	 * Atlas growth is also handled this way — MAX_ATLAS_LAYERS layers are
	 * pre-allocated by Init(), so there is no incremental growth to preserve. */
	this->DeleteGLObjects();
	this->Init();

	/* Reset per-frame monitoring; on-demand reload repopulates from here. */
	this->post_clear_frames = 0;
	this->post_clear_zero_streak = 0;

	Debug(driver, 0, "GLES: sprites_clear: deleted_pages={} deleted_entries={}", old_pages, old_sprites);

	/* Rebuild GL-thread sprite file copies (GRF files may have changed). */
	this->BuildGLSpriteFiles();
}

GLESAtlasPage &GLESSpriteAtlas::AllocPage(std::vector<GLESAtlasPage> &pages, bool luminance)
{
	/* Array textures are pre-allocated at MAX_ATLAS_LAYERS depth (see AllocLayers),
	 * so adding a page is pure bookkeeping — no GL calls, nothing to copy. */
	GLESAtlasPage page;
	page.width = this->atlas_size;
	page.height = this->atlas_size;
	pages.push_back(page);

	Debug(driver, 0, "GLES: Allocated {} atlas layer {} ({}x{})",
	      luminance ? "remap" : "colour", pages.size() - 1, page.width, page.height);

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
		if (static_cast<int>(pages.size()) >= MAX_ATLAS_LAYERS) {
			Debug(driver, 0, "GLES: Atlas full ({} layers max), triggering clear to evict stale sprites lum={}",
			      MAX_ATLAS_LAYERS, luminance);
			this->clear_pending.store(true);
			return false;
		}
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
	[[maybe_unused]] auto t_upload0 = std::chrono::steady_clock::now();
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
			GLES_PERF_COUNT(_gles_perf.gpu_sprites_reuploaded++);

			return key;
		}
		/* Different dimensions or remap status changed — discard old entry, re-pack below. */
		Debug(driver, 2, "GLES: Atlas repack sprite={} zoom={} old={}x{} new={}x{}",
		      sprite_id, static_cast<int>(zoom),
		      existing.colour.w, existing.colour.h, width, height);
		this->sprites.erase(it);
		GLES_PERF_COUNT(_gles_perf.gpu_sprites_repacked++);
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
		/* No remap data; reuse colour region so callers always have a valid rect. */
		entry.remap = entry.colour;
	}

	this->sprites[key] = entry;
	GLES_PERF_COUNT(_gles_perf.gpu_sprites_new++);
	GLES_PERF_COUNT(_gles_perf.sprite_upload_count++);
	GLES_PERF_COUNT(_gles_perf.sprite_upload_us += std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - t_upload0).count());

	return key;
}

const GLESSpriteEntry *GLESSpriteAtlas::LookupOrUpload(GLESSpriteID key)
{
	GLES_PERF_COUNT(_gles_perf.lookup_total++);

	/* Fast path: already uploaded. */
	auto it = this->sprites.find(key);
	if (it != this->sprites.end()) {
		GLES_PERF_COUNT(_gles_perf.lookup_hits++);
		return &it->second;
	}

	/* Per-frame budget check. */
	if (this->loads_this_frame >= MAX_LOADS_PER_FRAME) {
		GLES_PERF_COUNT(_gles_perf.gl_budget_skips++);
		GLES_PERF_COUNT(_gles_perf.gpu_sprites_missing++);
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
		GLES_PERF_COUNT(_gles_perf.gl_thread_fails++);
	}

	GLES_PERF_COUNT(_gles_perf.gpu_sprites_missing++);
	if (this->placeholder_ready) return &this->placeholder_entry;
	return nullptr;
}

const GLESSpriteEntry *GLESSpriteAtlas::Lookup(GLESSpriteID key) const
{
	auto it = this->sprites.find(key);
	if (it == this->sprites.end()) return nullptr;
	return &it->second;
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

	GLES_PERF_COUNT(_gles_perf.gl_thread_loads++);
	return true;
}
