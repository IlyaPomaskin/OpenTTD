# GLES Rendering Pipeline & Optimizations

> Reimpl decisions (see overview.md): gfxstream/emulator workarounds, smooth-scroll/interpolation bits, and non-wallpaper CPU optimizations below are **reference-only — do not port**. Marked ~~DROP~~.

## Shaders (src/table/gles_shader.h)

One shared vertex shader (pixel coords → NDC). Fragment programs:

| Program | Purpose |
|---|---|
| normal | RGBA sprite from colour atlas |
| remap | M channel → remap table → palette; brightness from RGB channel (`adj_brightness`, matches OpenGL blitter) |
| palette | palette-only sprite: M → palette lookup, m==0 discard |
| transparent | outputs black*alpha, blend `(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA)` — darkening |
| resolve | fullscreen: idx attachment → new palette → colour (palette animation w/o re-render) |
| blit | FBO→screen, `u_brightness` multiplier (wallpaper dimming) |
| solid | debug rects |

## Sprite atlas (gles_sprite.cpp)

- Two `GL_TEXTURE_2D_ARRAY`: colour RGBA + remap R8 (M channel). Page = 2048–4096² (min of GL_MAX_TEXTURE_SIZE / GL_MAX_3D_TEXTURE_SIZE / 4096), max 4 layers each.
- Shelf packer, 1px padding, precomputed UVs.
- **GPU zoom scaling**: atlas stores ONE variant per sprite at `kGPUScaleBaseZoom = In4x`. Other zooms rendered by UV scaling. Sprite key = `(SpriteID<<4)|base_zoom` regardless of requested zoom. Saves atlas memory ~6x, zero re-upload on zoom change.
- ~~DROP — Growing: new array texture depth+1, old layers copied GPU-side via read-FBO + `glCopyTexSubImage3D`.~~ Reimpl: atlas full → clear + reload (GL-thread decode repopulates cheaply).
- Clear is **in-place** (reset cursors, keep textures — ~~DROP: reason was gfxstream OOM; plain delete+recreate fine on device~~), deferred to GL thread via atomic flag.
- 1x1 placeholder entry returned for missing sprites (semi-transparent black).

## Sprite upload paths

**DECISION: GL-thread on-demand decode is the ONLY path in reimpl.** Reference stages 1–2 below are ~~DROP~~ (game-thread pipeline + PBO); kept for understanding the reference.

1. ~~DROP — **Enqueue** (game thread): `Blitter_Snapshot::Encode` picks best zoom variant ≤ In4x, pushes CommonPixel copy into mutex-protected `upload_queue`. Dedupe via `known_sprites` set + atomic clear handshake. Early-staging static buffer before backend exists.~~ Reimpl: `Encode()` = metadata only.
2. ~~DROP — **PBO batch upload** (GL thread, each frame): 2×4MB PBOs, 15ms time budget, fences, two-pass pack-before-bind, CPU staging.~~ DECISION (reimpl): drop the priority pre-warm idea too — natural Z-order draw already visits ground/foundation sprites first; atlas clears are rare (map switch/overflow) and a few frames of incidental pop-in order aren't perceptible in spectator mode. No pre-warm loop, no priority ordering state.
3. **GL-thread on-demand decode** (the path): `LookupOrUpload` miss → decode sprite directly from **memory-backed GRF copy** (`SpriteLoaderGrf` over `SpriteFile(mem)`), upload synchronously via `glTexSubImage3D`. Budget 500 loads/frame; placeholder for 1 frame on miss. GL thread fully self-sufficient — after atlas clear it reloads everything without game thread.
   - Enabled by: `BufferSpriteFilesToMemory()` (whole GRFs into RAM at GfxLoadSprites, cached across reloads via `SaveSpriteFileBuffers`), `BuildGLSpriteFiles()` (GL-thread-owned SpriteFile instances over same buffers), `GetSpriteCacheInfo()` (thread-safe id → file/pos/type).
   - `meta_cache` (SpriteID → root w/h/offs) never cleared, used for fast-path dimension queries.

