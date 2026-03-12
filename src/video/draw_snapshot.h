/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file draw_snapshot.h Data structures for two-thread snapshot rendering. */

#ifndef DRAW_SNAPSHOT_H
#define DRAW_SNAPSHOT_H

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
	int16_t x;              ///< Screen X coordinate (left)
	int16_t y;              ///< Screen Y coordinate (top)
	SpriteID sprite;        ///< Sprite ID in sprite cache
	PaletteID palette;      ///< Remap palette (for RECOLOUR, 0 = no remap)
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
	std::unordered_map<SnapshotSpriteKey, StagedSprite> staged;      ///< Sprites missing from GPU atlas
	std::array<uint32_t, 256> palette{};                        ///< Current 256-colour palette (RGBA)

	int viewport_width{0};          ///< Viewport width in pixels
	int viewport_height{0};         ///< Viewport height in pixels
	ZoomLevel zoom{};               ///< Current zoom level

	void Clear() {
		commands.clear();
		staged.clear();
		full_redraw = true;
		frame_id = 0;
	}
};

/**
 * Lock-free triple buffer for passing DrawSnapshots from CPU to GPU thread.
 *
 * Three buffers with atomic index swaps:
 * - write_buf: CPU thread writes here (exclusive CPU access)
 * - ready_buf: last published snapshot (swap point)
 * - read_buf:  GPU thread reads here (exclusive GPU access)
 */
struct SnapshotTripleBuffer {
	DrawSnapshot buffers[3];

	std::atomic<int> write_idx{0};   ///< CPU owns buffers[write_idx]
	std::atomic<int> ready_idx{1};   ///< Latest published snapshot
	std::atomic<int> read_idx{2};    ///< GPU owns buffers[read_idx]

	/* Metrics */
	uint32_t cpu_ahead_count{0};     ///< CPU overwrote ready before GPU grabbed it
	uint32_t gpu_ahead_count{0};     ///< GPU requested snapshot but none new
	uint32_t swap_count{0};          ///< Successful exchanges
	uint64_t cpu_frame_id{0};        ///< Snapshots published by CPU
	uint64_t gpu_frame_id{0};        ///< Last frame_id received by GPU

	/** CPU thread: get buffer to write into. */
	DrawSnapshot &GetWriteBuffer() { return buffers[write_idx.load(std::memory_order_relaxed)]; }

	/** CPU thread: publish finished snapshot. Swaps write <-> ready. */
	void Publish() {
		buffers[write_idx.load(std::memory_order_relaxed)].frame_id = ++cpu_frame_id;
		int old_ready = ready_idx.exchange(write_idx.load(std::memory_order_relaxed), std::memory_order_acq_rel);
		write_idx.store(old_ready, std::memory_order_relaxed);
		if (cpu_frame_id - gpu_frame_id > 1) cpu_ahead_count++;
	}

	/** GPU thread: acquire latest snapshot. Swaps read <-> ready. Returns true if new. */
	bool Acquire() {
		int old_read = read_idx.exchange(ready_idx.load(std::memory_order_relaxed), std::memory_order_acq_rel);
		ready_idx.store(old_read, std::memory_order_relaxed);
		uint64_t new_id = buffers[read_idx.load(std::memory_order_relaxed)].frame_id;
		if (new_id > gpu_frame_id) {
			gpu_frame_id = new_id;
			swap_count++;
			return true;
		}
		gpu_ahead_count++;
		return false;
	}

	/** GPU thread: get buffer to read from. */
	DrawSnapshot &GetReadBuffer() { return buffers[read_idx.load(std::memory_order_relaxed)]; }

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

#endif /* DRAW_SNAPSHOT_H */
