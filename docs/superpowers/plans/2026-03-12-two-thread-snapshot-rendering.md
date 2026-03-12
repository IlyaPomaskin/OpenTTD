# Two-Thread Snapshot Rendering — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split GLES wallpaper rendering into two parallel threads — CPU records draw commands into snapshots, GPU renders from snapshots via lock-free triple buffer.

**Architecture:** CPU thread runs GameLoop + UpdateWindows in recording mode, capturing draw commands into a DrawSnapshot. GPU thread (main, owns GL context) acquires latest snapshot from triple buffer, uploads staged sprites, and executes draw commands. No mutex between threads.

**Tech Stack:** C++20, OpenGL ES 3.0, SDL2, std::atomic

**Spec:** `docs/superpowers/specs/2026-03-12-two-thread-snapshot-rendering-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/video/draw_snapshot.h` | Create | DrawCommand, StagedSprite, DrawSnapshot, TripleBuffer structs |
| `src/gfx.cpp` | Modify | GfxBlitter intercept — record commands instead of drawing |
| `src/video/video_driver.cpp` | Modify | GameThread snapshot generation, Tick early return |
| `src/video/video_driver.hpp` | Modify | TripleBuffer member, PaintFromSnapshot declaration |
| `src/video/sdl2_gles_v.cpp` | Modify | PaintFromSnapshot implementation, `is_game_threaded = true` |
| `src/video/sdl2_gles_v.h` | Modify | PaintFromSnapshot method declaration |

---

## Task 1: DrawSnapshot data structures

**Files:**
- Create: `src/video/draw_snapshot.h`

- [ ] **Step 1: Create draw_snapshot.h with all data structures**

```cpp
// src/video/draw_snapshot.h
#ifndef DRAW_SNAPSHOT_H
#define DRAW_SNAPSHOT_H

#include "../stdafx.h"
#include "../gfx_type.h"
#include "../zoom_type.h"
#include "gles_sprite.h"
#include <array>
#include <atomic>
#include <unordered_map>
#include <vector>

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
	std::vector<SpriteLoader::CommonPixel> pixels; ///< Raw pixel data
};

/** A complete frame of draw commands with associated data. */
struct DrawSnapshot {
	uint64_t frame_id{0};           ///< Monotonic frame counter
	bool full_redraw{true};         ///< true = full screen, false = dirty only
	// Rect dirty_region{};         ///< Changed area (if !full_redraw)

	std::vector<DrawCommand> commands;                          ///< Draw commands in back-to-front order
	std::unordered_map<GLESSpriteID, StagedSprite> staged;      ///< Sprites missing from GPU atlas
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

#endif /* DRAW_SNAPSHOT_H */
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build /tmp/openttd -j4 2>&1 | grep -E "draw_snapshot|error"` — header-only, no compilation unit yet. Include it from `sdl2_gles_v.cpp` temporarily to test:

Add `#include "draw_snapshot.h"` at top of `src/video/sdl2_gles_v.cpp`, build, then remove.

- [ ] **Step 3: Commit**

```
git -C ~/work/OpenTTD add src/video/draw_snapshot.h
git -C ~/work/OpenTTD commit -m "Add DrawSnapshot, DrawCommand, TripleBuffer data structures"
```

---

## Task 2: GfxBlitter recording intercept

**Files:**
- Modify: `src/gfx.cpp:1078-1196` (GfxBlitter template function)
- Modify: `src/video/draw_snapshot.h` (add StartRecording/StopRecording declarations)

- [ ] **Step 1: Add recording state to draw_snapshot.h**

Add before `#endif` in `draw_snapshot.h`:

```cpp
/** Start recording draw commands into a snapshot. CPU thread only. */
void StartRecording(DrawSnapshot &snapshot);

/** Stop recording. Logs redundant staging count. */
void StopRecording();

/** Returns true if currently recording (for GfxBlitter intercept). */
bool IsRecording();

/** Get the current recording snapshot. Only valid when IsRecording() is true. */
DrawSnapshot *GetRecordingSnapshot();
```

- [ ] **Step 2: Add recording implementation in gfx.cpp**

Add after existing includes at top of `src/gfx.cpp` (around line 35):

```cpp
#include "video/draw_snapshot.h"
#include "video/gles_sprite.h"
#include "video/gles_backend.h"
```

Add recording state (after the includes, before first function):

