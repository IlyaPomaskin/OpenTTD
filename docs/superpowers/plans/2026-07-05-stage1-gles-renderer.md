# Stage 1: GLES Snapshot Renderer Implementation Plan

**Plan-confidence status:** Draft (iteration 2, confidence 80%)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GameActivity on a physical arm64 device renders a live, animated title map via the two-thread GLES snapshot pipeline; all JNI natives implemented (fixes the Stage-1-entry `UnsatisfiedLinkError` crash).

**Architecture:** Game thread records `DrawCommand`s via a recording blitter into a lock-free triple buffer; GL thread replays them through a batched GLES3 backend (sprite atlas + MRT FBO + resolve/blit passes). Sprites are decoded on the GL thread from RAM-buffered GRFs — no cross-thread pixel pipeline. Ported from branch `gles` with the reimpl decisions from `docs/wallpaper/overview.md` applied (this is a **selective re-port, not verbatim**).

**Tech Stack:** C++20, OpenGL ES 3.0, SDL2, Android NDK via gradle; specs in `docs/wallpaper/{rendering,engine-changes,wallpaper-mode,overview}.md`.

## Contents

- **Global Constraints** — binding build gate (gradle APK only), mergeability rules, and reimpl DROPs (no smooth scroll, no PBO/game-thread pixel pipeline, no legacy `Paint()`, no prewarm; `WALLPAPER_PERF`-gated instrumentation).
- **Spec deviations** — decisions vs overview.md: explicit coords ENFORCED (spec #7 upheld, per Q1.3 — realization reopened as Q2.1), PaletteID remap, POI stub, verbatim perf struct.
- **File Structure** — table of every new/edited file in the stage and its responsibility; dependency order = task order.
- **Task 1 — Interfaces, perf header, snapshot structs** — creates `gles_perf.h`, `draw_snapshot.{h,cpp}` and the additive blitter/spriteloader interface hooks. Nothing consumes them yet; the build just validates the interface edits against current upstream.
- **Task 2 — Sprite-cache memory backing** — RAM-buffers whole GRFs and adds memory-backed `RandomAccessFile`/`SpriteFile` so the GL thread can decode sprites self-sufficiently. All additive.
- **Task 3 — Shaders + sprite atlas** — ports the 7 GLSL ES shaders and the atlas packer with GL-thread on-demand decode. Strips all PBO/enqueue machinery, gfxstream workarounds, and the priority prewarm.
- **Task 4 — GLES backend** — the batched MRT-FBO renderer: single `PaintFBO` + `BlitToScreen` path (legacy `Paint()` deleted), three frame classes, GPU timer queries gated behind `WALLPAPER_PERF`.
- **Task 5 — Snapshot blitter** — the recording blitter: `Draw` records `DrawCommand`s (explicit coords from `BlitterParams`, per Q1.3), `Encode` caches metadata only. Wires `BuildGLSpriteFiles` into gfxinit.
- **Task 6 — VideoDriver base hooks** — adds minimal virtual hooks + pause CV to the base driver (deliberately NOT the reference's `Tick()` rewrite); all snapshot logic stays in the driver, out of the shared file.
- **Task 7 — Wallpaper boot + POI stub + guards** — `GM_WALLPAPER` boot, title-map rotation, stub POI, and the one-hunk `#ifdef WALLPAPER_BUILD` guards in openttd/window/viewport/main_gui. Adds the `WALLPAPER_BUILD`/`WALLPAPER_PERF` cmake defines.
- **Task 8 — GLES video driver + JNI** — the `sdl-gles` driver: EGL lifecycle, `RecordSnapshot`, snapshot replay with PaletteID→remap resolution, context/surface recovery, and all JNI natives (the fix for the Stage-1-entry `UnsatisfiedLinkError`).
- **Task 9 — On-device gate + docs** — install/launch on the physical device, verify rendering + animation + map switch + pause, then log deviations and append the Stage 1 result.
- **Out of scope** — Stage 2 candidates: real POI scanner, WallpaperService lifecycle, `GLES_PERF_SCOPE` macro sites, `StateGameLoop` timing, config-load revisit, perf tuning.
- **Self-Review / Confidence Survey / Reconciliation Log** — write-time spec-coverage check, the open plan-confidence questions (iteration 2: Q2.x), and the append-only decision log.

## Global Constraints

- **Sole build gate:** `JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug` → `BUILD SUCCESSFUL`. Desktop builds are NOT verified (Stage 0 precedent, Q1.6).
- Build takes 5–30 min. Run with a 10-min timeout and **re-run on timeout** — gradle/cmake are incremental; timeout ≠ failure. Iterate to a definitive result.
- **Do NOT modify branch `gles`** — it is read-only reference. Extract with `git show gles:<path>`.
- **Do NOT commit `android/local.properties`** (machine-local, gitignored).
- Mergeability rules from `docs/wallpaper/engine-changes.md` are binding: new headers over insertions; hooks over rewrites; one-hunk early-return guards at function top; additive-only interface edits; upstream code gated with `#ifdef WALLPAPER_BUILD`, never deleted.
- Reimpl DROPs are binding (overview.md): no smooth scroll / camera interpolation, no PBO/game-thread pixel pipeline, no legacy `GLESBackend::Paint()`, no gfxstream workarounds, no 192px record margin, no atlas layer-copy growth, no priority pre-warm loop after atlas clear. Instrumentation (`_gles_perf`, PERF logs, GPU timer queries, [CTX]/[LOAD] logs, crash handler) is KEPT, but counter *collection* is gated (Q1.8: full gating): every `_gles_perf.*` write routes through a `GLES_PERF_COUNT(stmt)` macro (defined Task 1) that compiles to a no-op when `WALLPAPER_PERF` is undefined, so an OFF build does zero perf bookkeeping. The `_gles_perf` global still links unconditionally.
- Commit per task, message style: extremely concise, ends with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Python: `/usr/bin/python3` only.
- Target: physical arm64-v8a device (connected; Pixel 10 Pro). Emulator is NOT a target.

## Spec deviations taken by this plan (flag to reviewer)

1. **"Explicit coords in BlitterParams" (overview.md) — TAKEN (spec #7 upheld, per Q1.3).** `BlitterParams` gains `sprite_x, sprite_y` (absolute screen coords) + `sprite_id` + `pal`; `Blitter_Snapshot::Draw` reads the explicit coords and the reference's `SetRecordingBuffer` + recording-buffer pointer-math are removed. **Open risk (Q2.1):** deriving absolute screen coords inside `GfxBlitter` across the DrawPixelInfo/zoom relationship is non-trivial — the exact reason the reference used pointer-math (`bp.dst` comes from a `MoveTo` chain off `dpi->dst_ptr`). The realization (coord source, and whether to keep the reference pointer-math after all) is reopened as Q2.1 before Task 1/5 start.
2. **`DrawCommand.palette` (PaletteID) replaces the remap ptr** per spec; consequence: text-recolour draws (`SetColourRemap`) record `PAL_NONE` and render un-remapped. Acceptable: strings are skipped in wallpaper frames.
3. **`gles_poi.cpp` is a stub in Stage 1** (camera = map center). Real POI scanner (648 lines, wallpaper-mode.md) is Stage 2. The `gles_poi.h` API is kept identical so Stage 2 is a file-body swap.
4. **`GLESPerfCounters` copied verbatim** into new `src/video/gles_perf.h` (rule: new header, not gfx_func.h). Unused PBO-era fields stay — harmless, pruning deferred to Stage 2.

## File Structure (whole stage)

| File | Status | Responsibility |
|---|---|---|
| `src/video/gles_perf.h` | NEW | `GLESPerfCounters` struct, `extern _gles_perf`, `_gles_video_active`, `_gles_context_lost` |
| `src/video/draw_snapshot.{h,cpp}` | NEW | slim `DrawCommand`/`DrawSnapshot`, `SnapshotTripleBuffer`, recording API + globals impl |
| `src/blitter/snapshot.{cpp,hpp}` | NEW | recording blitter (Draw→record; Encode→metadata only) |
| `src/table/gles_shader.h` | NEW | 7 GLSL ES 300 shaders (verbatim) |
| `src/video/gles_sprite.{h,cpp}` | NEW | atlas packer + GL-thread GRF decode (PBO/Enqueue machinery stripped) |
| `src/video/gles_backend.{h,cpp}` | NEW | GL state, batching, MRT FBO, PaintFBO/BlitToScreen (legacy Paint stripped) |
| `src/video/sdl2_gles_v.{cpp,h}` | NEW | driver: EGL lifecycle, RecordSnapshot, replay, JNI, recovery, crash handler |
| `src/video/gles_poi.{h,cpp}` | NEW | POI API; Stage-1 stub bodies |
| `src/wallpaper.{cpp,h}` | NEW | title-map rotation, `LoadWallpaperGame` |
| `src/blitter/base.hpp`, `factory.hpp`, `spriteloader/spriteloader.hpp`, `gfx.cpp` | EDIT | additive interface hooks |
| `src/spritecache.{cpp,h}`, `random_access_file.{cpp,h}` (+`.hpp` variants), `spriteloader/sprite_file*.{cpp,h*}`, `gfxinit.cpp` | EDIT | memory-backed GRF support (additive) |
| `src/video/video_driver.{cpp,hpp}` | EDIT | snapshot buffer member, pause CV, virtual hooks (NOT the reference rewrite) |
| `src/video/sdl2_v.cpp` | EDIT | app background/foreground + device-reset + exposed events |
| `src/openttd.{cpp,h}`, `main_gui.cpp`, `window.cpp`, `viewport.cpp` | EDIT | GM_WALLPAPER boot + one-hunk guards |
| `src/CMakeLists.txt`, `src/video/CMakeLists.txt`, `src/blitter/CMakeLists.txt` | EDIT | register new sources |

Dependency order = task order. Tasks 1–8 gate on green APK build; Task 9 is the on-device gate.

---

### Task 1: Interfaces, perf header, snapshot data structures

**Files:**
- Create: `src/video/gles_perf.h`, `src/video/draw_snapshot.h`, `src/video/draw_snapshot.cpp`
- Modify: `src/blitter/base.hpp` (~line 32 `BlitterParams`), `src/blitter/factory.hpp` (move `GetActiveBlitter` to public), `src/spriteloader/spriteloader.hpp` (4-arg `Encode`), `src/spritecache.cpp` (`ReadSprite` → 4-arg call), `src/gfx.cpp` (fill new fields), `src/video/CMakeLists.txt`

**Interfaces produced (later tasks rely on these exact names):**
- `struct DrawCommand { BlitterMode mode; int16_t x, y; SpriteID sprite; PaletteID palette; int16_t width, height, skip_left, skip_top, sprite_width, sprite_height; ZoomLevel zoom; }`
- `struct DrawSnapshot { uint64_t frame_id; std::vector<DrawCommand> commands; std::array<uint32_t, 256> palette; int viewport_width, viewport_height; void Clear(); }`
- `SnapshotTripleBuffer` — copy verbatim from `gles:src/video/draw_snapshot.h` (`GetWriteBuffer/Publish/Acquire/GetReadBuffer/ResetMetrics`, public `gpu_frame_id`)
- `void StartRecording(DrawSnapshot&); void StopRecording(); bool IsRecording(); void RecordCommand(const DrawCommand&);` — **no `RecordDirtyRect`** (dirty_rects dropped)
- `GLESPerfCounters` + `extern GLESPerfCounters _gles_perf; extern bool _gles_video_active; extern bool _gles_context_lost;` in `gles_perf.h`
- `Blitter::BlitterParams` += `int sprite_x, sprite_y; SpriteID sprite_id; PaletteID pal = PAL_NONE;` (appended, additive; `sprite_x/sprite_y` = absolute screen coords per Q1.3 — see Q2.1 for the derivation)
- `SpriteLoader... Sprite *Encode(SpriteType, const SpriteCollection&, SpriteAllocator&, SpriteID sprite_id)` non-virtual wrapper + `protected: SpriteID encoding_sprite_id_ = UINT32_MAX;`
- `BlitterFactory::GetActiveBlitter()` public

- [ ] **Step 1: Create `src/video/gles_perf.h`**

Extract the `GLESPerfCounters` struct verbatim from the gles branch (it currently lives inside gfx_func.h there):

```bash
git -C ~/work/OpenTTD show gles:src/gfx_func.h | sed -n '/^struct GLESPerfCounters/,/^};/p'
```

Wrap in a new header with GPL header comment (copy from any src file), guard `GLES_PERF_H`, plus:

```cpp
extern GLESPerfCounters _gles_perf;
extern bool _gles_video_active;
extern bool _gles_context_lost;

/* Q1.8 (full gating): route every `_gles_perf.*` write through this macro so a
 * WALLPAPER_PERF-off build does zero perf bookkeeping. Statement-wrapping form
 * covers all write kinds — `GLES_PERF_COUNT(_gles_perf.encode_total++);`,
 * `GLES_PERF_COUNT(_gles_perf.snap_commands = n);`, etc. The `_gles_perf` global
 * still links unconditionally (see draw_snapshot.cpp). */
#ifdef WALLPAPER_PERF
#define GLES_PERF_COUNT(stmt) do { stmt; } while (0)
#else
#define GLES_PERF_COUNT(stmt) do { } while (0)
#endif
```

- [ ] **Step 2: Create `src/video/draw_snapshot.h`**

Base: `git -C ~/work/OpenTTD show gles:src/video/draw_snapshot.h`. Apply the slim-DrawSnapshot decision — the header shrinks to: `DrawCommand` and `DrawSnapshot` exactly as in *Interfaces produced* above (drop `Type` enum, `remap` ptr, `StagedSprite`, `staged`, `dirty_rects`, `full_redraw`, all `scrollpos_*`/`scroll_zoom` fields, `SnapshotSpriteKey` alias), `SnapshotTripleBuffer` verbatim, recording API declarations minus `RecordDirtyRect`. `DrawSnapshot::Clear()` = `commands.clear(); frame_id = 0;`.

- [ ] **Step 3: Create `src/video/draw_snapshot.cpp`**

```cpp
#include "../stdafx.h"
#include "draw_snapshot.h"
#include "gles_perf.h"
#include "../safeguards.h"

GLESPerfCounters _gles_perf;
bool _gles_video_active = false;
bool _gles_context_lost = false;

static DrawSnapshot *_recording_snapshot = nullptr;

void StartRecording(DrawSnapshot &snapshot)
{
	snapshot.Clear();
	_recording_snapshot = &snapshot;
}

void StopRecording() { _recording_snapshot = nullptr; }
bool IsRecording() { return _recording_snapshot != nullptr; }

void RecordCommand(const DrawCommand &cmd)
{
	if (_recording_snapshot != nullptr) _recording_snapshot->commands.push_back(cmd);
}
```

Note: globals live here (not gfx.cpp) per mergeability rule 1. Do NOT add them to gfx.cpp as the reference did. The `_gles_perf` global is defined **unconditionally** (not under `#ifdef WALLPAPER_PERF`) — the symbol must always link because every counter write in later tasks is wrapped in `GLES_PERF_COUNT(...)`, which compiles to a no-op (but still references the global at ON builds). Per Q1.8 (full gating): all `_gles_perf.*` writes — increments, assignments, `max`, GPU-timer reads, and the 500 ms PERF log block — go through the macro; nothing touches a `_gles_perf` field directly.

- [ ] **Step 4: Interface edits**

`src/blitter/base.hpp` — append to `BlitterParams` after the `remap` member:

```cpp
		int sprite_x, sprite_y;      ///< Absolute screen coords of the sprite top-left (Q1.3: explicit coords)
		SpriteID sprite_id;          ///< Stable sprite identifier for GPU atlas keying
		PaletteID pal = PAL_NONE;    ///< Palette that produced `remap` (GL thread re-resolves it)
```

(add `#include "../gfx_type.h"` only if PaletteID unresolved — it almost certainly already is via existing includes). `sprite_x/sprite_y` are filled in Step 5; the exact derivation is Q2.1 — if Q2.1 reverts to pointer-math, drop these two fields and this whole explicit-coords fold.

`src/blitter/factory.hpp` — move the entire `static std::unique_ptr<Blitter> &GetActiveBlitter()` method from `private:` to `public:` (see `git -C ~/work/OpenTTD diff e24f92ce82..gles -- src/blitter/factory.hpp` — apply the same move to current code).

`src/spriteloader/spriteloader.hpp` — after the pure-virtual `Encode`, append:

```cpp
	/** Encode with known SpriteID. Sets encoding_sprite_id_ before calling virtual Encode(). */
	Sprite *Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator, SpriteID sprite_id)
	{
		this->encoding_sprite_id_ = sprite_id;
		auto *result = this->Encode(sprite_type, sprite, allocator);
		this->encoding_sprite_id_ = UINT32_MAX;
		return result;
	}
```

and at class end: `protected: SpriteID encoding_sprite_id_ = UINT32_MAX;`.

`src/spritecache.cpp` — in `ReadSprite`, find the `encoder->Encode(...)` call and pass the sprite id as 4th arg (the function already has the sprite `id` in scope; check the exact local name).

- [ ] **Step 5: gfx.cpp fills**

In `GfxBlitter` (gfx.cpp:1070; the `bp.*` setup runs ~line 1111–1155, `bp.remap = _colour_remap_ptr;` at 1119):

```cpp
	bp.sprite_id = sprite_id;
	bp.pal = _colour_remap_pal;
	bp.sprite_x = /* absolute screen X of sprite top-left — Q2.1 */;
	bp.sprite_y = /* absolute screen Y of sprite top-left — Q2.1 */;
```

**Q2.1 (open):** the absolute screen coord is NOT a single variable in `GfxBlitter`. Candidate derivation: `bp.sprite_x = dpi->left + bp.left; bp.sprite_y = dpi->top + bp.top;` (`bp.left/top` are set to `x_unscaled/y_unscaled` at 1136/1155; `dpi->left/top` are the DrawPixelInfo's screen offset) — but the zoomed-viewport DPI needs verification. This is the crux the reference sidestepped with pointer-math; do not implement Task 1/5 until Q2.1 is answered.

Add file-static next to `_colour_remap_ptr`'s definition: `static PaletteID _colour_remap_pal = PAL_NONE;`
Set it at the sprite-remap sites in `DrawSpriteViewport` (gfx.cpp:1014/1020) and `DrawSprite` (1042/1048) — each `_colour_remap_ptr = GetNonSprite(...) + 1;` line gets a sibling `_colour_remap_pal = <the pal expression passed to GetNonSprite>;` — and reset `_colour_remap_pal = PAL_NONE;` inside `SetColourRemap()` (text recolour → recorded as no-remap, see deviation 2). To be safe, also set `_colour_remap_pal = PAL_NONE;` at the top of both `DrawSpriteViewport` and `DrawSprite` (`GfxBlitter` reads `pal` only when mode ≠ Normal).

- [ ] **Step 6: Register sources**

`src/video/CMakeLists.txt`, inside `if(NOT OPTION_DEDICATED)`:

```cmake
    add_files(
        draw_snapshot.cpp
        draw_snapshot.h
        gles_perf.h
    )
```

- [ ] **Step 7: Build**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```

Expected: `BUILD SUCCESSFUL`. (Nothing consumes the new code yet — this validates the interface edits against current upstream.)

- [ ] **Step 8: Commit**

```bash
git -C ~/work/OpenTTD add src/video/gles_perf.h src/video/draw_snapshot.h src/video/draw_snapshot.cpp src/blitter/base.hpp src/blitter/factory.hpp src/spriteloader/spriteloader.hpp src/spritecache.cpp src/gfx.cpp src/video/CMakeLists.txt
git -C ~/work/OpenTTD commit -m "GLES: snapshot structs, perf header, blitter interface hooks

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Sprite-cache memory backing (GL-thread decode enabler)

**Files:**
- Modify: `src/spritecache.cpp`, `src/spritecache.h`, `src/random_access_file_type.h`, `src/random_access_file.cpp`, `src/spriteloader/sprite_file.cpp`, `src/spriteloader/sprite_file_type.hpp`, `src/gfxinit.cpp`

**Interfaces produced:**
- `void BufferSpriteFilesToMemory(); void SaveSpriteFileBuffers(); SpriteID GetRegisteredSpriteCount();`
- `struct SpriteCacheInfo { SpriteFile *file; size_t file_pos; SpriteType type; SpriteCacheCtrlFlags control_flags; }; bool GetSpriteCacheInfo(SpriteID id, SpriteCacheInfo &out);`
- `RandomAccessFile` memory-backed constructor `(const uint8_t *data, size_t size, const std::string &filename, size_t base_offset)` with mem-mode branches in `ReadByte/ReadBlock/SeekTo/SkipBytes/GetPos`
- `SpriteFile` memory-backed ctor, `LoadIntoMemory()`, `GetMemoryData()/GetMemorySize()`, `TakeMemoryBuffer()/SetMemoryBuffer()`, `GetContentBegin()`

- [ ] **Step 1: Extract the reference diffs**

```bash
git -C ~/work/OpenTTD diff e24f92ce82..gles -- src/spritecache.cpp src/spritecache.h src/random_access_file_type.h src/random_access_file.cpp src/spriteloader/sprite_file.cpp src/spriteloader/sprite_file_type.hpp > /tmp/openttd/memback.diff
```

- [ ] **Step 2: Apply to current sources**

Try `git -C ~/work/OpenTTD apply --3way /tmp/openttd/memback.diff`; on conflicts, re-apply hunks by hand against 114-days-newer upstream. All changes are **additive** (new functions/ctors/branches) — do not restructure existing code. Skip any hunk that only adds `_gles_perf` sprite-loading counters if its context drifted badly (counters can be re-added in Stage 2); keep all functional hunks.

- [ ] **Step 3: gfxinit hook**

In `src/gfxinit.cpp` `GfxLoadSprites()`, per reference: call `SaveSpriteFileBuffers()` before sprite tables load and `BufferSpriteFilesToMemory()` after (exact placement in the reference diff). Do NOT call `BuildGLSpriteFiles()` yet (atlas doesn't exist until Task 3) — leave a `/* Stage1 Task5 adds BuildGLSpriteFiles() here */` marker only if the reference hunk includes it; the call is added in Task 5.

- [ ] **Step 4: Build** — same command as Task 1. Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 5: Commit** — `git add -u && git commit -m "Sprite cache: RAM-backed GRFs for GL-thread decode ..."` (same trailer).

---

### Task 3: GLES shaders + sprite atlas (GL-thread on-demand decode only)

**Files:**
- Create: `src/table/gles_shader.h` (verbatim: `git show gles:src/table/gles_shader.h`)
- Create: `src/video/gles_sprite.h`, `src/video/gles_sprite.cpp` (ported, stripped)
- Modify: `src/video/CMakeLists.txt`, `src/table/CMakeLists.txt` (if table files are listed there — mirror how `opengl_shader.h`-like tables are registered; otherwise header needs no registration)

**Interfaces produced (used by Task 4/5):**
- `using GLESSpriteID = uint64_t;` `MakeGLESSpriteKey(SpriteID, ZoomLevel)`, `kGPUScaleBaseZoom = ZoomLevel::In4x`
- `GLESSpriteRegion`, `GLESSpriteEntry` (unchanged from reference)
- `class GLESSpriteAtlas` public API kept: `Init/Destroy/DeleteGLObjects/RequestClear/ProcessPendingClear/AbandonGLObjects/Upload/LookupOrUpload/LoadSpriteOnGLThread/BuildGLSpriteFiles/ResetFrameLoadCounter/Lookup/CacheMeta/GetCachedMeta/GetColourTexture/GetRemapTexture/GetSpriteCount/GetColourPageCount/GetRemapPageCount/GetColourOccupancyPercent/GetRemapOccupancyPercent/IsPostClearMonitoring/TickPostClearFrame` (no `PrewarmAfterClear` — DECISION (reimpl): dropped, see rendering.md)

**Strip list (delete from both header and cpp — per GL-thread-only decision):** `PBOPendingEntry`, `PBOUploadBatch`, `GLESUploadRequest`, `Enqueue`, `upload_queue`, `queue_mutex`, `known_sprites`, `known_clear_pending`, `IsKnown`, `GetStagedCount`, `GetStagedBytes`, `ProcessPBOUploads`, `PBOCheckInflight`, `PBOFillBatch`, `PBOSubmitBatch`, `pbo_*` members, `pbo_staging`, `upload_rgba_buf`/`upload_m_buf` **only if** they're PBO-only (they're used by `Upload()` pixel conversion — check; keep if `Upload` uses them). In `AbandonGLObjects` drop the PBO/known_sprites lines.

**Also strip (gfxstream workarounds — write plain GL):** CPU-staging-instead-of-glMapBufferRange comments/paths, glGenTextures error-drain/fake-handle bail, two-pass PackRegion-before-bind ordering (keep natural order), in-place clear: `ClearSprites()` becomes plain `glDeleteTextures` + recreate via `Init()`-style realloc. **Q1.7 (planned fallback):** delete+realloc ships as primary; if the Task 9 device gate shows artifacts/flicker on map switch, a follow-up commit reinstates the reference in-place clear — Task 9 Step 5 records which path shipped. This is a defined conditional, not an open question.

- [ ] **Step 1: Extract files**

```bash
mkdir -p ~/work/OpenTTD/src/table 2>/dev/null
git -C ~/work/OpenTTD show gles:src/table/gles_shader.h > ~/work/OpenTTD/src/table/gles_shader.h
git -C ~/work/OpenTTD show gles:src/video/gles_sprite.h > ~/work/OpenTTD/src/video/gles_sprite.h
git -C ~/work/OpenTTD show gles:src/video/gles_sprite.cpp > ~/work/OpenTTD/src/video/gles_sprite.cpp
```

- [ ] **Step 2: Apply strip list** to gles_sprite.{h,cpp}. Keep: shelf packer (`AllocPage/PackRegion`), `Upload`, `LookupOrUpload` (miss → `LoadSpriteOnGLThread`, budget `MAX_LOADS_PER_FRAME = 500`, placeholder entry), `LoadSpriteOnGLThread` (decode via `SpriteLoaderGrf` over `gl_sprite_files`), `BuildGLSpriteFiles`, `meta_cache` + `CacheMeta/GetCachedMeta`, deferred clear (`clear_pending` atomic + `ProcessPendingClear`), post-clear monitoring, occupancy queries. Atlas-full handling: `PackRegion` failure on a new layer → `RequestClear()` and return placeholder (clear + on-demand reload repopulates; no `glCopyTexSubImage3D` growth path — delete it). DECISION (reimpl): do NOT port the reference's priority pre-warm sort (`PrewarmAfterClear`, ~line 231 in `gles:src/video/gles_sprite.cpp`) — natural Z-order draw already visits ground/foundation sprites first, and atlas clears are rare enough that a few frames of incidental pop-in order aren't perceptible in spectator mode. `ProcessPendingClear()` just clears; nothing runs after it.

- [ ] **Step 3: Register sources** — `src/video/CMakeLists.txt`:

```cmake
    add_files(
        gles_sprite.cpp
        gles_sprite.h
        CONDITION SDL2_FOUND AND OPENGLES_FOUND
    )
```

(`OPENGLES_FOUND` is already set TRUE for Android in root CMakeLists.txt:188 — verify with `grep -n OPENGLES ~/work/OpenTTD/CMakeLists.txt`.)

- [ ] **Step 4: Build.** Expected: `BUILD SUCCESSFUL`. Compile errors here are almost always a missed strip-list reference — delete the dangling use, don't reintroduce the machinery.

- [ ] **Step 5: Commit** (`GLES: sprite atlas + shaders, GL-thread decode only`).

---

### Task 4: GLES backend (PaintFBO + BlitToScreen single path)

**Files:**
- Create: `src/video/gles_backend.h`, `src/video/gles_backend.cpp` (ported, stripped)
- Modify: `src/video/CMakeLists.txt`

**Interfaces produced (used by Task 5+):**
- `GLESDrawCommand` — as reference but **delete `remap_idx` and `sort_atlas`** if only used by dropped sort/legacy path (verify), keep `const uint8_t *remap` (GL-thread-resolved pointer, set by replay)
- `class GLESBackend` public API kept: `Get/Create/Destroy/Resize/UpdatePalette/GetSpriteAtlas/QueueDraw/AddDirtyRect/PaintFBO/BlitToScreen/RecoverGPUState/SetPaletteDirty/HasFBOContent/ClearQueue/GetDrawQueueSize/GetScreenWidth/GetScreenHeight/SetBrightness`
- **Changed:** `bool Paint()` deleted; `void BlitToScreen()` — no UV-offset params (interpolation dropped)

- [ ] **Step 1: Extract**

```bash
git -C ~/work/OpenTTD show gles:src/video/gles_backend.h > ~/work/OpenTTD/src/video/gles_backend.h
git -C ~/work/OpenTTD show gles:src/video/gles_backend.cpp > ~/work/OpenTTD/src/video/gles_backend.cpp
```

- [ ] **Step 2: Strip** — delete `GLESBackend::Paint()` entirely (~450 lines; `PaintFBO` is the surviving twin). Change `BlitToScreen(float u_offset, float v_offset)` → `BlitToScreen()`; delete the u/v uniform plumbing inside (brightness uniform stays). Delete any `ProcessPBOUploads` calls. Keep: MRT FBO + R8 idx attachment, batching with Z-order + shader/remap-table batch breaks, VBO orphaning, `glBindAttribLocation` unification, double-buffered remap-table textures with `last_remap_ptr` cache, palette texture, three frame classes in `PaintFBO` (full render / resolve-only / skip), `DrawDebugDirtyOverlay`, GPU timer queries (collection wrapped `#ifdef WALLPAPER_PERF`), `RecoverGPUState` (zero handles w/o glDelete → `AbandonGLObjects` → InitGLObjects+Resize → `_switch_mode = SM_WALLPAPER`). Dirty-rect scissor clear: keep the mechanism; the driver feeds one full-screen rect per replayed frame (Task 5).

- [ ] **Step 3: Register** — append `gles_backend.cpp` / `gles_backend.h` to the Task-3 `add_files` block.

- [ ] **Step 4: Build.** Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 5: Commit** (`GLES: backend — batched MRT FBO render, single PaintFBO path`).

---

### Task 5: Snapshot blitter

**Files:**
- Create: `src/blitter/snapshot.hpp`, `src/blitter/snapshot.cpp`
- Modify: `src/blitter/CMakeLists.txt`, `src/gfxinit.cpp` (add `BuildGLSpriteFiles` call)

**Interfaces produced:**
- `class Blitter_Snapshot : public Blitter` — name `"snapshot"`, 32bpp; all pixel ops no-op (as reference). **No `SetRecordingBuffer`** (Q1.3: explicit coords remove the recording buffer — `Draw` reads `bp->sprite_x/sprite_y` directly). If Q2.1 reverts to pointer-math, restore `SetRecordingBuffer(void *buf, int pitch)`.
- `FBlitter_Snapshot` factory, registered via file-static instance
- **Removed vs reference:** `FlushEarlyStaged()` + early-staging buffer (GL-thread decode obsoletes them), and `SetRecordingBuffer` + `recording_buffer`/`recording_pitch` (Q1.3, subject to Q2.1)

- [ ] **Step 1: Extract** both files from gles (`git show gles:src/blitter/snapshot.{hpp,cpp}`).

- [ ] **Step 2: Slim `Encode`** — metadata only:

```cpp
Sprite *Blitter_Snapshot::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	GLES_PERF_COUNT(_gles_perf.encode_total++);

	const auto &root = sprite.Root();
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	/* Skip font glyphs — no valid this->encoding_sprite_id_. */
	if (sprite_type == SpriteType::Font) return dest_sprite;

	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->GetSpriteAtlas().CacheMeta(this->encoding_sprite_id_,
			root.width, root.height, root.x_offs, root.y_offs);
	}
	return dest_sprite;
}
```

Delete `GetEarlyStaged`, `FlushEarlyStaged`, the zoom-variant search, and the `Enqueue` call. Remove `FlushEarlyStaged` from snapshot.hpp too.

- [ ] **Step 3: Slim `Draw`** — explicit coords (Q1.3), record PaletteID:

Take reference `Draw`, then: delete `cmd.type = ...;` and `cmd.remap = bp->remap;`; set `cmd.x = bp->sprite_x; cmd.y = bp->sprite_y;` (replacing the `recording_buffer`/`recording_pitch` pointer-subtraction that derived coords); set `cmd.palette = (mode == BlitterMode::ColourRemap || mode == BlitterMode::TransparentRemap) ? bp->pal : PAL_NONE;`. Keep `UnScaleByZoom` on sprite dims and `RecordCommand(cmd)`. `MoveTo` is no longer needed for coords (all pixel ops are no-op, so `bp->dst` is unused). **Subject to Q2.1** — if it reverts to pointer-math, this step keeps the reference `recording_buffer` coord math and `SetRecordingBuffer` instead.

- [ ] **Step 4: Register** — `src/blitter/CMakeLists.txt`:

```cmake
add_files(
    snapshot.cpp
    snapshot.hpp
    CONDITION NOT OPTION_DEDICATED AND OPENGLES_FOUND
)
```

- [ ] **Step 5: gfxinit** — in `GfxLoadSprites()` after `BufferSpriteFilesToMemory()` (Task 2 site):

```cpp
	if (GLESBackend::Get() != nullptr) GLESBackend::Get()->GetSpriteAtlas().BuildGLSpriteFiles();
```

(guarded include `"video/gles_backend.h"`).

- [ ] **Step 6: Build.** Expected: `BUILD SUCCESSFUL`.
- [ ] **Step 7: Commit** (`GLES: snapshot recording blitter`).

---

### Task 6: VideoDriver base hooks (hooks over rewrites)

**Files:**
- Modify: `src/video/video_driver.hpp`, `src/video/video_driver.cpp`

This task deliberately does NOT copy the reference's video_driver.cpp rewrite (worst merge-conflict spot per engine-changes.md rule 2). Base class gets minimal hooks; ALL snapshot logic lands in `sdl2_gles_v.cpp` (Task 8).

**Interfaces produced (Task 8 overrides these):**

```cpp
/* video_driver.hpp, protected unless noted: */
virtual void OnGameLoopDone() {}                 ///< Called after ::GameLoop() with game_state_mutex held. GLES: record snapshot.
virtual bool SnapshotTick() { return false; }    ///< Called at top of draw section of Tick(); true = handled, skip legacy path.
virtual bool RecoverContextIfLost() { return false; }  ///< public, as reference
virtual bool PaintFromSnapshot() { return false; }
public: void SetGameThreadPaused(bool paused);   ///< as reference (atomic store + cv notify)
protected:
std::atomic<bool> game_thread_paused{false};
std::mutex game_pause_mutex;
std::condition_variable game_pause_cv;
std::unique_ptr<SnapshotTripleBuffer> snapshot_buffer;  ///< nullptr = legacy mode
```

- [ ] **Step 1: video_driver.hpp** — add `#include "draw_snapshot.h"`, `<memory>`; add the members/virtuals above (mirror reference diff for `SetGameThreadPaused` + members; `OnGameLoopDone`/`SnapshotTick` are new names replacing the reference's inline rewrites). Add the reference's `GetDrawInterval()` snapshot branch verbatim:

```cpp
		/* Snapshot mode: match draw rate to game tick rate. */
		if (this->snapshot_buffer != nullptr) {
			return std::chrono::milliseconds(MILLISECONDS_PER_TICK);
		}
```

- [ ] **Step 2: video_driver.cpp — three one-hunk edits:**

(a) `GameLoop()` — after `::GameLoop();`, still inside the `game_state_mutex` scope: `this->OnGameLoopDone();`

(b) `GameThread()` — insert pause block at top of the `while (!_exit_game)` loop (reference logic, warm-up ticks included):

```cpp
		if (this->game_thread_paused.load()) {
			Debug(driver, 0, "[CTX] GameThread: paused — running 5 warm-up ticks");
			for (int i = 0; i < 5 && !_exit_game; i++) {
				this->GameLoop();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			Debug(driver, 0, "[LOAD] game_thread_sleep: entering pause wait");
			std::unique_lock<std::mutex> lock(this->game_pause_mutex);
			this->game_pause_cv.wait(lock, [this] {
				return !this->game_thread_paused.load() || _exit_game;
			});
			this->next_game_tick = std::chrono::steady_clock::now();
			Debug(driver, 0, "[LOAD] game_thread_sleep: resumed");
			continue;
		}
```

(c) `Tick()` — find where the draw section starts (after the `next_draw_tick` due-time check); insert:

```cpp
		if (this->SnapshotTick()) {
			this->next_draw_tick += this->GetDrawInterval();
			/* Avoid next_draw_tick lag when paint takes too long. */
			auto now = std::chrono::steady_clock::now();
			if (this->next_draw_tick < now) this->next_draw_tick = now;
			return;
		}
```

Read the current `Tick()` first; place the hook so the legacy `LockVideoBuffer/.../Paint` sequence is fully bypassed in snapshot mode but tick scheduling stays correct (mirror how the existing code advances `next_draw_tick`).

Also add `SetGameThreadPaused` inline in the hpp per reference:

```cpp
	void SetGameThreadPaused(bool paused)
	{
		this->game_thread_paused.store(paused);
		if (!paused) this->game_pause_cv.notify_one();
	}
```

- [ ] **Step 3: Build.** Expected: `BUILD SUCCESSFUL` (hooks are inert defaults).
- [ ] **Step 4: Commit** (`VideoDriver: snapshot/pause hooks (no logic in base)`).

---

### Task 7: Wallpaper mode boot + POI stubs + engine guards

**Files:**
- Create: `src/wallpaper.cpp`, `src/wallpaper.h` (port from gles), `src/video/gles_poi.h` (verbatim from gles), `src/video/gles_poi.cpp` (NEW stub)
- Modify: `src/openttd.h`, `src/openttd.cpp`, `src/main_gui.cpp`, `src/window.cpp`, `src/viewport.cpp`, `src/CMakeLists.txt`, `src/video/CMakeLists.txt`, root `CMakeLists.txt` (WALLPAPER_BUILD define)

**Interfaces produced:** `wallpaper.h` API verbatim (`BuildTitleFileList/CanRotateTitleMap/RequestNextTitleMap/RotateTitleMap/LoadWallpaperGame/LoadNextTitleMap`); `gles_poi.h` API verbatim (`PrepareBackground/NavigatePOI/RecenterOnCurrentPOI/InvalidatePOIs/DrawPOIMarkers`, `GlesPOI` struct); `GM_WALLPAPER`/`SM_WALLPAPER` enum values appended.

- [ ] **Step 1: root CMakeLists.txt** — in the Android block (near the OPENGLES lines ~188):

```cmake
    option(WALLPAPER_PERF "Collect GLES perf counters (_gles_perf)" ON)
    add_definitions(-DWALLPAPER_BUILD)
    if(WALLPAPER_PERF)
        add_definitions(-DWALLPAPER_PERF)
    endif()
```

`WALLPAPER_PERF` defaults ON for Stage 1 (device verification in Task 9 needs the PERF log lines) but lets a future battery-optimized release build disable counter *collection* without touching call sites — see Global Constraints.

- [ ] **Step 2: openttd.h** — append `GM_WALLPAPER` to `GameMode`, `SM_WALLPAPER` to `SwitchMode` (per reference diff; additive, keep trailing-comma hygiene).

- [ ] **Step 3: wallpaper.{cpp,h}** — extract verbatim from gles; register in `src/CMakeLists.txt` `add_files` (alphabetical position, unconditional as reference). Fix compile errors from upstream drift only (API renames), no redesign.

- [ ] **Step 4: gles_poi.h verbatim; gles_poi.cpp stub:**

```cpp
/* (GPL header) */
/** @file gles_poi.cpp Stage-1 STUB — camera centers on map. Real POI scanner lands in Stage 2 (see docs/wallpaper/wallpaper-mode.md). */
#include "../stdafx.h"
#include "gles_poi.h"
#include "../map_func.h"
#include "../viewport_func.h"
#include "../window_func.h"
#include "../window_gui.h"
#include "../debug.h"
#include "../safeguards.h"

void PrepareBackground()
{
	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr) return;
	ScrollWindowToTile(TileXY(Map::SizeX() / 2, Map::SizeY() / 2), w, true);
	Debug(misc, 1, "POI(stub): centered on map");
}

void NavigatePOI(int) { PrepareBackground(); }
void RecenterOnCurrentPOI() { PrepareBackground(); }
void InvalidatePOIs() {}
void DrawPOIMarkers(const Viewport &) {}
```

(Adjust include set / `ScrollWindowToTile` signature to current upstream — check `src/viewport_func.h`.) Register in `src/video/CMakeLists.txt` unconditionally (as reference did).

- [ ] **Step 5: openttd.cpp boot** — apply the reference diff (`git diff e24f92ce82..gles -- src/openttd.cpp`) with these changes:
  - `_game_mode = GM_WALLPAPER; _switch_mode = SM_WALLPAPER;` at init — **gate with `#ifdef WALLPAPER_BUILD` keeping the upstream lines in `#else`**.
  - sound/music: force `sounddriver = "null"; musicdriver = "null";` and skip `BaseSounds/BaseMusic::FindSets` — wrap the upstream block in `#ifndef WALLPAPER_BUILD` (rule 6: gate, don't delete).
  - `LoadFromConfig`/`LoadFromHighScore`/`LoadHotkeysFromConfig`/`WindowDesc::LoadFromConfig` — gate under `#ifndef WALLPAPER_BUILD` (Q1.6: config load intentionally disabled in Stage 1 — defaults only; code gated, not deleted). Marker: `// Stage 1: config load disabled by design (defaults only); Stage 2 revisits — see Out of scope`.
  - `GenerateWorld(GWM_EMPTY, 64, 64); LoadWallpaperGame(); _switch_mode = SM_NONE;` replacing `LoadIntroGame(false)` — gated likewise.
  - `#ifdef __ANDROID__ if (videodriver.empty()) videodriver = "sdl-gles"; #endif` before `SelectDriver(..., Type::Video)`.
  - `SafeLoad`: add `case GM_WALLPAPER: LoadWallpaperGame(); break;`.
  - `LoadIntroGame`: apply reference changes (InvalidatePOIs + atlas RequestClear + `LoadNextTitleMap()` + `PrepareBackground()`) gated `#ifdef WALLPAPER_BUILD` where they replace upstream behavior.
  - SKIP: `StateGameLoop` timing instrumentation hunks (Stage 2), `prctl` debug remnants.

- [ ] **Step 6: main_gui.cpp** — from reference diff take: `FixTitleGameZoom` accepts `GM_WALLPAPER`; MainWindow `InitializeViewport(..., ZoomLevel::In4x)`; `OnPaint` = `DrawWidgets()` only + empty `OnClick` (gate title-screen drawing with `#ifndef WALLPAPER_BUILD`, don't delete); `SetupColoursAndInitialWindow` — `GM_WALLPAPER` falls through with `GM_MENU`, and gate `ShowSelectGameWindow()` out under WALLPAPER_BUILD; `GameSizeChanged` wallpaper branch (FixTitleGameZoom + RecenterOnCurrentPOI; dimension change → `_switch_mode = SM_WALLPAPER`). SKIP the Debug() noise lines and the cosmetic OnScroll refactor.

- [ ] **Step 7: window.cpp / viewport.cpp guards** (one-hunk each, from reference diff via `git diff e24f92ce82..gles -- src/window.cpp src/viewport.cpp`):
  - `HandleMouseEvents()`: `if (_game_mode == GM_WALLPAPER) return ES_NOT_HANDLED;`-style early return at top (match current return type).
  - `DrawOverlappedWindowForAll`: wallpaper fast path at top — paint only `WC_MAIN_WINDOW`, skip overlap logic (copy reference hunk).
  - `UpdateWindows`: skip chat/cursor drawing in wallpaper (reference hunk); SKIP the every-2nd-frame throttle (DROP) and order changes unless the reference hunk applies cleanly as-is.
  - `viewport.cpp ViewportDoDraw`: in wallpaper skip `ViewportAddKdtreeSigns` + `DrawTextEffects` + string sprites (reference guards); include `DrawPOIMarkers` call (stub no-ops). SKIP `_gles_perf` phase-timing hunks (Stage 2 instrumentation task) and forced-smooth-scroll (DROP).
  - `SetViewportPosition`: disable the scroll-blit path in snapshot mode (reference hunk — early path selection, snapshot re-records full frames).

- [ ] **Step 8: Build.** Expected: `BUILD SUCCESSFUL`.
- [ ] **Step 9: Commit** (`Wallpaper mode: GM_WALLPAPER boot, title rotation, POI stubs, guards`).

---

### Task 8: GLES video driver + JNI + SDL event hooks

**Files:**
- Create: `src/video/sdl2_gles_v.h` (from gles, minus interpolation members), `src/video/sdl2_gles_v.cpp` (from gles, heavily adapted — see below)
- Modify: `src/video/sdl2_v.cpp` (event hooks), `src/video/CMakeLists.txt`

**Interfaces produced:** driver `"sdl-gles"` (factory `FVideoDriver_SDL_GLES`, priority 10); JNI exports exactly 1:1 with the Java glue:
`Java_org_openttd_android_GameActivity_{nativeRotateMap,nativeNavigatePOI,nativeScrollCamera,nativeSetGamePaused,nativeSetBrightness}` and `Java_org_openttd_android_OpenTTDWallpaperService_{nativePrepareBackground,nativeSwitchMap,nativeNavigatePOI,nativeRotateMap,nativeScrollCamera,nativeSetGamePaused,nativeSetBrightness,nativeSurfaceChanged}`. **Q1.5:** the Java `nativeCycleZoom` declaration (OpenTTDWallpaperService.java:31, no callers) is REMOVED in Step 2b rather than stubbed — zoom-cycle isn't Stage 1 scope. Acceptance: `grep "private static native" .../*.java` count == number of `Java_..._native*` exports (assert 1:1, no orphans in either direction).

- [ ] **Step 1: Extract both files from gles.** Then adapt `sdl2_gles_v.cpp`:
  - **Delete** (smooth-scroll DROP): `snap_prev_/snap_curr_scrollpos_*`, `snap_scroll_zoom`, `snap_time` members; interpolation math in `PaintFromSnapshot`; call `BlitToScreen()` with no args.
  - **Move IN from reference video_driver.cpp** (they lived in base there; reimpl puts driver logic in the driver):
    - `RecordSnapshot()` — reference version minus the 192px margin machinery: no viewport/window dimension mutation, no `SNAP_MARGIN`, no command coordinate shift. **Q1.3:** no recording buffer and no `SetRecordingBuffer` call (explicit coords come from `bp->sprite_x/sprite_y`); just full-area `RedrawScreenRect(0, 0, w, h)` (snapshot blitter no-ops all pixel writes), coordinate validation loop, palette fill from `_cur_palette`, `Publish()`, snapshot counters via `GLES_PERF_COUNT(...)`. Drop the scroll-state recording block (fields no longer exist). Override: `void OnGameLoopDone() override` → `{ UpdateViewportPosition(main window); RecordSnapshot(); }` (reference GameLoop hunk shows the exact UpdateViewportPosition call). **If Q2.1 reverts to pointer-math:** restore the dummy buffer (`_screen.width * _screen.height * 4`) + `SetRecordingBuffer`.
    - `ProcessOverlayActions()` — reference version (drain JNI atomics: `_gles_jump_waypoint` → `PrepareBackground()`, `_gles_navigate_poi` → `NavigatePOI`, `_gles_rotate_map` → `RotateTitleMap`, scroll atomics → `dest_scrollpos += ScaleByZoom(...)`; also `DrainCommandQueue()` + `while (PollEvent()) {}`). Lives as private driver method.
    - `SnapshotTick() override` → `{ ProcessOverlayActions(); if (this->RecoverContextIfLost()) return true; this->PaintFromSnapshot(); return true; }` (only when `snapshot_buffer != nullptr`; return false otherwise so pre-Start ticks fall through).
    - Android crash handler (`_Unwind_Backtrace` SIGSEGV/SIGABRT installer) — move here under `#ifdef __ANDROID__`.
  - **Replay remap resolution** (PaletteID → table ptr, replaces `cmd.remap` passthrough) — add file-static and use in the replay loop where GLESDrawCommand is filled:

```cpp
#include "../spritecache.h"
#include "../table/palettes.h"  /* if needed for PALETTE_WIDTH; else core/bitmath GB */

/** Resolve a recorded PaletteID to a 256-byte remap table on the GL thread.
 *  Recolour sprites are stable once loaded (same assumption the reference
 *  made for its raw pointer). Returns nullptr when there is no remap. */
static const uint8_t *ResolveRemap(PaletteID pal)
{
	if (pal == PAL_NONE) return nullptr;
	return GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1;
}
```

In the replay loop: `gcmd.remap = ResolveRemap(cmd.palette);` — if it resolves to nullptr while `cmd.mode == BlitterMode::ColourRemap`, downgrade that command to `BlitterMode::Normal`.
  - **Dirty rects:** each replayed frame with non-empty commands → `backend->AddDirtyRect(0, 0, backend->GetScreenWidth(), backend->GetScreenHeight());` (full-frame recording ⇒ full clear).
  - **Driver `Start()`:** after GL context up — `BlitterFactory::SelectBlitter("snapshot")` if not already active (check reference Start for exact placement), `_gles_video_active = true`, `snapshot_buffer = std::make_unique<SnapshotTripleBuffer>()`, `GLESBackend::Create()` etc. per reference. Keep EGL surface-rebind path (`_gles_surface_changed` manual `eglCreateWindowSurface`) and full `RecoverContextIfLost()` logic — this is the Android-essential section of rendering.md, port faithfully.
  - Keep: deferred missing-sprite upload after `SDL_GL_SwapWindow` (`deferred_upload_keys`), `video_buffer`/`AllocateBackingStore`/`GetVideoPointer` as reference, `CheckPaletteAnim`, PERF logging block (500 ms cadence) — every `_gles_perf.*` write AND the log line wrapped in `GLES_PERF_COUNT(...)` (Q1.8), so an OFF build compiles the block to nothing.
  - JNI functions from reference kept (atomics + `SetGameThreadPaused` + `SetBrightness` + `_gles_surface_changed`), export names 1:1 with the Java glue.
- [ ] **Step 2b: Remove `nativeCycleZoom` (Q1.5)** — delete the `private static native void nativeCycleZoom();` line at OpenTTDWallpaperService.java:31 (grep-confirmed: no callers). Then assert 1:1 Java-native↔JNI-export both directions; no orphan on either side.

- [ ] **Step 2: sdl2_v.cpp hooks** (reference diff, SKIP the desktop POI hotkeys — DROP): `SDL_APP_DIDENTERBACKGROUND` → `SetGameThreadPaused(true)`; `SDL_APP_WILLENTERFOREGROUND` → `SetGameThreadPaused(false)`; `SDL_RENDER_DEVICE_RESET` → `_gles_context_lost = true;`; `SDL_WINDOWEVENT_EXPOSED` → reset `snapshot_buffer->gpu_frame_id = 0` when snapshot mode. Include `"gles_perf.h"`.

- [ ] **Step 3: Register** — `src/video/CMakeLists.txt`: append `sdl2_gles_v.cpp` / `sdl2_gles_v.h` to the `SDL2_FOUND AND OPENGLES_FOUND` block.

- [ ] **Step 4: Build.** Expected: `BUILD SUCCESSFUL`. This is the task where drift bites (SDL driver base class evolved 114 days) — fix mechanically against current `sdl2_v.h`/`video_driver.hpp`, keep changes inside the new files.

- [ ] **Step 5: Commit** (`GLES: sdl-gles driver — EGL lifecycle, snapshot replay, JNI`).

---

### Task 9: On-device integration gate + docs

**Files:**
- Modify: `docs/wallpaper/engine-changes.md` (log deviations), this plan (Stage 1 Result section)

- [ ] **Step 1: Install + launch**

**Asset precondition (Q1.1):** the asset-packaging wart (fresh build can produce an assetless APK) is deferred to Stage 2, not fixed here. Before installing, if a prior build's assets aren't already packaged, expect `Error extracting baseset assets` on launch — rebuild+reinstall once (assets land on the second build). Exact guard wording is Q2.3.

```bash
adb install -r ~/work/OpenTTD/android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -c
adb shell am start -n org.openttd.android/.GameActivity
sleep 10 && adb logcat -d | grep -E "OpenTTD|SDL|AndroidRuntime|FATAL" | tail -60
```

Expected: NO `UnsatisfiedLinkError`, NO `FATAL`; logcat shows `Successfully loaded blitter 'snapshot'`, `[CTX] GameThread: started`, PERF lines within ~1 s.

- [ ] **Step 2: Visual verification**

```bash
adb exec-out screencap -p > /tmp/openttd/stage1_frame1.png
sleep 5
adb exec-out screencap -p > /tmp/openttd/stage1_frame2.png
```

Read both PNGs: a rendered isometric map (not black, not placeholder-grey soup) and frame2 ≠ frame1 (water animation / simulation movement). If black screen: check `[CTX]` logs for context loss loops and `snap_commands`/`gpu_batches` PERF values (0 commands = recording problem; commands >0 but black = replay/batch problem).

- [ ] **Step 3: Control surface**

```bash
adb shell am broadcast -a org.openttd.android.SWITCH_MAP   # → RotateTitleMap: map reloads, camera recenters
adb shell am broadcast -a org.openttd.android.JUMP_POI     # → stub: recenters (no crash)
```

Plus tap Map </> buttons in GameActivity via screen if convenient. Expected: map switch visibly reloads; no crashes; logcat shows atlas clear + reload (no prewarm step — dropped, see rendering.md).

- [ ] **Step 4: Pause/resume** — `adb shell input keyevent KEYCODE_HOME` then relaunch: logcat shows `game_thread_sleep: entering pause wait` on hide and `resumed` on return; rendering resumes.

- [ ] **Step 5: Docs + result**
  - `docs/wallpaper/engine-changes.md`: append a `## Stage 1 reimpl log` noting: recording API lives in `draw_snapshot.cpp` + `gles_perf.h` (not gfx.cpp/gfx_func.h); `BlitterParams` got `sprite_x`/`sprite_y`+`sprite_id`+`pal` (explicit coords — spec #7 upheld, per Q1.3/Q2.1); PaletteID-based remap resolution; base-driver hooks `OnGameLoopDone`/`SnapshotTick`; POI = stub; `WALLPAPER_BUILD` gates upstream code; `WALLPAPER_PERF`+`GLES_PERF_COUNT` gate perf collection; `nativeCycleZoom` removed from Java glue.
  - Append `## Stage 1 Result` to THIS plan file: device, pass/fail per step above, **which atlas-clear path shipped (delete+realloc vs reference in-place — Q1.7)**, known issues.

- [ ] **Step 6: Commit** (`Stage 1 done: GLES snapshot renderer on device` + docs in same commit).

---

## Out of scope (Stage 2 candidates)

Real POI scanner (`gles_poi.cpp` port), WallpaperService on-device lifecycle verification (preview↔live, rotation) + service-only JNI natives, **asset-packaging fix** (wire APK asset packaging to `dependsOn` the cmake `copy_assets` target so a single clean build is asset-complete — Q1.1), viewport/openttd `_gles_perf` instrumentation sites + `GLES_PERF_SCOPE` macros (route through `GLES_PERF_COUNT`/`#ifdef WALLPAPER_PERF` from the start), `StateGameLoop` timing, config-load revisit, atlas clear-strategy verification under memory pressure, perf tuning vs `tools/run_android.py perf` baselines.

## Self-Review (performed at write time)

- Spec coverage: rendering.md — shaders T3, atlas/decode T3, frame classes/batching T4, context recovery T4+T8, CPU-side guards T7, upload-after-swap T8 ✓; engine-changes.md — every listed interface change mapped (blitter T1/T5, gfx T1, video driver T6, spritecache T2, openttd/window/viewport/main_gui T7, sdl2_v T8); wallpaper-mode.md — loading T7, JNI table T8, POI deferred (stub, declared deviation 3) ✓
- Placeholders: none (stub POI file is a declared Stage-1 deliverable, not a TBD).
- Type consistency: `SnapshotTick`/`OnGameLoopDone`/`PaintFromSnapshot`/`RecoverContextIfLost` names consistent T6↔T8; `BlitToScreen()` zero-arg T4↔T8; `bp->sprite_x/sprite_y`+`bp->pal` T1↔T5 (Q1.3, pending Q2.1); `cmd.palette`→`ResolveRemap` T5↔T8; `GLES_PERF_COUNT` macro T1(def)↔T5(encode)↔T8(counters+log)↔T4(GPU queries), `WALLPAPER_PERF` cmake T7, `_gles_perf` global unconditional T1 ✓ (prewarm dropped — no PrewarmAfterClear cross-ref)

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely.

### Iteration 2 — 2026-07-07

#### Q2.1. Q1.3 chose "enforce explicit coords," reversing the plan's own deviation 1. Checking `GfxBlitter` (gfx.cpp:1070) confirms the difficulty the reference deliberately avoided: there is no ready absolute-screen-coord variable — `bp.left/top` are within-DPI unscaled offsets and `bp.dst = dpi->dst_ptr`, so the true screen position must be reconstructed from the DrawPixelInfo (and the zoomed-viewport DPI complicates it). How should recording coords actually be produced?
- [ ] Revert to the reference pointer-math coords (keep `SetRecordingBuffer` + recording buffer; `BlitterParams` gains only `sprite_id`+`pal`) — lowest risk, the battle-tested path the reference shipped; overview.md #7 stays an aspirational-but-deferred note  *(Recommended)*
- [ ] Explicit coords via `bp.sprite_x = dpi->left + bp.left; bp.sprite_y = dpi->top + bp.top;` in GfxBlitter; validate one known sprite position on-device in Task 9 before trusting the frame
- [ ] Explicit coords computed in the snapshot blitter as the `bp->dst - _screen.dst_ptr` pixel offset (no separate recording buffer, but still pointer-subtraction against the live screen base)
- [ ] Keep BOTH explicit coords and `SetRecordingBuffer` wired; compare on-device, delete the loser in the Stage 1 Result

#### Q2.2. The `GLES_PERF_COUNT(stmt)` macro (Q1.8 fold, defined Task 1) is a statement-wrapper — `GLES_PERF_COUNT(_gles_perf.snap_commands = n);` compiles to the statement when `WALLPAPER_PERF` is defined, else nothing — so one macro covers increments, assignments, `max`, and timer reads uniformly. Confirm this shape?
- [ ] Yes — one statement-wrapping `GLES_PERF_COUNT(stmt)` for every `_gles_perf.*` write; no separate counter/timer macros in Stage 1 (Stage 2's `GLES_PERF_SCOPE` chrono macro layers on top)  *(Recommended)*
- [ ] Prefer a value-style `GLES_PERF_COUNT(field, delta)` that only does `+=`; leave assignments/`max`/resets under a plain `#ifdef WALLPAPER_PERF` block
- [ ] Two macros — `GLES_PERF_COUNT(stmt)` for counters, `GLES_PERF_TIME(stmt)` for GPU-timer reads — for clearer call-site intent
- [ ] Drop the macro; wrap each perf region in raw `#ifdef WALLPAPER_PERF` instead

#### Q2.3. Q1.1 defers the asset-packaging fix to Stage 2, so a fresh Stage-1 build may still yield an assetless APK. How explicit should Task 9 Step 1 be about the workaround?
- [ ] Add a scripted precondition — after build, check the APK for baseset entries (`unzip -l ...apk | grep -c baseset`); if zero, rebuild+reinstall once automatically before launch  *(Recommended)*
- [ ] Keep the prose note only (rebuild+reinstall once if `Error extracting baseset assets` appears) — no scripted check
- [ ] Always build twice for the Stage 1 device gate, unconditionally; drop the conditional wording
- [ ] Move the assetless-APK handling out of Task 9 into a one-line Task 8 post-build note

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-07
- **Confidence:** 72% (cap from Unknowns/open-questions)
- **Resolved:** none (first pass)
- **Verified against current tree:** `Tick`/`GameThread`/`GameLoop`/`GetDrawInterval` hook sites exist as Task 6 assumes (video_driver.cpp:104/45/30, .hpp:331) → low hook risk; `GetActiveBlitter()` genuinely `private` (factory.hpp:42) → Task 1 move is real; `BlitterParams` has no member defaults (base.hpp:32) → appending `sprite_id;` matches convention; `OPENGLES_FOUND` TRUE at CMakeLists.txt:188 ✓.
- **Findings driving the gap:** (1) asset-packaging wart unaddressed though memory flags it as Stage 1 scope [Q1.1]; (2) big-bang device integration — 8 unverified tasks before first device run [Q1.2]; (3) four declared deviations are flagged "for review" = unratified decisions, one reversing locked spec #7 [Q1.3, Q1.4]; (4) `nativeCycleZoom` present in Java glue, absent from Task 8 export list [Q1.5]; (5) config-load-disable + literal `TODO`/rhetorical `?` tokens in body [Q1.6]; (6) atlas clear fallback noted but unplanned [Q1.7]. Readiness also inherently ≤80% (every task gates on APK build only — no failing-test-first; ratified Stage-0 precedent, not re-litigated here).
- **Still uncertain:** Unknowns (unratified deviations + deferral tokens) and Readiness (build-only gates, big-bang integration) drive the gap.
- **New questions:** Q1.1 … Q1.7

### Iteration 1 re-check — 2026-07-07
- **Trigger:** user hand-edited the plan (architecture-simplification pass), not a survey fold. Verified those edits for internal consistency.
- **Confidence:** 72% (unchanged — cap still the unanswered Q1.1–Q1.8; the edits reduced complexity but didn't touch the capping items).
- **Applied edits verified:** prewarm-drop propagated across Global Constraints + T3 (interface list, strip note, step renumber) + T9 expected-log + Out-of-scope; `WALLPAPER_PERF` gate added to Global Constraints + T4 (GPU queries) + T7 (cmake option, default ON) + T8 (counters+log) + Out-of-scope (`GLES_PERF_SCOPE` gated from start).
- **Fixed this pass:** stale `PrewarmAfterClear T3↔T3` cross-ref removed from Self-Review; T1 Step 3 now states `_gles_perf` global stays unconditional (link-safety) and points to Q1.8.
- **New questions:** Q1.8 (perf-gating scope — the one genuinely under-specified item the `WALLPAPER_PERF` edit introduced).
- **No downstream skill invoked (HARD-GATE): survey still unanswered.**

### Iteration 2 — 2026-07-07
- **Confidence:** 80% (cap from Readiness — the Q1.3 reversal reopened the core recording-coord mechanic; everything else folded clean).
- **Resolved (Q1.1–Q1.8 folded, dissolved from survey):**
  - Q1.1 → defer asset-packaging fix to Stage 2 → Out-of-scope + Task 9 Step 1 asset precondition note (refinement → Q2.3).
  - Q1.2 → keep single Task 9 device gate → no body change (plan already single-gate).
  - Q1.3 → **enforce explicit coords (reverses deviation 1)** → deviation 1 rewritten to "TAKEN"; `BlitterParams` += `sprite_x/sprite_y`; Task 1 Step 4/5, Task 5 interface + Step 3, Task 8 RecordSnapshot rewritten (no `SetRecordingBuffer`/recording buffer). Realization flagged open → Q2.1.
  - Q1.4 → ratify deviations 2–4 as written → no body change.
  - Q1.5 → remove `nativeCycleZoom` from Java glue (no callers) → Task 8 interfaces + new Step 2b + 1:1 export assertion.
  - Q1.6 → confirm config-load disabled → Task 7 Step 5 `TODO` reworded to decided-deferral marker.
  - Q1.7 → delete+realloc primary, in-place clear as planned follow-up → Task 3 strip note + Task 9 Step 5 records shipped path.
  - Q1.8 → full gating via `GLES_PERF_COUNT(stmt)` macro → defined in Task 1 (gles_perf.h); Global Constraints + Task 5 Encode + Task 8 counters/log routed through it (shape confirmation → Q2.2).
- **Honesty note on readiness cap:** every task gates on APK build + the Task 9 on-device gate; there is no unit-test-first path (no harness exists for GLES rendering). Per the ratified Stage-0 verification model, I do NOT treat the "failing-test-first" cap as the binding blocker — the binding open item is Q2.1.
- **Pushback recorded:** Q1.3's chosen option reverses a decision whose original rationale (avoid the `_screen.dst_ptr` race; absolute coords are not readily available in `GfxBlitter`) I independently assess as sound. Folded as chosen, but Q2.1 re-surfaces it with the concrete mechanics and recommends reverting to pointer-math — user's call.
- **Still uncertain:** Readiness (Q2.1 coord mechanic is the one load-bearing open decision; Q2.2/Q2.3 are low-stakes confirmations).
- **New questions:** Q2.1 … Q2.3
