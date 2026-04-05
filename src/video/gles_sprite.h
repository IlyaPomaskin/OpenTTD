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
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <mutex>
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

/** Entry tracking a single sprite in a PBO upload batch. */
struct PBOPendingEntry {
	GLESSpriteID key;
	GLESSpriteEntry entry;       ///< Pre-packed atlas regions (colour + remap).
	size_t colour_offset;        ///< Byte offset in PBO for RGBA data.
	size_t remap_offset;         ///< Byte offset in PBO for M channel data.
	uint16_t width, height;
	bool has_remap;
};

/** A batch of sprites uploaded via PBO, pending GPU fence. */
struct PBOUploadBatch {
	GLuint pbo = 0;
	GLsync fence = nullptr;
	std::vector<PBOPendingEntry> entries;
	size_t used_bytes = 0;
};

/** Pixel data queued for GPU upload. */
struct GLESUploadRequest {
	GLESSpriteID key;
	std::vector<SpriteLoader::CommonPixel> pixels;
	uint16_t width, height;
	bool has_rgb, has_remap;
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
	static constexpr int MAX_ATLAS_LAYERS = 4; ///< Pre-allocated layer count.
	uint16_t atlas_size = 2048;              ///< Atlas page dimension.

	std::unordered_map<GLESSpriteID, GLESSpriteEntry> sprites; ///< All uploaded sprites.

	std::vector<GLESUploadRequest> upload_queue; ///< Sprites queued for GPU upload.
	std::mutex queue_mutex; ///< Protects upload_queue (game thread pushes, GL thread drains).

	/** Sprites known to be in the pipeline (queued, PBO, or uploaded).
	 *  Game thread only — GL thread signals clear via atomic flag. */
	std::unordered_set<SpriteID> known_sprites;
	std::atomic<bool> known_clear_pending{false}; ///< GL thread requests clear.

	/** Cached Sprite root dimensions for ReadSprite fast-path.
	 *  Never cleared — dimensions are constant per SpriteID. Game thread only. */
	struct SpriteMeta { int16_t width, height, x_offs, y_offs; };
	std::unordered_map<SpriteID, SpriteMeta> meta_cache;

	std::vector<uint8_t> upload_rgba_buf; ///< Reusable buffer for RGBA pixel conversion.
	std::vector<uint8_t> upload_m_buf;    ///< Reusable buffer for M channel extraction.

	/* PBO async upload state.
	 * Alternative strategies considered:
	 *   1. Ring buffer: N fixed PBOs, cyclic reuse. Simple but fixed size per PBO.
	 *   2. Pool of small PBOs: one per sprite, return to pool after fence. Flexible but many GL objects.
	 *   3. (chosen) Single large PBO + offset: one large PBO per frame, write at offsets,
	 *      fence on whole batch. Fewest GL objects, best throughput for batch upload. */
	static constexpr size_t PBO_SIZE = 4 * 1024 * 1024;  ///< 4MB per PBO buffer.
	static constexpr int64_t PBO_TIME_BUDGET_US = 15000; ///< Per-frame upload time budget (15ms).
	PBOUploadBatch pbo_current;   ///< Batch being filled this frame.
	PBOUploadBatch pbo_inflight;  ///< Batch submitted last frame, waiting fence.
	std::unordered_set<GLESSpriteID> pbo_inflight_keys; ///< Fast lookup for inflight sprites.

	GLESSpriteEntry placeholder_entry{}; ///< 1x1 semi-transparent black sprite for missing sprites.
	bool placeholder_ready = false;

	/** GL-thread-owned memory-backed SpriteFile copies, keyed by original SpriteFile pointer. */
	std::unordered_map<const SpriteFile *, std::unique_ptr<SpriteFile>> gl_sprite_files;
	static constexpr int MAX_LOADS_PER_FRAME = 500; ///< Per-frame budget for on-demand sprite loads.
	int loads_this_frame = 0;

	/** Post-clear sprite load monitoring (GL thread only). */
	int post_clear_frames = -1;      ///< Frames since last clear (-1 = not monitoring).
	int post_clear_zero_streak = 0;  ///< Consecutive frames with 0 new sprites.

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
		this->colour_pages.clear();
		this->remap_pages.clear();
		this->sprites.clear();
		this->pbo_current.pbo = 0;
		this->pbo_inflight.pbo = 0;
		/* Can't delete fence after context loss, just null it. */
		this->pbo_inflight.fence = nullptr;
		this->pbo_inflight.entries.clear();
		this->pbo_current.entries.clear();
		this->pbo_inflight_keys.clear();
		/* Signal game thread to clear known_sprites on next access. */
		this->known_clear_pending.store(true);
	}

	/** Process deferred clear. Must be called from GL thread (e.g. in Paint). */
	void ProcessPendingClear();

	/** Process PBO async uploads: check fence, fill batch, submit. GL thread only. */
	void ProcessPBOUploads();

private:
	std::atomic<bool> clear_pending{false};
	std::chrono::steady_clock::time_point clear_time{};
	size_t sprites_after_clear = 0;
	bool measuring_reload = false;
	void ClearSprites();
	void PBOCheckInflight();
	void PBOFillBatch(std::chrono::steady_clock::time_point t_start);
	void PBOSubmitBatch();

public:
	/** Upload a sprite to the atlas. Must be called from the GL thread. */
	GLESSpriteID Upload(SpriteID sprite_id, ZoomLevel zoom,
	                    const SpriteLoader::CommonPixel *pixels,
	                    uint16_t width, uint16_t height,
	                    bool has_rgb, bool has_remap);

	/** Enqueue pixel data for GPU upload. Thread-safe, no GL calls.
	 *  Skips if sprite already known to pipeline. */
	GLESSpriteID Enqueue(SpriteID sprite_id, ZoomLevel zoom,
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

	/** Check if sprite is in the pipeline (queued/PBO/uploaded). Thread-safe. */
	bool IsKnown(SpriteID id);

	/** Cache root dimensions from Encode. Game thread only. */
	void CacheMeta(SpriteID id, int16_t w, int16_t h, int16_t xo, int16_t yo);

	/** Get cached root dimensions. Returns true if found. Game thread only. */
	bool GetCachedMeta(SpriteID id, int16_t &w, int16_t &h, int16_t &xo, int16_t &yo) const;

	/** Get the GL_TEXTURE_2D_ARRAY handle for the colour atlas. */
	GLuint GetColourTexture() const { return colour_array_tex; }

	/** Get the GL_TEXTURE_2D_ARRAY handle for the remap atlas. */
	GLuint GetRemapTexture() const { return remap_array_tex; }

	/** Get atlas page counts for diagnostics. */
	size_t GetColourPageCount() const { return colour_pages.size(); }
	size_t GetRemapPageCount() const { return remap_pages.size(); }
	size_t GetSpriteCount() const { return sprites.size(); }

	/** Get queued sprite count and memory usage. */
	size_t GetStagedCount() const { std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex)); return upload_queue.size(); }
	int64_t GetStagedBytes() const {
		std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex));
		int64_t bytes = 0;
		for (const auto &req : upload_queue) bytes += req.pixels.size() * sizeof(SpriteLoader::CommonPixel);
		return bytes;
	}

	/** Get atlas occupancy as approximate percentage (0-100). */
	int GetColourOccupancyPercent() const;
	int GetRemapOccupancyPercent() const;

};

#endif /* VIDEO_GLES_SPRITE_H */
