# Core Engine / Interface Changes

Diff base: upstream/master (e24f92ce8). Android dir excluded.

> Reimpl decisions (see overview.md): wallpaper-only + Android-only + no smooth scroll. Items marked ~~DROP~~ are reference-only.

## Blitter interface

- `Blitter::BlitterParams.sprite_id` added — GPU atlas keying needs sprite identity at Draw time (`gfx.cpp GfxBlitter` fills it).
- DECISION (reimpl): also add explicit screen x/y to `BlitterParams`; `Blitter_Snapshot::Draw` uses them instead of `dst − recording_buffer` pointer arithmetic. Recording dummy buffer → token allocation.
- DECISION (reimpl): slim `DrawSnapshot` — drop `staged`, `dirty_rects`, `full_redraw`, `DrawCommand.type/.palette`; snapshot palette = single source (no driver-side `local_palette` parallel path); `DrawCommand.remap` raw ptr → `PaletteID` resolved on GL thread.
- `SpriteLoader/blitter Encode()`: new 4-arg non-virtual wrapper sets `protected encoding_sprite_id_` before calling virtual `Encode` — sprite identity during encode w/o changing all subclasses. `ReadSprite` (spritecache.cpp) calls the 4-arg version.
- `BlitterFactory::GetActiveBlitter()` private → public (driver needs `dynamic_cast<Blitter_Snapshot*>` to set recording buffer).
- New `snapshot` blitter: 32bpp, Draw→RecordCommand (screen pos recomputed from dst-pointer offset into recording buffer), Encode→atlas Enqueue + CacheMeta, all pixel ops no-op. `_blitter_autodetected=false` prevents SwitchNewGRFBlitter from replacing it.

## gfx.cpp / gfx_func.h

- Recording API: `StartRecording/StopRecording/IsRecording/RecordCommand/RecordDirtyRect` (global `_recording_snapshot` ptr, game thread only).
- Globals: `_gles_video_active`, `_gles_context_lost`, `GLESPerfCounters _gles_perf`.
- `RedrawScreenRect` calls `RecordDirtyRect`.

## VideoDriver (video_driver.{cpp,hpp})

- `GameLoop()`: after sim tick — `UpdateViewportPosition(main window)` + `RecordSnapshot()` (see overview.md) under game_state_mutex.
- `Tick()`: snapshot path replaces the whole lock-video/lock-game/InputLoop/Paint sequence with ProcessOverlayActions → RecoverContextIfLost → PaintFromSnapshot. Old interactive path unreachable (assert).
- New members: `snapshot_buffer` (unique_ptr, null = legacy mode), `game_thread_paused` atomic + condvar (`SetGameThreadPaused`), virtuals `PaintFromSnapshot()`, `RecoverContextIfLost()`.
- `GameThread()`: on pause runs 5 warm-up ticks (20ms apart, warms atlas for new camera pos) then blocks on CV.
- `GetDrawInterval()` = MILLISECONDS_PER_TICK in snapshot mode.
- Android: SIGSEGV/SIGABRT handler with `_Unwind_Backtrace` → logcat.

## Sprite cache / file IO (GL-thread decode support)

- `spritecache`: `BufferSpriteFilesToMemory()` (GRFs → RAM after LoadSpriteTables), `SaveSpriteFileBuffers()` (persists buffers across GfxLoadSprites by filename), `GetSpriteCacheInfo(id)` (thread-safe {file, file_pos, type, control_flags}), `GetRegisteredSpriteCount()`.
- `RandomAccessFile`: memory-backed constructor `(data, size, filename, base_offset)`; ReadByte/ReadBlock/SeekTo/SkipBytes/GetPos all have mem-mode branches. Concurrent reads safe on separate instances over same buffer.
- `SpriteFile`: memory-backed ctor (copies container metadata), `LoadIntoMemory()`, `GetMemoryData/Size`, `TakeMemoryBuffer/SetMemoryBuffer`, `GetContentBegin()`.
- `gfxinit.cpp GfxLoadSprites`: SaveSpriteFileBuffers → LoadSpriteTables → BufferSpriteFilesToMemory → BuildGLSpriteFiles.

## openttd.{cpp,h}

- `GameMode GM_WALLPAPER`, `SwitchMode SM_WALLPAPER`.
- Boot: `_game_mode/_switch_mode` start as WALLPAPER; `LoadWallpaperGame()` instead of `LoadIntroGame`; Android default videodriver `sdl-gles`.
- Sound/music: null drivers forced, InitializeSound/Music and MusicLoop removed, baseset scanning skipped.
- `LoadFromConfig` / highscores / hotkeys / window desc loading **commented out** (Android threading hack — revisit).
- `LoadIntroGame` uses `LoadNextTitleMap()` (rotation) + `PrepareBackground()`.
- `SafeLoad` failure in wallpaper → `LoadWallpaperGame()`.
- StateGameLoop instrumented (tileloop/vehicletick timing, vehicle counts).

## window.cpp