```cpp
static DrawSnapshot *_recording_snapshot = nullptr;
static uint32_t _recording_stage_redundant = 0;
static uint32_t _recording_stage_new = 0;

void StartRecording(DrawSnapshot &snapshot) {
	snapshot.Clear();
	_recording_snapshot = &snapshot;
	_recording_stage_redundant = 0;
	_recording_stage_new = 0;
}

void StopRecording() {
	if (_recording_snapshot != nullptr && (_recording_stage_new > 0 || _recording_stage_redundant > 0)) {
		Debug(driver, 0, "SNAPSHOT: {} commands, staged {} new sprites ({} redundant)",
			(int)_recording_snapshot->commands.size(), _recording_stage_new, _recording_stage_redundant);
	}
	_recording_snapshot = nullptr;
}

bool IsRecording() { return _recording_snapshot != nullptr; }
DrawSnapshot *GetRecordingSnapshot() { return _recording_snapshot; }
```

- [ ] **Step 3: Add intercept at top of GfxBlitter function body**

In `src/gfx.cpp`, inside `GfxBlitter()` (line 1079), add after opening brace, before `const DrawPixelInfo *dpi = ...`:

```cpp
	if (_recording_snapshot != nullptr) {
		DrawCommand cmd;
		cmd.type = (mode == BlitterMode::ColourRemap) ? DrawCommand::RECOLOUR : DrawCommand::SPRITE;

		/* Compute screen position same as the normal path. */
		int rx = SCALED_XY ? ScaleByZoom(x, zoom) : x;
		int ry = SCALED_XY ? ScaleByZoom(y, zoom) : y;
		rx += sprite->x_offs;
		ry += sprite->y_offs;

		cmd.x = static_cast<int16_t>(rx);
		cmd.y = static_cast<int16_t>(ry);
		cmd.sprite = sprite_id;
		cmd.palette = 0; /* TODO: extract from _colour_remap_ptr */
		_recording_snapshot->commands.push_back(cmd);

		/* Stage sprite if not in GPU atlas.
		 * False negative OK: GPU may have uploaded after our check.
		 * GPU thread skips duplicate upload. */
		if (_gles_gpu_sprites && GLESBackend::Get() != nullptr) {
			GLESSpriteID key = MakeGLESSpriteKey(sprite_id, zoom);
			if (_recording_snapshot->staged.find(key) == _recording_snapshot->staged.end()) {
				auto &atlas = GLESBackend::Get()->GetSpriteAtlas();
				if (atlas.Lookup(key) == nullptr) {
					StagedSprite ss;
					ss.width = sprite->width;
					ss.height = sprite->height;
					ss.x_offs = sprite->x_offs;
					ss.y_offs = sprite->y_offs;
					ss.has_rgb = true;  /* TODO: detect from sprite data */
					ss.has_remap = false;
					ss.pixels.assign(
						reinterpret_cast<const SpriteLoader::CommonPixel *>(sprite->data),
						reinterpret_cast<const SpriteLoader::CommonPixel *>(sprite->data) + sprite->width * sprite->height);
					_recording_snapshot->staged[key] = std::move(ss);
					_recording_stage_new++;
				} else {
					_recording_stage_redundant++;
				}
			}
		}
		return;
	}
```

- [ ] **Step 4: Build and verify compilation**

Run: `cmake --build /tmp/openttd -j4`

Expected: compiles with no new errors (existing GLES3 error is pre-existing).

- [ ] **Step 5: Commit**

```
git -C ~/work/OpenTTD add src/gfx.cpp src/video/draw_snapshot.h
git -C ~/work/OpenTTD commit -m "Add GfxBlitter recording intercept for snapshot mode"
```

---

## Task 3: PaintFromSnapshot in GPU thread

**Files:**
- Modify: `src/video/sdl2_gles_v.h:15-47` (add method declaration)
- Modify: `src/video/sdl2_gles_v.cpp:227+` (add PaintFromSnapshot implementation)

- [ ] **Step 1: Add PaintFromSnapshot declaration to sdl2_gles_v.h**

Add inside `VideoDriver_SDL_GLES` class, in the `protected:` section, after `void Paint() override;`:

```cpp
	bool PaintFromSnapshot();
```

- [ ] **Step 2: Implement PaintFromSnapshot in sdl2_gles_v.cpp**

Add new function before the existing `Paint()` method. This needs access to the triple buffer which will be stored in `video_driver.hpp` — for now reference it via `this->snapshot_buffer`:

