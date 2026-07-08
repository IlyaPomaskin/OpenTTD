/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_sprite.h Sprite atlas types for OpenGL ES backend. */

#ifndef VIDEO_GLES_SPRITE_H
#define VIDEO_GLES_SPRITE_H

#include <GLES3/gl3.h>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <string>
#include "../spriteloader/spriteloader.hpp"
#include "../spriteloader/sprite_file_type.hpp"
#include "../zoom_type.h"
#include "../gfx_func.h"

/** Unique key for a sprite at a specific zoom level. */
using GLESSpriteID = uint64_t;

/** Base zoom level used for GPU-scaled rendering.
 *  Paint() looks up this zoom variant and scales UV coordinates for other zoom levels. */
static constexpr ZoomLevel kGPUScaleBaseZoom = ZoomLevel::In4x;

/** Make a sprite key from SpriteID. Zoom is always base zoom (GPU scaling). */
inline GLESSpriteID MakeGLESSpriteKey(SpriteID sprite_id, [[maybe_unused]] ZoomLevel zoom)
{
	return (static_cast<uint64_t>(sprite_id) << 4) | static_cast<uint64_t>(kGPUScaleBaseZoom);
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

/** A single layer in an atlas texture array, with shelf packer state. */
struct GLESAtlasPage {
	uint16_t width = 0;       ///< Atlas width in pixels.
	uint16_t height = 0;      ///< Atlas height in pixels.
	uint16_t cursor_x = 0;    ///< Current packing cursor X.
	uint16_t cursor_y = 0;    ///< Current packing cursor Y.
	uint16_t row_height = 0;  ///< Height of the current shelf row.
};

/** Manages sprite atlas textures for the GLES backend. */
class GLESSpriteAtlas {
private:
	std::vector<GLESAtlasPage> colour_pages; ///< RGBA atlas layers (packing state).
	std::vector<GLESAtlasPage> remap_pages;  ///< Remap atlas layers (packing state).
	GLuint colour_array_tex = 0;             ///< GL_TEXTURE_2D_ARRAY for colour (RGBA).
	GLuint remap_array_tex = 0;              ///< GL_TEXTURE_2D_ARRAY for remap (R8).
	int colour_array_depth = 0;              ///< Allocated depth of colour array texture.
	int remap_array_depth = 0;               ///< Allocated depth of remap array texture.
	static constexpr int MAX_ATLAS_LAYERS = 4; ///< Fixed layer count, allocated up front.
	uint16_t atlas_size = 2048;              ///< Atlas page dimension.

	std::unordered_map<GLESSpriteID, GLESSpriteEntry> sprites; ///< All uploaded sprites.

	std::vector<uint8_t> upload_rgba_buf; ///< Reusable buffer for RGBA pixel conversion.
	std::vector<uint8_t> upload_m_buf;    ///< Reusable buffer for M channel extraction.

	GLESSpriteEntry placeholder_entry{}; ///< 1x1 semi-transparent black sprite for missing sprites.
	bool placeholder_ready = false;

	/** GL-thread-owned memory-backed SpriteFile copies, keyed by original SpriteFile pointer. */
	std::unordered_map<const SpriteFile *, std::unique_ptr<SpriteFile>> gl_sprite_files;
	static constexpr int MAX_LOADS_PER_FRAME = 500; ///< Per-frame budget for on-demand sprite loads.
	int loads_this_frame = 0;

	/** Post-clear sprite load monitoring (GL thread only). */
	int post_clear_frames = -1;      ///< Frames since last clear (-1 = not monitoring).
	int post_clear_zero_streak = 0;  ///< Consecutive frames with 0 new sprites.

	void AllocLayers();
	GLESAtlasPage &AllocPage(std::vector<GLESAtlasPage> &pages, bool luminance);
	bool PackRegion(std::vector<GLESAtlasPage> &pages, bool luminance,
	                uint16_t w, uint16_t h, GLESSpriteRegion &out);

public:
	void Init();
	void Destroy();
	void DeleteGLObjects();

	/** Request atlas clear (thread-safe, deferred to GL thread). */
	void RequestClear() { this->clear_pending.store(true); }

	/** True while per-frame sprite load logging is active (after a clear). */
	bool IsPostClearMonitoring() const { return this->post_clear_frames >= 0; }
	/** Advance post-clear frame counter. Returns frame index, or -1 when stable. */
	int TickPostClearFrame(int new_sprites_this_frame);

	/** Abandon GL handles without deleting (after EGL context loss). */
	void AbandonGLObjects() {
		this->colour_array_tex = 0;
		this->remap_array_tex = 0;
		this->colour_array_depth = 0;
		this->remap_array_depth = 0;
		this->colour_pages.clear();
		this->remap_pages.clear();
		this->sprites.clear();
		this->placeholder_ready = false;
	}

	/** Process deferred clear. Must be called from GL thread (e.g. in Paint). */
	void ProcessPendingClear();

private:
	std::atomic<bool> clear_pending{false};
	void ClearSprites();

public:
	/** Upload a sprite to the atlas. Must be called from the GL thread. */
	GLESSpriteID Upload(SpriteID sprite_id, ZoomLevel zoom,
	                    const SpriteLoader::CommonPixel *pixels,
	                    uint16_t width, uint16_t height,
	                    bool has_rgb, bool has_remap);

	/** Look up a sprite; decode from memory and upload if not yet present. GL thread only. */
	const GLESSpriteEntry *LookupOrUpload(GLESSpriteID key);

	/** Decode sprite from memory-backed GRF and upload to atlas. GL thread only. */
	bool LoadSpriteOnGLThread(SpriteID sprite_id);

	/** Build GL-thread SpriteFile copies from buffered memory. Call after BufferSpriteFilesToMemory(). */
	void BuildGLSpriteFiles();

	/** Reset per-frame load counter. Call at start of each frame. */
	void ResetFrameLoadCounter() { this->loads_this_frame = 0; }

	/** Look up a previously uploaded sprite. Returns nullptr if not found. */
	const GLESSpriteEntry *Lookup(GLESSpriteID key) const;

	/** Get the GL_TEXTURE_2D_ARRAY handle for the colour atlas. */
	GLuint GetColourTexture() const { return colour_array_tex; }

	/** Get the GL_TEXTURE_2D_ARRAY handle for the remap atlas. */
	GLuint GetRemapTexture() const { return remap_array_tex; }

	/** Get atlas page counts for diagnostics. */
	size_t GetColourPageCount() const { return colour_pages.size(); }
	size_t GetRemapPageCount() const { return remap_pages.size(); }
	size_t GetSpriteCount() const { return sprites.size(); }

	/** Get atlas occupancy as approximate percentage (0-100). */
	int GetColourOccupancyPercent() const;
	int GetRemapOccupancyPercent() const;

	/** Debug: dump every atlas layer to PNG + a per-atlas JSON of sprite regions
	 *  into `dir`. GL thread only (reads textures back via glReadPixels). */
	void DumpToFiles(const std::string &dir);

};

#endif /* VIDEO_GLES_SPRITE_H */
