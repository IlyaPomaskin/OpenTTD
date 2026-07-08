# Live Wallpaper Fork — Overview

Goal: OpenTTD as Android live wallpaper. Full simulation runs as spectator; rendering moved to GPU (GLES3); CPU blitting eliminated.

## Core idea: two-thread snapshot rendering

Instead of the CPU blitter writing pixels, the active blitter (`Blitter_Snapshot`) **records draw commands**. Game thread and GL thread never share GL state; they communicate via a lock-free triple buffer.

```
GAME THREAD (per tick)                     GL THREAD (main, per tick)
──────────────────────                     ──────────────────────────
::GameLoop()  (full sim)                   ProcessOverlayActions()   (JNI atomics: POI jump, scroll, map switch)
UpdateViewportPosition()                   RecoverContextIfLost()    (EGL surface/context checks)
RecordSnapshot():                          PaintFromSnapshot():
  redirect _screen to dummy buf              Acquire() from triple buffer
  (ref only: +192px margin — DROP)           replay DrawCommands -> GLESBackend queue
  RedrawScreenRect(full area)                  (atlas Lookup only; misses deferred)
    Blitter_Snapshot::Draw ->                PaintFBO()  (batched GL render to FBO)
      DrawCommand{sprite_id,x,y,clip,        BlitToScreen() + SwapWindow
      zoom,remap} into snapshot              upload missing sprites AFTER swap (idle time)
  Publish() -> triple buffer
```

- `DrawSnapshot` (src/video/draw_snapshot.h): vector of `DrawCommand` + palette + scroll state. Command = sprite id, screen xy, clip (skip/width/height), zoom, remap ptr.
- `SnapshotTripleBuffer`: 3 slots, one atomic `shared_idx`, publish/acquire = single `exchange`. Never blocks. GPU peeks frame_id, skips swap if stale.
- Sprite **pixels** flow separately: `Encode()` (game thread) enqueues CommonPixel data → GL thread uploads via PBO; plus GL thread can decode sprites itself from RAM-buffered GRFs (see rendering.md).
- Draw interval = game tick interval (30ms) in snapshot mode. Draw thread never takes `game_state_mutex` (only snapshot recording inside GameLoop does).

## Key files

| File | Role |
|---|---|
| `src/video/draw_snapshot.h` | DrawCommand, DrawSnapshot, SnapshotTripleBuffer, recording API |
| `src/blitter/snapshot.{cpp,hpp}` | Recording blitter (Draw→record, Encode→enqueue pixels) |
| `src/video/gles_backend.{cpp,h}` | GL state, batching, FBO, Paint/PaintFBO/BlitToScreen |
| `src/video/gles_sprite.{cpp,h}` | Atlas packer, PBO upload pipeline, GL-thread GRF decode |
| `src/table/gles_shader.h` | 7 GLSL ES 300 shaders |
| `src/video/sdl2_gles_v.{cpp,h}` | Video driver: EGL lifecycle, snapshot replay, JNI entry points |
| `src/video/video_driver.{cpp,hpp}` | GameLoop hook, RecordSnapshot, Tick snapshot path, pause CV |
| `src/wallpaper.{cpp,h}` | GM_WALLPAPER load, title map rotation |
| `src/video/gles_poi.{cpp,h}` | POI scanner + camera control |

Docs: [rendering.md](rendering.md), [engine-changes.md](engine-changes.md), [wallpaper-mode.md](wallpaper-mode.md), [android-app.md](android-app.md), [sdl-android-changes.md](sdl-android-changes.md), [build-system.md](build-system.md), [tools.md](tools.md).

## Reimpl decisions (2026-07-04)

- **Wallpaper-only, Android-only.** No playable game mode, no desktop/macOS support. Drops: menu-mode mouse hacks + LMB scroll + half-rate redraw in window.cpp, cocoa_wnd.mm / sdl2_v.cpp dev hotkeys, GM_MENU handling in wallpaper paths. GameActivity debug viewer optional (dev tool only). Legacy non-snapshot driver path can be deleted outright.
- **DROP smooth scrolling entirely.** Camera jumps instantly. Removes: GPU camera interpolation scaffolding (snapshot scroll state, `BlitToScreen(u,v)` UV offsets, driver prev/curr scrollpos), the 192px RecordSnapshot margin + viewport dimension mutation (existed only to cover edges during GPU-side scroll), forced smooth-scroll in viewport.cpp. Manual camera moves (JNI dpad) become instant jumps.
- **Emulator NOT a target.** Drop gfxstream workarounds: CPU staging buffer instead of glMapBufferRange, glGenTextures error-drain + fake-handle bail, two-pass PackRegion-before-PBO-bind ordering, in-place atlas clear (was gfxstream OOM; plain delete+recreate OK — verify on real device). Write straightforward GL code.
- **Single render path.** No `GLESBackend::Paint()` legacy path — only `PaintFBO()` + `BlitToScreen()`. (~450 duplicated lines gone.)
- **GL-thread-only sprite pipeline.** Drop game-thread pixel pipeline entirely: `Enqueue`/`upload_queue`+mutex, `known_sprites`+clear handshake, early-staging buffer, `GLESUploadRequest`, all PBO machinery. `LookupOrUpload` → `LoadSpriteOnGLThread` (decode from memory-backed GRF, per-frame budget) is the ONLY upload path. `Encode()` = metadata only. Keep priority-loading idea via post-clear pre-warm of ground/water sprites. Cross-thread surface = triple buffer + pause CV only. No priority pre-warm loop either (see rendering.md) — natural Z-order draw covers it, atlas clears are rare enough that pop-in order is imperceptible.
- **Slim `DrawSnapshot`.** Drop dead fields: `staged` map, `dirty_rects`, `full_redraw`, `DrawCommand.type`, `DrawCommand.palette`. Palette: single source — snapshot carries it (game thread fills, GL thread uploads); no parallel driver-side `local_palette` copy path. Replace raw `DrawCommand.remap` pointer with `PaletteID`, resolved to remap table on GL thread.
- **Explicit coords in `BlitterParams`.** Add screen x/y fields; `Blitter_Snapshot::Draw` reads them instead of reconstructing position from `dst − recording_buffer` pointer arithmetic. Recording dummy buffer shrinks to token allocation.
- **Atlas growth = clear + reload.** No read-FBO + `glCopyTexSubImage3D` layer-copy machinery; atlas full → clear, GL-thread decode repopulates on demand.
- **Instrumentation/debug KEPT, collection gated.** `_gles_perf`, PERF logging, GPU timer queries, debug overlays, [CTX]/[LOAD] logs, crash handler all stay (matched to tools/). DECISION (reimpl): counter *collection* (not just the log line) sits behind a compile-time flag (`WALLPAPER_PERF`) from the start — avoids always-on per-draw timing cost in shipping/battery builds.

## Status / caveats for reimplementation

- Camera interpolation is **scaffolded but off** in the reference: snapshot carries scroll state, `BlitToScreen(u_offset, v_offset)` supports UV shift, driver stores prev/curr scrollpos — always called with (0,0). Do not port.
- `Paint()` and `PaintFBO()` in gles_backend.cpp duplicate ~200 lines of batching code (noted TODO in source).
- `DrawCommand.remap` is a raw pointer into sprite cache memory — relies on recolour sprites being stable across threads.
- Config load (`LoadFromConfig`), sound, music are hard-disabled (commented out) — desktop build is intentionally crippled.
- Debug logging is verbose everywhere ([CTX], [LOAD], PERF); MouseLoop has leftover debug prints.
