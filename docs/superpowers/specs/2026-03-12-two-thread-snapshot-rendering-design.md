# Two-Thread Snapshot Rendering

## Problem

Current GLES wallpaper mode runs single-threaded (`is_game_threaded = false`). GameLoop, UpdateWindows, and Paint all execute sequentially in the main thread. This wastes CPU cycles — while the GPU renders, the CPU idles, and vice versa.

## Goal

Split work into two fully parallel threads:
- **CPU thread** — game state + draw command recording
- **GPU thread** — sprite upload + rendering

Neither thread waits for the other. Communication via lock-free triple buffer.

## Architecture

### Thread Roles

```
CPU thread ("ottd:game")                    GPU thread (main)
========================                    ========================

lock(game_state_mutex)
  DrainCommandQueue()
  PollEvent() + InputLoop()
  ::GameLoop()
unlock(game_state_mutex)

StartRecording(write_buf)                   LockVideoBuffer()  // vsync
::UpdateWindows()                           Acquire()  // atomic swap
  -> GfxBlitter intercept                   PaintFromSnapshot():
  -> commands + staged sprites                Upload staged -> atlas
StopRecording()                               UpdatePalette
CopyPalette -> write_buf                      Execute draw commands
Publish()  // atomic swap                     SDL_GL_SwapWindow
                                            UnlockVideoBuffer()
sleep / yield
```

### Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Snapshot content | Draw command list | Minimal changes to existing draw pipeline |
| Buffer strategy | Triple buffer | No blocking, minimal latency |
| Sprite staging | CPU stages, GPU uploads | Load distributed across both threads |
| Dirty rects | Full redraw (with fallback to partial) | Camera usually moves in wallpaper mode |
| Intercept point | GfxBlitter() in gfx.cpp | Single point for all sprites, has SpriteID/coords/palette |
| Thread ownership | Main thread = GPU (owns GL context) | Avoids GL context transfer across threads on Android |
| Input handling | CPU thread | Input modifies game/window state |

### Synchronization

- `game_state_mutex` — protects GameLoop + Input in CPU thread only. GPU thread never touches it.
- Triple buffer Publish/Acquire — `atomic_exchange`, non-blocking.
- `cmd_queue_mutex` — for QueueOnMainThread (OS input events -> CPU thread).
- `game_thread_wait_mutex` — not needed in snapshot mode (GPU does not wait for CPU).

## Data Structures

### DrawCommand

```cpp
struct DrawCommand {
    enum Type : uint8_t {
        SPRITE,          // plain sprite: tile, building, tree
        RECOLOUR,        // sprite with palette remap (vehicle, company colour)
        // STRING,       // text on viewport (station/town names)
        // FILL_RECT,    // filled rectangle (selection box, grid overlay)
        // LINE,         // line (debug markers, POI overlay)
        // CHILD_SPRITE, // child sprite (overlay on vehicle: cargo, status)
    };

    Type type;              // operation type
    int16_t x;              // screen X coordinate (left)
    int16_t y;              // screen Y coordinate (top)
    SpriteID sprite;        // sprite ID in sprite cache
    PaletteID palette;      // remap palette (for RECOLOUR, 0 = no remap)
    // uint16_t width;      // area width (for FILL_RECT, STRING)
    // uint16_t height;     // area height (for FILL_RECT)
    // uint8_t colour;      // colour index (for FILL_RECT, LINE)
    // int16_t x2, y2;      // end point (for LINE)
    // uint16_t string_id;  // StringID (for STRING)
};
```

### StagedSprite

```cpp
struct StagedSprite {
    uint16_t width;                 // width in pixels
    uint16_t height;                // height in pixels
    int16_t x_offs;                 // X offset from anchor point
    int16_t y_offs;                 // Y offset from anchor point
    std::vector<uint8_t> pixels;    // RGBA pixel data
};
```

### DrawSnapshot

```cpp
struct DrawSnapshot {
    uint64_t frame_id{0};                                   // monotonic frame counter
    bool full_redraw{true};                                 // true = full screen, false = dirty only
    Rect dirty_region{};                                    // changed area (if !full_redraw)

    std::vector<DrawCommand> commands;                      // draw commands in back-to-front order
    std::unordered_map<SpriteID, StagedSprite> staged;      // sprites missing from GPU atlas
    std::array<uint32_t, 256> palette;                      // current 256-colour palette (RGBA)

    int viewport_width{0};                                  // viewport width in pixels
    int viewport_height{0};                                 // viewport height in pixels
    ZoomLevel zoom{};                                       // current zoom level

    void Clear() {
        commands.clear();
        staged.clear();
        full_redraw = true;
    }
};
```