## Frame rendering (gles_backend.cpp Paint/PaintFBO)

**DECISION: single render path — `PaintFBO()` + `BlitToScreen()` only; legacy `Paint()` (~450 duplicated lines) not ported.**

- **Persistent MRT FBO**: attachment0 = RGBA colour, attachment1 = R8 palette index. Sprites accumulate; only dirty rects scissor-cleared before draw.
- **Batching**: all queued commands → one vertex array in original Z-order (no sorting). 6 verts/sprite: pos + colour UV + remap UV + per-vertex atlas page indices (cpage/rpage) → **batch breaks only on shader change or remap-table change**, never on atlas page change. Single `glBufferSubData`; VBO orphaned each frame (`glBufferData(nullptr)`) to avoid GPU sync stalls. Attribute locations forced identical across programs (`glBindAttribLocation`) → vertex attribs configured once per frame.
- UV computation: full-sprite draws use exact 0..1 (avoids seams); clipped draws scale skip/width by zoom ratio, clamped to [0,1]. Atlas entry dims used as truth (not cmd dims — integer UnScaleByZoom rounding differs).
- Remap tables: 256×1 R8 texture, double-buffered (swap on table change to avoid ghosting), pointer-cached to skip redundant uploads.
- Palette: 256×1 RGBA, index0 alpha=0, rest forced 255.
- **Three frame classes** (biggest win):
  1. draw queue non-empty → full render into FBO;
  2. queue empty + palette dirty + FBO valid → **resolve pass only** (fullscreen quad: idx→palette; detach idx attachment to avoid feedback loop) — palette animation (water) costs one fullscreen pass, zero sprite work;
  3. nothing changed → **skip everything incl. swap** (return false).
- Phase 2: blit FBO→default framebuffer with brightness; ~~DROP: UV offset params for camera interpolation~~.
- GPU timing: `GL_EXT_disjoint_timer_query`, double-buffered queries, non-blocking readback → `_gles_perf.gpu_time_us`.
- Missing-sprite uploads deferred to after `SDL_GL_SwapWindow` (frame jitter moved into idle time; 1-frame latency for new sprites).

## CPU-side render optimizations (engine)

- ~~DROP (non-wallpaper): `UpdateWindows()` every-2nd-frame redraw throttle.~~
- GM_WALLPAPER in `ViewportDoDraw`: skip `ViewportAddKdtreeSigns` + `DrawTextEffects` + string sprites.
- `DrawOverlappedWindowForAll`: wallpaper fast path — only WC_MAIN_WINDOW painted, no overlap clipping logic.
- ~~DROP (no smooth scroll): `RecordSnapshot` 192px margin + viewport dimension mutation.~~ Record at real screen size; normal sprite clipping covers edges.
- Viewport scroll blit (`DoSetViewportPosition`) disabled — snapshot re-records whole frame anyway.
- Perf counters: `GLESPerfCounters _gles_perf` (gfx_func.h) — huge struct covering viewport phases, blitter, PBO, snapshot stages, jank/percentiles; logged every 500ms at debug level 3 from driver `Paint()`. DECISION (reimpl): gate *collection*, not just logging, behind a compile-time flag (`WALLPAPER_PERF`) — per-draw chrono/atomic bookkeeping is wasted battery in shipping builds; lives in its own header (`gles_perf.h`) so the gate costs zero merge risk.

## Context loss / surface recovery (Android essential)

- Detection: `SDL_RENDER_DEVICE_RESET`, EGL error after swap, ctx==EGL_NO_CONTEXT, ctx pointer changed between ticks, JNI `nativeSurfaceChanged` flag.
- Surface-only change: manually `eglCreateWindowSurface` on new ANativeWindow (SDL doesn't rebind wallpaper surfaces), destroy old, make current, Resize FBO. Fallback: recreate SDL GL context.
- Full loss: `RecoverGPUState()` — zero all GL handles WITHOUT glDelete (dead context), atlas `AbandonGLObjects()`, re-run InitGLObjects+Resize, `_switch_mode = SM_WALLPAPER` → map reload repopulates sprites.
