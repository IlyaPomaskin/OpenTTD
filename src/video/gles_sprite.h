/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_sprite.h Sprite atlas types for OpenGL ES backend. */

#ifndef VIDEO_GLES_SPRITE_H
#define VIDEO_GLES_SPRITE_H

#include <GLES2/gl2.h>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include "../spriteloader/spriteloader.hpp"
#include "../zoom_type.h"
#include "../gfx_func.h"

/** Unique key for a sprite at a specific zoom level. */
using GLESSpriteID = uint64_t;

/** Make a sprite key from SpriteID and zoom level. */
inline GLESSpriteID MakeGLESSpriteKey(SpriteID sprite_id, ZoomLevel zoom)
{
	return (static_cast<uint64_t>(sprite_id) << 4) | static_cast<uint64_t>(zoom);
}

/** A region within a texture atlas. */
struct GLESSpriteRegion {
	uint16_t atlas_idx;   ///< Index of the atlas texture.
	uint16_t x, y;        ///< Top-left position in the atlas (pixels).
	uint16_t w, h;        ///< Dimensions in the atlas (pixels).
	float u0, v0, u1, v1; ///< Pre-computed UV coordinates.
};

/** An uploaded sprite with both RGBA and remap regions. */
struct GLESSpriteEntry {
	GLESSpriteRegion colour;  ///< Region in the colour (RGBA) atlas.
	GLESSpriteRegion remap;   ///< Region in the remap (M channel) atlas.
	bool has_remap;           ///< Whether this sprite has remap data.
	bool palette_only;        ///< True if sprite has only M channel (no RGB data).
};

/** A single atlas texture with a shelf packer. */
struct GLESAtlasPage {
	GLuint texture = 0;       ///< GL texture handle.
	uint16_t width = 0;       ///< Atlas width in pixels.
	uint16_t height = 0;      ///< Atlas height in pixels.
	uint16_t cursor_x = 0;    ///< Current packing cursor X.
	uint16_t cursor_y = 0;    ///< Current packing cursor Y.
	uint16_t row_height = 0;  ///< Height of the current shelf row.
	bool is_luminance;        ///< True for remap atlas (GL_LUMINANCE).
};

/** Deferred sprite upload queued from a non-GL thread. */
struct GLESPendingUpload {
	SpriteID sprite_id;
	ZoomLevel zoom;
	uint16_t width;
	uint16_t height;
	bool has_rgb;
	bool has_remap;
	std::vector<SpriteLoader::CommonPixel> pixels; ///< Copied pixel data.
};

/** Manages sprite atlas textures for the GLES backend. */
class GLESSpriteAtlas {
private:
	std::vector<GLESAtlasPage> colour_pages; ///< RGBA atlas pages.
	std::vector<GLESAtlasPage> remap_pages;  ///< Luminance (M channel) atlas pages.
	uint16_t atlas_size = 2048;              ///< Atlas page dimension.

	std::unordered_map<GLESSpriteID, GLESSpriteEntry> sprites; ///< All uploaded sprites.
	std::thread::id gl_thread_id; ///< Thread that owns the GL context.

	std::mutex pending_mutex;                      ///< Guards pending_uploads.
	std::vector<GLESPendingUpload> pending_uploads; ///< Uploads deferred to the GL thread.

	GLESAtlasPage &AllocPage(std::vector<GLESAtlasPage> &pages, bool luminance);
	bool PackRegion(std::vector<GLESAtlasPage> &pages, bool luminance,
	                uint16_t w, uint16_t h, GLESSpriteRegion &out);

	/** Actually perform GL upload (must be on GL thread). */
	void DoUpload(SpriteID sprite_id, ZoomLevel zoom,
	              const SpriteLoader::CommonPixel *pixels,
	              uint16_t width, uint16_t height,
	              bool has_rgb, bool has_remap);

public:
	void Init();
	void Destroy();

	/** Upload a sprite — queues if not on GL thread. */
	GLESSpriteID Upload(SpriteID sprite_id, ZoomLevel zoom,
	                    const SpriteLoader::CommonPixel *pixels,
	                    uint16_t width, uint16_t height,
	                    bool has_rgb, bool has_remap);

	/** Process pending uploads from non-GL threads. Call from GL thread before rendering. */
	void FlushPendingUploads();

	/** Look up a previously uploaded sprite. Returns nullptr if not found. */
	const GLESSpriteEntry *Lookup(GLESSpriteID key) const;

	/** Get the GL texture handle for a colour atlas page. */
	GLuint GetColourTexture(uint16_t idx) const { return colour_pages[idx].texture; }

	/** Get the GL texture handle for a remap atlas page. */
	GLuint GetRemapTexture(uint16_t idx) const { return remap_pages[idx].texture; }

	/** Get atlas page counts for diagnostics. */
	size_t GetColourPageCount() const { return colour_pages.size(); }
	size_t GetRemapPageCount() const { return remap_pages.size(); }
	size_t GetSpriteCount() const { return sprites.size(); }
};

#endif /* VIDEO_GLES_SPRITE_H */