### TripleBuffer

```cpp
struct TripleBuffer {
    DrawSnapshot buffers[3];
    std::atomic<int> write_idx{0};   // CPU owns buffers[write_idx]
    std::atomic<int> ready_idx{1};   // latest published snapshot
    std::atomic<int> read_idx{2};    // GPU owns buffers[read_idx]

    // Metrics
    uint32_t cpu_ahead_count{0};     // CPU overwrote ready before GPU grabbed it
    uint32_t gpu_ahead_count{0};     // GPU requested snapshot but none new available
    uint32_t swap_count{0};          // successful exchanges (GPU got fresh snapshot)
    uint64_t cpu_frame_id{0};        // snapshots published by CPU
    uint64_t gpu_frame_id{0};        // frame_id of last snapshot received by GPU

    DrawSnapshot &GetWriteBuffer();

    void Publish() {
        buffers[write_idx].frame_id = ++cpu_frame_id;
        int old_ready = ready_idx.exchange(write_idx);
        write_idx = old_ready;
        if (cpu_frame_id - gpu_frame_id > 1) cpu_ahead_count++;
    }

    bool Acquire() {
        int old_read = read_idx.exchange(ready_idx);
        ready_idx = old_read;
        uint64_t new_id = buffers[read_idx].frame_id;
        if (new_id > gpu_frame_id) {
            gpu_frame_id = new_id;
            swap_count++;
            return true;
        }
        gpu_ahead_count++;
        return false;
    }

    DrawSnapshot &GetReadBuffer();
};
```

Periodic log: `TRIPLE_BUFFER: swaps=60 cpu_ahead=3 gpu_ahead=7 | cpu_fps=63 gpu_fps=60`
- `cpu_ahead` high -> GPU bottleneck
- `gpu_ahead` high -> CPU bottleneck

## GfxBlitter Intercept

Intercept point: `GfxBlitter()` in `src/gfx.cpp` (line 1078). All viewport sprites pass through this function.

```cpp
static DrawSnapshot *_recording_snapshot = nullptr;
static uint32_t _recording_stage_redundant = 0;

void StartRecording(DrawSnapshot &snapshot) {
    snapshot.Clear();
    _recording_snapshot = &snapshot;
    _recording_stage_redundant = 0;
}

void StopRecording() {
    if (_recording_stage_redundant > 0) {
        Debug(driver, 0, "SNAPSHOT: staged {} redundant sprites (already in atlas)",
            _recording_stage_redundant);
    }
    _recording_snapshot = nullptr;
}
```

Inside `GfxBlitter()`, before existing code:

```cpp
if (_recording_snapshot != nullptr) {
    DrawCommand cmd;
    cmd.type = (mode == BlitterMode::ColourRemap)
        ? DrawCommand::RECOLOUR : DrawCommand::SPRITE;
    cmd.x = x + sprite->x_offs;
    cmd.y = y + sprite->y_offs;
    cmd.sprite = sprite_id;
    cmd.palette = 0;  // TODO: extract from _colour_remap_ptr
    _recording_snapshot->commands.push_back(cmd);

    // Stage sprite if not in GPU atlas.
    // False negative OK: GPU may have uploaded after our check.
    // GPU thread skips duplicate upload in LookupOrUpload().
    GLESSpriteID key = (static_cast<uint64_t>(sprite_id) << 4)
                     | static_cast<uint8_t>(zoom);
    if (_recording_snapshot->staged.find(key) == _recording_snapshot->staged.end()) {
        auto *atlas = &GLESBackend::Get()->GetSpriteAtlas();
        if (atlas->Lookup(key) == nullptr) {
            StagedSprite ss;
            ss.width = sprite->width;
            ss.height = sprite->height;
            ss.x_offs = sprite->x_offs;
            ss.y_offs = sprite->y_offs;
            ss.pixels.assign(sprite->data,
                sprite->data + sprite->width * sprite->height * 4);
            _recording_snapshot->staged[key] = std::move(ss);
        } else {
            _recording_stage_redundant++;
        }
    }
    return;  // do not draw, only record
}
```

