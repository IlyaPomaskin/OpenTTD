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
#include <GLES2/gl2.h>
#include <algorithm>

#include "../safeguards.h"

void GLESSpriteAtlas::Init()
{
	/* Query maximum texture size, clamp to 4096 for reasonable atlas pages. */
	GLint max_size = 2048;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	this->atlas_size = static_cast<uint16_t>(std::min(max_size, (GLint)4096));

	/* Tightly-packed rows for all texture uploads (critical for GL_LUMINANCE
	 * where row byte count may not be a multiple of the default alignment 4). */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	Debug(driver, 1, "GLES: Atlas page size {}x{}", this->atlas_size, this->atlas_size);
}

void GLESSpriteAtlas::Destroy()
{
	for (auto &page : this->colour_pages) {
		if (page.texture != 0) glDeleteTextures(1, &page.texture);
	}
	for (auto &page : this->remap_pages) {
		if (page.texture != 0) glDeleteTextures(1, &page.texture);
	}
	this->colour_pages.clear();
	this->remap_pages.clear();
	this->sprites.clear();
}

GLESAtlasPage &GLESSpriteAtlas::AllocPage(std::vector<GLESAtlasPage> &pages, bool luminance)
{
	GLESAtlasPage page;
	page.width = this->atlas_size;
	page.height = this->atlas_size;
	page.cursor_x = 0;
	page.cursor_y = 0;
	page.row_height = 0;
	page.is_luminance = luminance;

	glGenTextures(1, &page.texture);
	glBindTexture(GL_TEXTURE_2D, page.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Allocate texture storage with null data. */
	if (luminance) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, page.width, page.height, 0,
		             GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, page.width, page.height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}

	pages.push_back(page);

	Debug(driver, 0, "GLES: Allocated {} atlas page {} ({}x{}) total_pages={}",
	      luminance ? "remap" : "colour", pages.size() - 1, page.width, page.height, pages.size());

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

	/* Check for existing entry. */
	auto it = this->sprites.find(key);
	if (it != this->sprites.end()) {
		GLESSpriteEntry &existing = it->second;
		if (existing.colour.w == width && existing.colour.h == height &&
		    existing.has_remap == has_remap) {
			/* Same dimensions — re-upload texture data in-place. */
			std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
			for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
				rgba[i * 4 + 0] = pixels[i].r;
				rgba[i * 4 + 1] = pixels[i].g;
				rgba[i * 4 + 2] = pixels[i].b;
				rgba[i * 4 + 3] = pixels[i].a;
			}
			glBindTexture(GL_TEXTURE_2D, this->colour_pages[existing.colour.atlas_idx].texture);
			glTexSubImage2D(GL_TEXTURE_2D, 0, existing.colour.x, existing.colour.y,
			                width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

			if (has_remap) {
				std::vector<uint8_t> m_data(static_cast<size_t>(width) * height);
				for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
					m_data[i] = pixels[i].m;
				}
				glBindTexture(GL_TEXTURE_2D, this->remap_pages[existing.remap.atlas_idx].texture);
				glTexSubImage2D(GL_TEXTURE_2D, 0, existing.remap.x, existing.remap.y,
				                width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_data.data());
			}

			existing.palette_only = has_remap && !has_rgb;
			_gles_perf.gpu_sprites_reuploaded++;
			return key;
		}
		/* Different dimensions or remap status changed — discard old entry, re-pack below. */
		this->sprites.erase(it);
	}

	GLESSpriteEntry entry;
	entry.has_remap = has_remap;
	entry.palette_only = has_remap && !has_rgb;

	/* Upload RGBA data. */
	{
		std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
			rgba[i * 4 + 0] = pixels[i].r;
			rgba[i * 4 + 1] = pixels[i].g;
			rgba[i * 4 + 2] = pixels[i].b;
			rgba[i * 4 + 3] = pixels[i].a;
		}

		if (!PackRegion(this->colour_pages, false, width, height, entry.colour)) {
			return key;
		}

		glBindTexture(GL_TEXTURE_2D, this->colour_pages[entry.colour.atlas_idx].texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, entry.colour.x, entry.colour.y,
		                width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	}

	/* Upload M (remap) channel. */
	if (has_remap) {
		std::vector<uint8_t> m_data(static_cast<size_t>(width) * height);
		for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
			m_data[i] = pixels[i].m;
		}

		if (!PackRegion(this->remap_pages, true, width, height, entry.remap)) {
			return key;
		}

		glBindTexture(GL_TEXTURE_2D, this->remap_pages[entry.remap.atlas_idx].texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, entry.remap.x, entry.remap.y,
		                width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_data.data());
	} else {
		/* No remap data; fill with zeros in colour region. */
		entry.remap = entry.colour;
	}

	this->sprites[key] = entry;

	return key;
}

const GLESSpriteEntry *GLESSpriteAtlas::Lookup(GLESSpriteID key) const
{
	auto it = this->sprites.find(key);
	if (it == this->sprites.end()) return nullptr;
	return &it->second;
}