```cpp
bool VideoDriver_SDL_GLES::PaintFromSnapshot()
{
	if (this->snapshot_buffer == nullptr) return false;

	bool have_new = this->snapshot_buffer->Acquire();
	DrawSnapshot &snap = this->snapshot_buffer->GetReadBuffer();

	if (snap.frame_id == 0) return false; /* No snapshot published yet. */

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return false;

	auto &atlas = backend->GetSpriteAtlas();

	/* 1. Upload ALL staged sprites BEFORE drawing. */
	uint32_t uploaded = 0;
	for (auto &[key, ss] : snap.staged) {
		if (atlas.Lookup(key) != nullptr) continue; /* Already in atlas. */
		SpriteID sid = static_cast<SpriteID>(key >> 4);
		ZoomLevel zm = static_cast<ZoomLevel>(key & 0xF);
		atlas.Upload(sid, zm, ss.pixels.data(), ss.width, ss.height, ss.has_rgb, ss.has_remap);
		uploaded++;
	}
	if (uploaded > 0) {
		Debug(driver, 0, "SNAPSHOT PAINT: uploaded {} new sprites to atlas", uploaded);
	}

	/* 2. Palette. */
	/* TODO: UpdatePaletteTexture(snap.palette); */

	/* 3. Draw commands. */
	/* TODO: iterate snap.commands and call GLESBackend draw for each sprite.
	 * For now, fall back to existing Paint() pipeline. */
	Debug(driver, 0, "SNAPSHOT PAINT: frame={} commands={} staged={} new={}",
		snap.frame_id, (int)snap.commands.size(), (int)snap.staged.size(), have_new);

	/* 4. Swap. */
	/* TODO: SDL_GL_SwapWindow after real draw implementation. */

	return false; /* Return false to fall through to normal Paint() until draw is implemented. */
}
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build /tmp/openttd -j4`

Note: will fail until Task 4 adds `snapshot_buffer` member. That's expected — proceed to Task 4.

- [ ] **Step 4: Commit**

```
git -C ~/work/OpenTTD add src/video/sdl2_gles_v.h src/video/sdl2_gles_v.cpp
git -C ~/work/OpenTTD commit -m "Add PaintFromSnapshot stub in GPU thread"
```

---

## Task 4: TripleBuffer integration and thread wiring

**Files:**
- Modify: `src/video/video_driver.hpp:362-373` (add snapshot_buffer member)
- Modify: `src/video/video_driver.cpp:47-64` (GameThread snapshot generation)
- Modify: `src/video/video_driver.cpp:106-205` (Tick early return)
- Modify: `src/video/sdl2_gles_v.cpp:114-124` (enable `is_game_threaded = true`)

- [ ] **Step 1: Add snapshot_buffer member to VideoDriver**

In `src/video/video_driver.hpp`, add include at top:

```cpp
#include "draw_snapshot.h"
```

Add member after `std::mutex game_thread_wait_mutex;` (around line 371):

```cpp
	std::unique_ptr<SnapshotTripleBuffer> snapshot_buffer; ///< Triple buffer for snapshot rendering. nullptr = disabled.
```

Add `#include <memory>` if not already present.

- [ ] **Step 2: Modify GameThread to generate snapshots**

In `src/video/video_driver.cpp`, replace `GameThread()` (lines 47-64):

```cpp
void VideoDriver::GameThread()
{
	while (!_exit_game) {
		this->GameLoop();

		/* Snapshot path: record draw commands after game state update. */
		if (this->snapshot_buffer != nullptr) {
			DrawSnapshot &snap = this->snapshot_buffer->GetWriteBuffer();
			snap.Clear();

			/* Copy palette. */
			extern Palette _cur_palette;
			for (int i = 0; i < 256; i++) {
				snap.palette[i] = _cur_palette.palette[i].data;
			}

			/* Record draw commands via GfxBlitter intercept. */
			StartRecording(snap);
			::UpdateWindows();
			StopRecording();

			snap.full_redraw = true;
			this->snapshot_buffer->Publish();
		}

		auto now = std::chrono::steady_clock::now();
		if (this->next_game_tick > now) {
			std::this_thread::sleep_for(this->next_game_tick - now);
		} else {
			std::lock_guard<std::mutex> lock(this->game_thread_wait_mutex);
		}
	}
}
```

Add include at top of `video_driver.cpp`:

```cpp
#include "draw_snapshot.h"
```

- [ ] **Step 3: Modify Tick() for early return via PaintFromSnapshot**

In `src/video/video_driver.cpp`, inside `Tick()` (line 120), after `this->LockVideoBuffer();` (line 128), add before the mutex block (line 132):