`atlas->Lookup(key)` reads `sprites` map from CPU thread while GPU thread writes to it. Race condition: CPU may not see a recent upload and stages redundantly. `_recording_stage_redundant` counter tracks this. Log `SNAPSHOT: staged N redundant sprites` shows scale. If high, add `atomic<bool> uploaded` per atlas entry.

## GPU Thread: PaintFromSnapshot

New function called from `VideoDriver::Tick()` with early return:

```cpp
bool VideoDriver_SDL_GLES::PaintFromSnapshot()
{
    if (!triple_buffer) return false;

    bool have_new = triple_buffer->Acquire();
    DrawSnapshot &snap = triple_buffer->GetReadBuffer();

    if (snap.frame_id == 0) return false;  // no snapshot yet

    auto &atlas = GLESBackend::Get()->GetSpriteAtlas();

    // 1. Upload ALL staged sprites BEFORE drawing
    uint32_t uploaded = 0;
    for (auto &[key, staged] : snap.staged) {
        if (atlas.Lookup(key) != nullptr) continue;
        atlas.Upload(..., staged.pixels.data(), staged.width, staged.height, ...);
        uploaded++;
    }
    if (uploaded > 0) {
        Debug(driver, 0, "SNAPSHOT: uploaded {} new sprites to atlas", uploaded);
    }

    // 2. Palette
    UpdatePaletteTexture(snap.palette);

    // 3. Draw
    if (snap.full_redraw) {
        glClear(GL_COLOR_BUFFER_BIT);
    }
    for (const DrawCommand &cmd : snap.commands) {
        auto *entry = atlas.Lookup(MakeKey(cmd.sprite, snap.zoom));
        if (entry == nullptr) continue;
        GLESBackend::DrawSprite(entry, cmd.x, cmd.y, cmd.palette);
    }

    // 4. Swap
    SDL_GL_SwapWindow(this->sdl_window);

    return true;
}
```

### Tick Integration

```cpp
void VideoDriver::Tick()
{
    // ... game loop for single-threaded mode ...

    this->LockVideoBuffer();

    // Snapshot path: early return if painted from snapshot
    if (this->PaintFromSnapshot()) {
        this->UnlockVideoBuffer();
        return;
    }

    // Fallback: old path with mutexes and UpdateWindows
    {
        std::lock_guard<std::mutex> lock_wait(this->game_thread_wait_mutex);
        std::lock_guard<std::mutex> lock_state(this->game_state_mutex);
        // ... existing UpdateWindows + Paint code ...
    }

    this->UnlockVideoBuffer();
}
```

## CPU Thread: Snapshot Generation

```cpp
void VideoDriver::GameThread()
{
    while (!_exit_game) {
        {
            std::lock_guard<std::mutex> lock(this->game_state_mutex);

            this->DrainCommandQueue();
            while (this->PollEvent()) {}
            this->InputLoop();
            ::InputLoop();

            ::GameLoop();
        }

        if (triple_buffer) {
            DrawSnapshot &snap = triple_buffer->GetWriteBuffer();
            snap.zoom = ...;
            snap.viewport_width = ...;
            snap.viewport_height = ...;

            // Copy palette
            for (int i = 0; i < 256; i++) {
                snap.palette[i] = GetPaletteRGBA(i);
            }

            StartRecording(snap);
            ::UpdateWindows();
            StopRecording();

            triple_buffer->Publish();
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

`UpdateWindows()` runs after `game_state_mutex` is released. This is safe: game state is only modified inside `::GameLoop()` which has already completed. `UpdateWindows()` reads game state (vehicle positions, tiles) but does not modify it.

## Dirty Rect Fallback

`DrawSnapshot` always contains full command list + dirty region:

- `full_redraw = true` -> GPU clears and draws everything (mode A)
- `full_redraw = false` -> GPU draws only commands within `dirty_region` over previous frame (mode B)

Switching A <-> B is one line: `snap.full_redraw = true` (always A) vs computed dirty rect. Start with A (full redraw), add B later as optimization.

## Key Files

| File | Changes |
|------|---------|
| `src/gfx.cpp` | GfxBlitter intercept, StartRecording/StopRecording |
| `src/video/sdl2_gles_v.cpp` | `is_game_threaded = true`, PaintFromSnapshot() |
| `src/video/video_driver.cpp` | Tick() early return, GameThread() snapshot generation |
| `src/video/video_driver.hpp` | TripleBuffer member, PaintFromSnapshot() declaration |
| NEW `src/video/draw_snapshot.h` | DrawCommand, StagedSprite, DrawSnapshot, TripleBuffer |
