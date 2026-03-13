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
#include <GLES3/gl3.h>
#include <algorithm>

#include "../safeguards.h"

void GLESSpriteAtlas::Init()
{
	/* Query maximum texture size, clamp to 4096 for reasonable atlas pages. */
	GLint max_size = 2048;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	this->atlas_size = static_cast<uint16_t>(std::min(max_size, (GLint)4096));

	/* Tightly-packed rows for all texture uploads (critical for GL_RED/R8
	 * where row byte count may not be a multiple of the default alignment 4). */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	Debug(driver, 1, "GLES: Atlas page size {}x{}", this->atlas_size, this->atlas_size);
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
}

void GLESSpriteAtlas::ProcessPendingClear()
{
	/* Check if sprite reload measurement is complete (no new sprites since last check). */
	if (this->measuring_reload && _gles_perf.gpu_sprites_new == 0 && this->sprites_after_clear > 0) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - this->clear_time).count();
		Debug(driver, 0, "GLES: Reload complete: {} sprites in {}ms",
		      this->sprites_after_clear, elapsed);
		this->measuring_reload = false;
	}

	if (!this->clear_pending.exchange(false)) return;
	this->ClearSprites();
}

void GLESSpriteAtlas::ClearSprites()
{
	/* Delete GPU textures and reset atlas layout.
	 * Keep stored_pixels and staged — they may already contain data for the
	 * new map (staged from the game thread between RequestClear and this call).
	 * LookupOrUpload will re-upload them on demand. */
	if (this->colour_array_tex != 0) glDeleteTextures(1, &this->colour_array_tex);
	if (this->remap_array_tex != 0) glDeleteTextures(1, &this->remap_array_tex);
	this->colour_array_tex = 0;
	this->remap_array_tex = 0;

	size_t old_pages = this->colour_pages.size() + this->remap_pages.size();
	size_t old_sprites = this->sprites.size();

	this->colour_pages.clear();
	this->remap_pages.clear();
	this->sprites.clear();

	this->clear_time = std::chrono::steady_clock::now();
	this->sprites_after_clear = 0;
	this->measuring_reload = true;
	Debug(driver, 0, "GLES: Atlas ClearSprites: deleted {} pages, {} gpu entries, {} staged",
	      old_pages, old_sprites, this->staged.size());
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

	return key;
}

GLESSpriteID GLESSpriteAtlas::Stage(SpriteID sprite_id, ZoomLevel zoom,
                                     const SpriteLoader::CommonPixel *pixels,
                                     uint16_t width, uint16_t height,
                                     bool has_rgb, bool has_remap)
{
	GLESSpriteID key = MakeGLESSpriteKey(sprite_id, zoom);
	size_t count = static_cast<size_t>(width) * height;

	GLESStagedPixels sp;
	sp.pixels.assign(pixels, pixels + count);
	sp.width = width;
	sp.height = height;
	sp.has_rgb = has_rgb;
	sp.has_remap = has_remap;

	std::lock_guard<std::mutex> lock(this->staged_mutex);
	this->staged[key] = std::move(sp);
	return key;
}

const GLESSpriteEntry *GLESSpriteAtlas::LookupOrUpload(GLESSpriteID key)
{
	/* Fast path: already uploaded to GPU. */
	auto it = this->sprites.find(key);
	if (it != this->sprites.end()) return &it->second;

	/* Check staged data (written by game thread via Stage()).
	 * Copy instead of move — keep staged data so it survives ClearSprites()
	 * and can be re-uploaded if the atlas is cleared during map switches.
	 * Mutex required in snapshot mode: Stage() runs on game thread
	 * concurrently with LookupOrUpload on the GPU thread. */
	GLESStagedPixels sp;
	{
		std::lock_guard<std::mutex> lock(this->staged_mutex);
		auto sit = this->staged.find(key);
		if (sit == this->staged.end()) return nullptr;
		sp = sit->second;
	}

	/* Upload to GPU (we're on the GL thread). */
	SpriteID sprite_id = static_cast<SpriteID>(key >> 4);
	ZoomLevel zoom = static_cast<ZoomLevel>(key & 0xF);
	this->Upload(sprite_id, zoom, sp.pixels.data(), sp.width, sp.height, sp.has_rgb, sp.has_remap);

	it = this->sprites.find(key);
	if (it != this->sprites.end()) return &it->second;
	return nullptr;
}

const GLESSpriteEntry *GLESSpriteAtlas::Lookup(GLESSpriteID key) const
{
	auto it = this->sprites.find(key);
	if (it == this->sprites.end()) return nullptr;
	return &it->second;
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