```cpp
		/* Snapshot path: paint from triple buffer, skip mutex wait. */
		if (this->snapshot_buffer != nullptr) {
			/* Input: drain command queue (protected by its own mutex). */
			this->DrainCommandQueue();

			bool painted = static_cast<VideoDriver_SDL_GLES *>(this)->PaintFromSnapshot();
			if (painted) {
				this->UnlockVideoBuffer();

				/* Log triple buffer metrics periodically. */
				auto &tb = *this->snapshot_buffer;
				if (tb.swap_count % 60 == 0 && tb.swap_count > 0) {
					Debug(driver, 0, "TRIPLE_BUFFER: swaps={} cpu_ahead={} gpu_ahead={}",
						tb.swap_count, tb.cpu_ahead_count, tb.gpu_ahead_count);
					tb.ResetMetrics();
				}
				return; /* Early return without touching draw_tick timing below. */
			}
			/* PaintFromSnapshot returned false — fall through to normal path. */
		}
```

Note: The `static_cast` is temporary. A cleaner approach is to make `PaintFromSnapshot` a virtual method in VideoDriver. For now this works since GLES is the only driver using snapshots.

- [ ] **Step 4: Enable is_game_threaded in GLES driver**

In `src/video/sdl2_gles_v.cpp`, find `this->is_game_threaded = false;` (line 124). Change to:

```cpp
	this->is_game_threaded = true;

	/* Create triple buffer for snapshot rendering. */
	this->snapshot_buffer = std::make_unique<SnapshotTripleBuffer>();
	Debug(driver, 0, "GLES: snapshot rendering enabled, is_game_threaded=true");
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build /tmp/openttd -j4`

- [ ] **Step 6: Commit**

```
git -C ~/work/OpenTTD add src/video/video_driver.hpp src/video/video_driver.cpp src/video/sdl2_gles_v.cpp
git -C ~/work/OpenTTD commit -m "Wire up triple buffer: CPU thread records, GPU thread paints"
```

---

## Task 5: Deploy and test on device

- [ ] **Step 1: Build and deploy to Android**

Run: `/usr/bin/python3 ~/work/OpenTTD/tools/run_android.py deploy`

- [ ] **Step 2: Launch and check logs**

Run:
```
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
```

Wait 5 seconds, then check logs:

```
adb logcat -d -s OpenTTD | grep -E "SNAPSHOT|TRIPLE_BUFFER|is_game_threaded"
```

Expected output should show:
- `GLES: snapshot rendering enabled, is_game_threaded=true`
- `SNAPSHOT: N commands, staged M new sprites (K redundant)` — from CPU thread
- `SNAPSHOT PAINT: frame=... commands=...` — from GPU thread
- `TRIPLE_BUFFER: swaps=60 cpu_ahead=... gpu_ahead=...` — periodic metrics

- [ ] **Step 3: Check FPS**

Run: `/usr/bin/python3 ~/work/OpenTTD/tools/run_android.py fps 5`

Compare with baseline (36-41 fps from single-threaded mode).

- [ ] **Step 4: Commit any fixes from testing**

---

## Task 6: Move input handling to CPU thread

**Files:**
- Modify: `src/video/video_driver.cpp:32-45` (GameLoop — add input processing)

- [ ] **Step 1: Move input into GameLoop under game_state_mutex**

In `src/video/video_driver.cpp`, modify `GameLoop()` (lines 32-45). Add input processing inside the mutex block, before `::GameLoop()`:

```cpp
void VideoDriver::GameLoop()
{
	this->next_game_tick += this->GetGameInterval();

	auto now = std::chrono::steady_clock::now();
	if (this->next_game_tick < now - ALLOWED_DRIFT * this->GetGameInterval()) this->next_game_tick = now;

	{
		std::lock_guard<std::mutex> lock(this->game_state_mutex);

		/* In snapshot mode, input is handled here in the CPU thread. */
		if (this->snapshot_buffer != nullptr) {
			this->DrainCommandQueue();
			::InputLoop();
		}

		::GameLoop();
	}
}
```

- [ ] **Step 2: Remove input from Tick snapshot path**

In `Tick()`, remove the `this->DrainCommandQueue();` call from the snapshot early-return block (added in Task 4 Step 3), since input is now handled in GameLoop.

- [ ] **Step 3: Build and deploy**

Run: `/usr/bin/python3 ~/work/OpenTTD/tools/run_android.py deploy`

- [ ] **Step 4: Verify input still works**

Launch app, send broadcast commands to test POI navigation:

```
adb shell am broadcast -a org.openttd.android.JUMP_POI
adb shell am broadcast -a org.openttd.android.SWITCH_MAP
```

- [ ] **Step 5: Commit**

```
git -C ~/work/OpenTTD add src/video/video_driver.cpp
git -C ~/work/OpenTTD commit -m "Move input handling to CPU thread under game_state_mutex"
```