- `HandleMouseEvents()`: no-op in wallpaper (pure spectator).
- `DrawOverlappedWindowForAll`: wallpaper fast path (main window only).
- `UpdateWindows`: order changed (viewport update before DrawDirtyBlocks); chat/cursor skipped in wallpaper. ~~DROP: 1/2 redraw rate for non-wallpaper.~~
- ~~DROP (wallpaper-only): LMB viewport scroll in wallpaper, menu-mode click passthrough, `ScrollMainViewport` in menu.~~
- `RelocateAllWindows`: ResizeWindow called with **delta** (bugfix for resize).

## viewport.cpp

- Wallpaper: skip kdtree signs/text effects/string sprites; POI markers drawn (`DrawPOIMarkers`, debug). ~~DROP: forced smooth scroll.~~
- `SetViewportPosition` scroll-blit path disabled (commented) — snapshot records full frame.
- Heavy `_gles_perf` phase timing in ViewportDoDraw.

## main_gui.cpp

- Main window: title logo/version drawing removed; OnClick swallowed; viewport init zoom fixed `In4x`.
- `FixTitleGameZoom` works in GM_WALLPAPER.
- `GameSizeChanged`: in menu/wallpaper → FixTitleGameZoom + RecenterOnCurrentPOI; wallpaper dimension change → `SM_WALLPAPER` full reload (fixes orientation-change stale snapshots).

## Upstream mergeability (REQUIREMENT: periodic `git merge upstream/master` w/ minimal conflicts)

Most code lives in NEW files (zero conflict risk): gles_*, draw_snapshot.h, blitter/snapshot.*, wallpaper.*, gles_poi.*, android_main.cpp, android/, tools/. Conflicts only possible in touched upstream files. Design rules:

1. **New headers over insertions.** `GLESPerfCounters` + recording API go in own headers (`gles_perf.h`, `draw_snapshot.h`), NOT into `gfx_func.h` — upstream files get `#include` + a few calls only.
2. **Hooks over rewrites.** Don't rewrite `VideoDriver::Tick()` inline (reference does — worst conflict spot). Base class gets 2–3 tiny virtual hooks (`OnGameLoopDone()` for snapshot recording, pause-check in `GameThread`); all logic lives in `VideoDriver_SDL_GLES` / new files.
3. **One-hunk guards.** Wallpaper behavior in shared functions = early-return/`if (GM_WALLPAPER)` block at function TOP (window.cpp `HandleMouseEvents`, `DrawOverlappedWindowForAll`), or `#ifdef WALLPAPER_BUILD` — never interleaved edits mid-function.
4. **1-line instrumentation sites.** Timing in viewport.cpp/openttd.cpp via macros from `gles_perf.h` (`GLES_PERF_SCOPE(vp_land_us)`), not inline chrono blocks — reference has 10–20-line hunks there, reimpl should have 1-line hunks.
5. **Additive interface changes.** `BlitterParams` new fields (append), `Encode()` 4-arg wrapper (append), spritecache/RandomAccessFile/SpriteFile new methods (append) — additive edits merge clean.
6. **Don't delete upstream code** (title screen drawing, sound scanning, config load) — gate with `#ifdef WALLPAPER_BUILD` so upstream edits to those blocks merge.

Conflict-risk ranking of touched upstream files:
| Risk | Files | Why / mitigation |
|---|---|---|
| high | root CMakeLists.txt | `OPENTTD_SOURCE_DIR` refactor spans file; upstream edits often. Mechanical conflicts, easy resolve; candidate to upstream as PR |
| med | openttd.cpp, window.cpp, viewport.cpp, main_gui.cpp | high upstream churn; mitigate w/ rules 3–4, keep total hunks <5 per file |
| low | blitter/base.hpp, spriteloader.hpp, spritecache.*, random_access_file.*, sprite_file.*, gfxinit.cpp, fileio.cpp, debug.cpp, video_driver.hpp | additive-only changes |

## Misc

- `debug.cpp`: `__android_log_print` output, level→priority mapping. `os/unix/unix.cpp ShowOSErrorBox` also mirrors to logcat.
- `ini.cpp`: `__ANDROID__` added to unistd/fcntl include guard (ini fsync path).
- `framerate_type.h`: `LogPerformanceStats()` declared but never defined/called — dead leftover, drop on reimpl.
- `fileio.cpp`: `OPENTTD_DATA_PATH` env overrides all search paths (Android passes it from Java).
- `misc.cpp`: sound/music init removed.
- `landscape.cpp`, `saveload`: timing instrumentation only.
- cmake: `OPENTTD_SOURCE_DIR` instead of `CMAKE_SOURCE_DIR` (subproject builds), GLES sources, android_main.cpp.
- ~~DROP (Android-only): `cocoa_wnd.mm` / `sdl2_v.cpp` macOS dev hotkeys (Enter=next POI, [/]=POI, ;/'=map, +/-=zoom).~~
- `sdl2_v.cpp`: SDL_APP_DIDENTERBACKGROUND/WILLENTERFOREGROUND → pause/resume game thread; SDL_RENDER_DEVICE_RESET → `_gles_context_lost`; WINDOWEVENT_EXPOSED resets triple-buffer gpu_frame_id (forces re-blit).
