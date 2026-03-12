/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file draw_snapshot.h Data structures for two-thread snapshot rendering. */

#ifndef DRAW_SNAPSHOT_H
#define DRAW_SNAPSHOT_H

#include "../blitter/base.hpp"
#include "../gfx_type.h"
#include "../zoom_type.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

/** Unique key for a sprite at a specific zoom level (same as SnapshotSpriteKey). */
using SnapshotSpriteKey = uint64_t;

/** A single draw command recorded from GfxBlitter. */
struct DrawCommand {
	enum Type : uint8_t {
		SPRITE,          ///< Plain sprite: tile, building, tree
		RECOLOUR,        ///< Sprite with palette remap (vehicle, company colour)
		// STRING,       ///< Text on viewport (station/town names)
		// FILL_RECT,    ///< Filled rectangle (selection box, grid overlay)
		// LINE,         ///< Line (debug markers, POI overlay)
		// CHILD_SPRITE, ///< Child sprite (overlay on vehicle: cargo, status)
	};

	Type type;              ///< Operation type
	BlitterMode mode;       ///< Actual blitter mode (Normal, ColourRemap, Transparent, etc.)
	int16_t x;              ///< Screen X coordinate (left)
	int16_t y;              ///< Screen Y coordinate (top)
	SpriteID sprite;        ///< Sprite ID in sprite cache
	PaletteID palette;      ///< Remap palette (for RECOLOUR, 0 = no remap)
	int16_t width;          ///< Visible width after clipping
	int16_t height;         ///< Visible height after clipping
	int16_t skip_left;      ///< Source skip X for clipping
	int16_t skip_top;       ///< Source skip Y for clipping
	int16_t sprite_width;   ///< Full sprite width (zoom-adjusted)
	int16_t sprite_height;  ///< Full sprite height (zoom-adjusted)
	ZoomLevel zoom;         ///< Zoom level at draw time
	const uint8_t *remap = nullptr; ///< Pointer to 256-byte remap table (stable sprite cache data)
	// uint16_t width;      ///< Area width (for FILL_RECT, STRING)
	// uint16_t height;     ///< Area height (for FILL_RECT)
	// uint8_t colour;      ///< Colour index (for FILL_RECT, LINE)
	// int16_t x2, y2;      ///< End point (for LINE)
	// uint16_t string_id;  ///< StringID (for STRING)
};

/** Pixel data for a sprite not yet in the GPU atlas. */
struct StagedSprite {
	uint16_t width;                 ///< Width in pixels
	uint16_t height;                ///< Height in pixels
	int16_t x_offs;                 ///< X offset from anchor point
	int16_t y_offs;                 ///< Y offset from anchor point
	bool has_rgb;                   ///< Has RGB/Alpha data
	bool has_remap;                 ///< Has palette remap data
	std::vector<uint8_t> pixels; ///< Raw pixel data (CommonPixel: 5 bytes per pixel: r,g,b,a,m)
};

/** A complete frame of draw commands with associated data. */
struct DrawSnapshot {
	uint64_t frame_id{0};           ///< Monotonic frame counter
	bool full_redraw{true};         ///< true = full screen, false = dirty only
	// Rect dirty_region{};         ///< Changed area (if !full_redraw)

	std::vector<DrawCommand> commands;                          ///< Draw commands in back-to-front order
	std::vector<Rect> dirty_rects;                              ///< Dirty regions from MakeDirty during recording
	std::unordered_map<SnapshotSpriteKey, StagedSprite> staged;      ///< Sprites missing from GPU atlas
	std::array<uint32_t, 256> palette{};                        ///< Current 256-colour palette (RGBA)

	int viewport_width{0};          ///< Viewport width in pixels
	int viewport_height{0};         ///< Viewport height in pixels
	ZoomLevel zoom{};               ///< Current zoom level

	void Clear() {
		commands.clear();
		dirty_rects.clear();
		staged.clear();
		full_redraw = true;
		frame_id = 0;
	}
};

/**
 * Lock-free triple buffer for passing DrawSnapshots from CPU to GPU thread.
 *
 * Uses a single atomic "shared" slot with non-atomic thread-local indices.
 * Each Publish/Acquire is a single atomic exchange — no multi-step races.
 *
 * Invariant: {write_idx, shared_idx, read_idx} is always a permutation of {0,1,2}.
 */
struct SnapshotTripleBuffer {
	DrawSnapshot buffers[3];

	std::atomic<int> shared_idx{1};  ///< Shared slot, accessed by both threads via exchange
	int write_idx{0};                ///< CPU thread only
	int read_idx{2};                 ///< GPU thread only

	/* Metrics */
	uint32_t cpu_ahead_count{0};     ///< CPU overwrote ready before GPU grabbed it
	uint32_t gpu_ahead_count{0};     ///< GPU requested snapshot but none new
	uint32_t swap_count{0};          ///< Successful exchanges
	uint64_t cpu_frame_id{0};        ///< Snapshots published by CPU
	uint64_t gpu_frame_id{0};        ///< Last frame_id received by GPU

	/** CPU thread: get buffer to write into. */
	DrawSnapshot &GetWriteBuffer() { return buffers[write_idx]; }

	/** CPU thread: publish finished snapshot. Single atomic exchange. */
	void Publish() {
		buffers[write_idx].frame_id = ++cpu_frame_id;
		/* Swap write <-> shared in one atomic op. */
		write_idx = shared_idx.exchange(write_idx, std::memory_order_acq_rel);
		if (cpu_frame_id - gpu_frame_id > 1) cpu_ahead_count++;
	}

	/** GPU thread: acquire latest snapshot. Peeks first, swaps only if new data. */
	bool Acquire() {
		/* Peek at shared buffer's frame_id to avoid unnecessary swaps. */
		int cur_shared = shared_idx.load(std::memory_order_acquire);
		if (buffers[cur_shared].frame_id <= gpu_frame_id) {
			gpu_ahead_count++;
			return false;
		}
		/* New data available — swap read <-> shared in one atomic op. */
		read_idx = shared_idx.exchange(read_idx, std::memory_order_acq_rel);
		gpu_frame_id = buffers[read_idx].frame_id;
		swap_count++;
		return true;
	}

	/** GPU thread: get buffer to read from. */
	DrawSnapshot &GetReadBuffer() { return buffers[read_idx]; }

	/** Reset all metrics counters. */
	void ResetMetrics() {
		cpu_ahead_count = 0;
		gpu_ahead_count = 0;
		swap_count = 0;
	}
};

/** Start recording draw commands into a snapshot. CPU thread only. */
void StartRecording(DrawSnapshot &snapshot);

/** Stop recording. Logs redundant staging count. */
void StopRecording();

/** Returns true if currently recording (for GfxBlitter intercept). */
bool IsRecording();

/** Append a draw command to the current recording snapshot. No-op if not recording. */
void RecordCommand(const DrawCommand &cmd);

#endif /* DRAW_SNAPSHOT_H */
