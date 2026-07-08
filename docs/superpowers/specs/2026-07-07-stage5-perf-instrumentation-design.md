# Stage 5 — Perf Instrumentation, Tooling & Hardening (Design Spec)

**STATUS: DONE (2026-07-08, `lwp2` tip `c7d6a86f69`).** Implemented in two waves + device-verified + a bounded §4 tuning pass. **Shipped:** §1 `GLES_PERF_SCOPE` RAII macro (`gles_perf.h`); §2 timing sites wired in `viewport.cpp`/`openttd.cpp`/`landscape.cpp` (per-call scope, documented delta vs `gles` overlapping spans); §3 perf tools ported verbatim from `gles` (`perf_monitor.py`/`perf_stats.py`/`perf_collect.sh`); Q3.1 `gameloop_ticks` single producer moved to `StateGameLoop` (driver dup deleted); Q3.2 desktop (non-`WALLPAPER_BUILD`) build restored (`wallpaper.cpp` + `openttd.cpp` gates + unconditional `gles_perf.h` include); §5 hardening (empty-mem-file crash fix + sprite-file-cache size-in-key). **Verified:** Android `assembleDebug` + macOS cmake both green (desktop build was broken since Stage 1); on-device (Pixel 10 Pro) all §2 fields log non-zero, ported parsers consume real output, ~37 fps sustained, jank=0. **§4 tuning pass done:** app healthy/jank-free, no localized regression → no code change warranted (per bounded-pass discipline). **Follow-ups (out of Stage 5, filed as observations):** on-demand sprite decode+upload rides inline in GL-thread paint (prefetch/warm-atlas at map-load); `ResizeSprites` zoom-ladder cost at `SWITCH_MAP`; `TICK`-line draw-thread phase timers still unwired; screen-off idle CPU check not yet run. Config-load left defaults-only (Stage 4 persists via Android prefs, not `openttd.cfg`). Commits: `3b5ea87309`, `0f831f4055`, `33b78dfef5`, `8b4c188414` (Wave A), `9ba1a0b2cb`, `c7d6a86f69` (Wave B).

---

**Plan-confidence status (historical):** Ready for implementation plan (iteration 4, confidence 92%). **Stage 1 is now COMPLETE and device-verified** (Pixel 10 Pro, `lwp2` tip `634dd21703`). `src/video/sdl2_gles_v.cpp` ships with the 500 ms PERF-log block (`Debug(driver,3)`, cadence at L867, six lines L873/885/893/931/938/944), the whole block wrapped in `GLES_PERF_COUNT({…})` (L841–955, incl. the `p = {}` per-period reset at L951). The `GameMode::Wallpaper` boot + the guarded `viewport.cpp`/`openttd.cpp`/`landscape.cpp` forms exist — so the §2 sites are now **pinned to real file:line** (below) instead of reference templates. The entry-readiness cap that held this spec at 80% is **LIFTED**. All six residual survey questions were **human-ratified interactively at iteration 4** (Q2.1/Q2.3/Q2.4/Q2.5/Q3.1/Q3.2 — see Reconciliation Log); every §2 site is pinned to verified file:line and every Open decision is DECIDED. The only remaining softness is inherent to the work, not the doc: §4 perf-tuning findings are empirical (device-run-dependent), and config-load's final field wiring is finalized once Stage 3/4's settings-storage lands (the decision — minimal set behind `#ifndef WALLPAPER_BUILD` — is made). Ready to become an implementation plan.

**Goal:** Close the instrumentation gap Stage 1 deferred and make the wallpaper shipping-quality. Add the `GLES_PERF_SCOPE(field)` RAII chrono macro; wire the viewport/gameloop timing **sites** through it and `GLES_PERF_COUNT` as 1-line hunks (mergeability rule 4); port the offline/live perf parsers from `gles` so the C++ PERF log lines are consumable; run a bounded perf-tuning pass against `tools/run_android.py perf/fps` + simpleperf baselines; and land the hardening items Stage 1 punted (atlas-clear verification, config-load decision, battery/idle behaviour).

**Nature:** A mixed stage. Two parts are owned/low-risk (the macro in `src/video/gles_perf.h`; four python tools under `tools/`). One part is the highest-merge-risk work in the fork — timing edits inside shared upstream files (`viewport.cpp`, `openttd.cpp`, `landscape.cpp`) — which is precisely why it is gated to **1-line-per-site** hunks. The tuning and hardening parts are measurement + decision passes, not large code.

## Context / dependencies

- **Real dependency (reassessed — was "hard dep on Stages 1–4").** Stage 5 *functionally* needs only **Stage 1**: the driver's 500 ms PERF-log block (`src/video/sdl2_gles_v.cpp:867`/`873-944`) and the `GameMode::Wallpaper` boot + guards. That is now **MET** (device-verified). The macro (§1), the §2 wiring, the tools port (§3), and the hardening items (§5) can all proceed now. Stages 2–4 (real POI scanner, WallpaperService lifecycle, settings surface) are **benefits, not gates**: they make the §4 tuning pass exercise a realistic workload, and Stage 3/4's settings-storage is the deciding input for config-load (b) — but nothing in the Stage-1-complete tree blocks starting Stage 5. This stage adds no new rendering behaviour.
- **Reference (read-only):** `gles:src/viewport.cpp` (ViewportDoDraw chrono block, lines 1846-1889), `gles:src/openttd.cpp` (StateGameLoop, lines 1242-1324), `gles:src/landscape.cpp:844`, `gles:src/video/sdl2_gles_v.cpp` (PERF log block + jank ring buffer, ~670-772), `gles:tools/{perf_monitor.py,perf_stats.py,perf_collect.sh,extract_vehicles.py}`. Extract with `git show gles:<path>`; NEVER modify `gles`.
- **Spec authority:** `docs/wallpaper/rendering.md` ("Frame rendering" three frame classes, "CPU-side render optimizations", "Perf counters"), `docs/wallpaper/engine-changes.md` (mergeability rule 4; "Misc" instrumentation sites), `docs/wallpaper/tools.md` (the matched C++/python pair), `docs/wallpaper/overview.md` (instrumentation-gated decision).
- **Current-tree state (verified on `lwp2` tip `634dd21703`, 2026-07-08):**
  - **Stage 1 driver present:** `src/video/sdl2_gles_v.cpp` exists (37 KB). PERF-log block: cadence gate `if (fps_now - fps_last >= 500ms)` at L867; six `Debug(driver,3)` lines — `PERF` L873, `  VP` L885, `  TICK` L893, `  CPU` L931, `  EXTRA` L938, `  GPU_SNAP` L944 — all inside one `GLES_PERF_COUNT({…})` opened at L841 and closed at L955, which also does the per-period `p = {}` reset (L951). So an OFF build drops the entire block (Q1.8 invariant **confirmed**, not merely assumed).
  - `src/video/gles_perf.h` ships `GLESPerfCounters`, `extern _gles_perf`, and `GLES_PERF_COUNT(stmt)` (L173-177, no-op unless `WALLPAPER_PERF`). **`GLES_PERF_SCOPE` still does not exist**, and the header has **no `#include`** at all — so §1 adding `<chrono>` + the guard struct under the ON branch is confirmed-new surface.
  - `src/viewport.cpp`, `src/openttd.cpp`, `src/landscape.cpp` still contain **zero** `_gles_perf`/`GLES_PERF` writes (`git grep` empty). Consequently `vp_land_us`, `vp_vehicles_us`, `vp_sort_us`, `vp_draw_us`, `vp_tiles_iterated`, `vp_parent_sprites`, `vp_child_sprites`, `vp_calls`, `vp_area_w/h`, `tileloop_us`, `tileloop_count`, `vehicletick_us`, `gameloop_us`, `vehicle_trains/road/ships/aircraft` are populated by nothing — the driver PERF block prints them as **0**. Wiring these is the core of this stage (the Stage-1 DEFERRAL "instrumentation sites mostly UNWIRED — PERF logs read ~0"; `.superpowers/sdd/progress.md`).
  - **Exception — `gameloop_ticks` is already non-zero:** the driver's `RecordSnapshot()` (once per recorded game tick) does `GLES_PERF_COUNT(_gles_perf.gameloop_ticks++)` at **sdl2_gles_v.cpp:450**. In the `gles` reference this increment lived in `StateGameLoop` (`gles:src/openttd.cpp:1307`) and the gles driver only *read* it (`gles:sdl2_gles_v.cpp:753/759`, `gt = max(1, gameloop_ticks)`). So `gameloop_ticks` has moved producers between branches — the §2 openttd.cpp site must NOT re-add it or it double-counts. See §2 note + Q3.1.
  - `GLES_PERF_COUNT` is already used in `sdl2_gles_v.cpp`, `gles_backend.cpp`, `gles_sprite.cpp` (GL-thread render/atlas/snapshot counters, done in Stage 1). No bare `_gles_perf.` writes exist outside the macro anywhere in `src/`.
  - `tools/run_android.py` and `tools/test_wallpaper.sh` are present and **byte-identical** to `gles` (same blob hashes). `tools/{perf_monitor.py,perf_stats.py,perf_collect.sh,extract_vehicles.py}` exist **only in `gles`**.
- **Binding principles (overview.md + engine-changes.md):** no dead scaffolding; instrumentation KEPT but collection gated behind compile-time `WALLPAPER_PERF`; **mergeability rule 4 is central** — timing in shared upstream files goes through header macros, never inline chrono blocks; the `_gles_perf` global links unconditionally.

## Scope

### 1. `GLES_PERF_SCOPE(field)` — RAII chrono macro (in `src/video/gles_perf.h`)

The only new C++ symbol this stage adds. It is the timing counterpart to the existing statement-wrapper `GLES_PERF_COUNT(stmt)`:

- **`WALLPAPER_PERF` ON:** `GLES_PERF_SCOPE(field)` constructs a stack RAII timer that samples `std::chrono::steady_clock` at construction and, at scope exit, adds the elapsed microseconds to `_gles_perf.<field>` (`+=`, matching the reference's accumulate-per-period semantics; the driver zeroes the struct each 500 ms window). `field` must name an `int64_t *_us` member. A small guard `struct` (e.g. `GLESPerfScopeTimer` holding `int64_t &acc` + a start time) lives in the header under the ON branch, which also pulls in `<chrono>`. Instances get unique names via `__LINE__` token-paste so two scopes can coexist in one block.
- **`WALLPAPER_PERF` OFF:** expands to `do {} while (0)` — no timer object, no `<chrono>` dependency, no reference to `_gles_perf`. **Zero cost, zero code, zero merge surface** in a battery build.

Layering: `GLES_PERF_SCOPE` handles the `*_us` timers only; `GLES_PERF_COUNT(stmt)` continues to carry increments, assignments, `max`, and the whole vehicle-count block. Keeping the two macros distinct (rather than one overloaded macro) is the recommendation — see Open decision (e).

Placement rationale (rule 1 + 4): the macro lives in the fork-owned `gles_perf.h`, so the upstream files that use it gain only `#include "video/gles_perf.h"` + one-token call sites.

### 2. Instrumentation sites (the deferred Stage-1 work — all 1-line hunks)

Every site below replaces a reference **inline chrono block** (10–20 lines of `_t0.._t5` timestamps + a batch-accumulate `{}`) with per-phase macro calls, per mergeability rule 4. Total added hunks kept **< 5 per file**.

**`src/viewport.cpp` — `ViewportDoDraw`** (current tree: function opens at **L1820**; reference block `gles:src/viewport.cpp:1846-1889`). Each phase call gets wrapped in a brace-scoped `GLES_PERF_SCOPE`. Pinned sites (verified 2026-07-08):

| Field (`_gles_perf.`) | Site (current `lwp2` file:line) |
|---|---|
| `vp_land_us` | scope around `ViewportAddLandscape()` — **L1840** |
| `vp_vehicles_us` | scope around `ViewportAddVehicles(&_vd.dpi)` — **L1841** |
| `vp_tilesprites_us` | scope around `if (!…empty()) ViewportDrawTileSprites(…)` — **L1854** |
| `vp_sort_us` | scope around `_vp_sprite_sorter(&_vd.parent_sprites_to_sort)` — **L1860** |
| `vp_draw_us` | scope around `ViewportDrawParentSprites(…)` — **L1861** |

Count fields via `GLES_PERF_COUNT` (one call each, near the end of `ViewportDoDraw` ~L1899-1903 before the `.clear()` calls): `vp_calls++`, `vp_tile_sprites`, `vp_sprites_generated`, `vp_parent_sprites`, `vp_child_sprites`, `vp_area_w`/`vp_area_h` (`= std::max(...)`). The `vp_tiles_iterated++` site is a single `GLES_PERF_COUNT` inside `ViewportAddLandscape` per-tile inner loop (**~L1264**, the `IsInsideBS(...)` per-tile body; def starts L1216).

**Stage-1 guard wrinkle (new — verify before impl).** Stage 1 wrapped the signs/text-effects calls in `#ifdef WALLPAPER_BUILD ... #else ... #endif` (L1843-1852): the `WALLPAPER_BUILD` copy is `if (_game_mode != GameMode::Wallpaper) { ViewportAddKdtreeSigns(...); DrawTextEffects(...); }` (L1844-1847); the `#else` copy is unconditional (L1849-1851). So `vp_kdtree_us`/`vp_texteff_us`/`vp_signs_tiles_us` (and `vp_kdtree_found`, `vp_strings_queued`) now sit in a **textually duplicated** region — a per-call scope there costs **2 hunks each** (one per `#ifdef` arm) and reads ≈0 in wallpaper anyway. **Recommend: skip these three `*_us` sub-timers** (leave them 0; they measure the non-wallpaper path only) rather than double the viewport hunk count; this also keeps the file ≤5 hunks. If a non-wallpaper/upstream build ever wants them, add the scope only to the `#else` arm. Folds into the fidelity delta (Q2.3).

**`src/openttd.cpp` — `StateGameLoop`** (current tree: function opens at **L1268**; reference `gles:src/openttd.cpp:1242-1324`). The pause/modal guard returns at L1275-1289; the `if (_game_mode == GameMode::Editor)` branch is L1294-1303; the **normal branch** (wallpaper path) is the `else` at L1304+, with `RunTileLoop()` L1324, `CallVehicleTicks()` L1325, `CallLandscapeTick()` L1326. Pinned sites:

| Field | Site (current `lwp2` file:line) |
|---|---|
| `gameloop_us` | `GLES_PERF_SCOPE(gameloop_us)` spanning the whole tick body — place after the pause-guard `return` and before the `if (_game_mode == GameMode::Editor)` (**~L1290-1291**) so it covers both branches (matches the reference `gl_t0`-before-branch extent; Q2.4) |
| `tileloop_us` | scope around `RunTileLoop()` — **L1324** |
| `vehicletick_us` | scope around `CallVehicleTicks()` — **L1325** |
| `vehicle_trains/road/ships/aircraft` | the `Vehicle::Iterate()` counting loop wrapped as **one** `GLES_PERF_COUNT({ …block… })` hunk (the macro's `do { stmt; } while(0)` form accepts a braced block with no top-level commas); this loop is **new fork code**, not present in `lwp2` yet |

**`gameloop_ticks` — DO NOT add here (correction).** The driver already produces it: `RecordSnapshot()` does `GLES_PERF_COUNT(_gles_perf.gameloop_ticks++)` at `sdl2_gles_v.cpp:450`. Re-adding it in `StateGameLoop` would double-count. Two coherent shapes (Q3.1): **(A)** keep the driver's L450 increment, drop it from §2 openttd.cpp — fewest edits, but `gameloop_ticks` then counts *recorded snapshots*, which can differ from *executed ticks* if the game thread skips a snapshot when the draw thread is behind, making the per-tick divisors (`tileloop_us/gt`, `vehicletick_us/gt`) slightly off; **(B)** move the single producer into `StateGameLoop` next to the `tileloop_us`/`vehicletick_us` timers (so numerator and divisor are written in the same scope, matching the `gles` reference at `gles:openttd.cpp:1307`) and delete the driver's L450 increment (driver is a fork-owned file — free to edit). **Recommend (B)** for divisor coherence; flagged Q3.1.

The scoped enum is `GameMode::Editor` (not `GM_EDITOR`; Stage 1 migrated all `GM_*` → `GameMode::*`). The reference instruments both the editor and normal branches; the wallpaper only ever runs the normal branch, so instrument that one (add the editor branch only if trivially symmetric — otherwise skip, no wallpaper impact). With `gameloop_ticks` folded to a single site, this keeps `openttd.cpp` to ~4 hunks.

**`src/landscape.cpp` — `RunTileLoop`** (current tree: function opens at **L804**). The tile count is already computed into a local `uint count` at **L822** (`1 << (Map::LogX() + Map::LogY() - TILE_UPDATE_FREQUENCY_LOG)`); it is then decremented at L831/L834. So the single hunk is `GLES_PERF_COUNT(_gles_perf.tileloop_count += count)` placed **immediately after L822** (before the `count--` / `while (count--)` mutate it) — reuse the local rather than recomputing the shift.

**Map-load / saveload timing (optional, secondary):** engine-changes.md "Misc" lists saveload timing, but `gles` has **no `_gles_perf` field** for it — the reference logs map-load duration as a plain `Debug(...)` line in `LoadIntroGame` (gles openttd.cpp ~333-362). Recommendation: port that as a single `Debug(driver, 1, ...ms)` line (not a counter) if map-switch latency needs a number; otherwise drop it. Flagged in Open decision (a) as tuning-scope.

**Counter-semantics caveat (verified against `gles`, still-open Q2.3/Q2.4).** The reference does NOT time these phases with tidy per-call scopes; it uses overlapping `_t*`/`gl_t*` spans: `tileloop_us` = `gl_t0..gl_t1` (spans `AnimateAnimatedTiles` [current L1318] + the `TimerManager<…>::Elapsed` calls [L1319-1323] + `RunTileLoop` [L1324], **not** `RunTileLoop` alone); `gameloop_us` `gl_t0` is set *before* the `GameMode::Editor`/normal branch (spans the whole tick body — §2 now matches this, placing the scope at ~L1290); `vp_signs_tiles_us` = `_t2.._t3` (overlaps `vp_tilesprites_us` = `_t2b.._t3`, and per the §2 guard-wrinkle these signs/text sub-timers are recommended dropped in wallpaper). A literal "scope around `RunTileLoop()`" per §2 therefore yields a **narrower** `tileloop_us` than the reference. **DECIDED (plan-confidence iter 4, human-ratified Q2.3):** accept the tidier per-call semantics and **document the delta vs `gles`** — exact parity is not needed for a bounded tuning pass (do NOT wrap the broader `L1318..L1324` region). **DECIDED (iter 4, human-ratified Q2.4):** `gameloop_us` spans the **whole tick body** (scope at ~L1290, matching the reference `gl_t0`-before-branch extent), instrumenting the **normal branch only** — skip `GameMode::Editor` (never runs in wallpaper).

**Invariant to preserve (now confirmed in-tree):** the driver's PERF-log block reads all of the above fields, and the whole block (jank ring buffer + all six `Debug` lines + the `p = {}` reset) is already wrapped in one `GLES_PERF_COUNT({…})` at `sdl2_gles_v.cpp:841-955` — so the Q1.8 "OFF build drops it entirely" invariant is verified, not assumed. This stage only makes the §2 fields non-zero; no change to the log block itself, only confirming every field it prints now has a producer.

### 3. Perf tooling port (`tools/`)

The C++ PERF log format and the python parsers are a matched pair (tools.md). The log lines (reference `gles:src/video/sdl2_gles_v.cpp`) are: `PERF fps=…`, `  VP landscape=…`, `  TICK total=…`, `  CPU gameloop=…`, `  EXTRA gpu_actual=…`, `  GPU_SNAP clear=…`, all at `Debug(driver, 3, …)`, tag `OpenTTD`. Port these tools verbatim from `gles`, then verify they parse the current build's real output (drift check):

| Tool | Source | Role | Port note |
|---|---|---|---|
| `tools/perf_monitor.py` | `gles` (546 L) | live TUI: adb/file/pipe → braille graphs; 9 screens (0 = adb-broadcast remote, 1–8 metric groups) | port verbatim; verify regexes vs current PERF lines |
| `tools/perf_stats.py` | `gles` (238 L) | offline min/avg/max/p95 for PERF/VP/TICK sections | port verbatim |
| `tools/perf_collect.sh` | `gles` | wrapper: collect N s (default 20) from device → TICK/VP/EXTRA/PERF `sed`+`awk` breakdown | port verbatim |
| `tools/extract_vehicles.py` | `gles` | GRF sprite dump (atlas/decode debugging) | port only if kept — Open decision (c) |
| `tools/run_android.py` | already in tree | main automation (build/deploy/perf/fps/jump/switch/logs) | **no change** (byte-identical to gles) |
| `tools/test_wallpaper.sh` | already in tree | E2E smoke → screenshot | **no change** |

**Matched-pair rule:** any change to a `_gles_perf` field name or to a PERF-log key must be mirrored in `perf_stats.py`'s regex (`key=NUMBERus` / `key=NUMBER`) and `perf_monitor.py`/`perf_collect.sh`'s `sed` capture groups. Since the fields wired in §2 already exist in the reference log format, the parsers should consume them unchanged — the drift check is the acceptance test, not a rewrite. All python runs use `/usr/bin/python3`.

### 4. Perf tuning pass (bounded)

Instrumentation exists to be used once, here, against the three frame classes (rendering.md): full render / resolve-only / idle-blit. Method:

1. Baseline capture on the Pixel 10 Pro: `python3 tools/run_android.py perf 30` + `tools/perf_stats.py 30` over a steady spectator run and across repeated `SWITCH_MAP`; a simpleperf profile via the `simpleperf` skill / `run_android.py all`.
2. Read the breakdown: TICK total split (game_mutex wait, paint, gpu_render, swap), VP split (landscape/vehicles/sort/draw), CPU split (gameloop/tileloop/vehtick), EXTRA (jank, p95/p99, overdraw, batch_eff, cache_hit), GPU_SNAP replay stages.
3. Confirm the frame-class distribution matches design intent: steady water/sim → mostly resolve-only + idle-blit; full renders only on camera move / atlas repopulation. Any unexpected full-render churn (e.g. every-frame full render while idle) is the highest-value finding.
4. Apply only findings that are localized and clearly attributable (e.g. a stray full-frame dirty rect, a missing idle-skip). **Open-ended optimization is out of scope** — this is a bounded pass with a stop condition (Open decision (a)).

Recommended target metrics (surface as the stop condition, not a hard SLA): sustained ≥ the game tick rate on the device with headroom, idle-frame CPU ≈ 0 (screen-off), and no jank spikes (`jank_count`≈0, p99 within ~2× p50) over a multi-minute run.

### 5. Hardening (deferred from Stage 1)

**Atlas-clear strategy verification (Stage 1 Q1.7).** Stage 1 shipped **delete+realloc** as the primary clear path (`ClearSprites` → `glDeleteTextures` + `Init`-style realloc); the reference's in-place clear (reset cursors, keep textures) is the documented fallback if map-switch shows artifacts. This stage confirms the shipped path under sustained use: extended run with repeated `SWITCH_MAP` (each triggers atlas clear + GL-thread reload), watching for flicker/pop-in artifacts, leaked GL memory, or growth in `gpu_sprites_repacked`/occupancy that never settles. If artifacts appear → land the in-place-clear fallback commit and record the swap. Scope of a *dedicated* memory-pressure stress test vs observation-during-normal-runs is Open decision (d).

**Config-load revisit.** Stage 1 gated `LoadFromConfig` / `LoadFromHighScore` / `LoadHotkeysFromConfig` / `WindowDesc::LoadFromConfig` behind `#ifndef WALLPAPER_BUILD` (defaults-only). Decide what, if anything, to re-enable for shipping — the wallpaper has real user settings (brightness, POI cadence, map rotation) coming from Stage 3/4 that may want persistence. Recommendation and tradeoffs in Open decision (b); whichever path, it stays a one-hunk `#ifdef` gate per rule 6 (gate, don't delete upstream code).

**Battery / idle behaviour.** Verify the two idle paths already designed:
- Game-thread pause → ≈0 CPU: on screen-off / background, `SDL_APP_DIDENTERBACKGROUND` → `SetGameThreadPaused(true)` blocks the game thread on its CV after the 5 warm-up ticks (engine-changes.md video_driver). Confirm no wakeups, ≈0% CPU with `adb shell top`/simpleperf while screen-off.
- GL-thread idle-blit skip → no swaps: frame class 3 (nothing changed) returns without `SwapWindow` (rendering.md). Confirm `idle_blits` rises and `swap_us`/`full_renders` fall when the scene is static.

**Atlas VRAM pre-alloc (Stage-1 DEFERRAL — memory/perf).** Stage 1 (Task 3) changed the atlas to pre-allocate **all `MAX_ATLAS_LAYERS` (4) layers up front** via `GLESSpriteAtlas::AllocLayers()` (`src/video/gles_sprite.cpp:24`, called from the ctor L66; two `glTexImage3D` array textures RGBA + R8 at `atlas_size²×4`), to avoid `glCopyTexSubImage3D` growth cost. That is **~80 MB VRAM @2048² / ~320 MB @4096²** reserved regardless of occupancy. It **ran fine on the Pixel 10 Pro** (device-verified boot: `atlas(2048x2048x4)`), so this is a watch-item, not a blocker. Stage-5 action: with the `atlas_occ` counters (`EXTRA` line) confirm real occupancy under a full spectator run; **decide whether to lazy-alloc layers on demand** (grow to N layers only when packing needs them) to cut idle VRAM — but only if occupancy shows the upper layers are usually empty AND a lower-memory device is a target. Recommend: measure, keep eager-alloc unless a memory-constrained device forces it (eager-alloc avoids mid-run growth stalls, which matters for a smooth wallpaper).

**Robustness / sprite-cache hardening (Stage-1 DEFERRAL — tracked at `progress.md` S1 Task 2).** Two faithful-port latent bugs, non-triggering in Stage 1's fixed-GRF Android path but real:
- `RandomAccessFile` mem-mode **nullptr-sentinel crash on a legitimately empty file**: when `mem_data == nullptr` the code falls to the file path and dereferences an unset `file_handle` (`src/random_access_file.cpp`). Fix = distinguish "no mem buffer" from "empty file".
- `_sprite_file_cache` is keyed by **`GetSimplifiedFilename()` (basename only), with no size/hash check** (`src/spritecache.cpp:97`/`121`) → a same-named GRF swap serves a stale buffer. Fix = include size (or content hash) in the key. (Also minor: dup filename-parse in two ctors; possibly-unused `gfx_func.h` include.)
- Stage-5 action: land both as small localized fixes (they touch fork-owned/additive code, low merge risk). In scope for Stage 5 hardening.

**Desktop / non-`WALLPAPER_BUILD` build breaks (Stage-1 DEFERRAL — tracked at `progress.md` S1 Task 7; mergeability rule 4/6).** Stage 1 left the non-Android build broken in two places (out of Stage 1's Android-only scope, unverifiable there):
- `src/openttd.cpp` — **ungated `LoadWallpaperGame()` case bodies**: `SafeLoad`'s `switch (ogm)` at **L1040** (`case GameMode::Wallpaper: LoadWallpaperGame();`) and `SwitchToMode`'s `switch` at **L1213-1215** (`case SwitchMode::Wallpaper:`). Both switches have a `default`, so a 2×`#ifdef WALLPAPER_BUILD` gate around just those case bodies is `-Wswitch`-safe.
- `src/wallpaper.cpp` — **compiled unconditionally but references `GLESBackend`/`video/gles_backend.h`** (includes at L23/L28, `GLESBackend::Get()` at L138) → link/compile break on a non-GLES desktop build. Fix = gate the file's registration (or its GLES refs) behind `WALLPAPER_BUILD`.
- **Scope call (SURFACED):** these do **not** block Android (the shipping target), so they are strictly *mergeability* hardening. `docs/wallpaper/engine-changes.md`'s "Upstream mergeability" rule wants a clean upstream merge, which argues for fixing them here. **Recommend: include in Stage 5** (cheap, `#ifdef`-only, no logic change) so the fork stays merge-clean — but they are legitimately deferrable to a dedicated "desktop-build restore" task if Stage 5 must stay Android-only. Decision → Q3.2.

## Known risk / non-goals

- **Race (same as Stage 2's known risk):** the timing sites in `ViewportDoDraw` run on the draw thread; the `StateGameLoop`/`landscape.cpp` sites run on the game thread under `game_state_mutex`. The macros only read the clock and `+=` into distinct fields; the driver resets the struct at period end. Cross-thread `+=` on `_gles_perf` is already the reference's accepted model (single-writer-per-field in practice, best-effort counters — not required to be exact). No new synchronization is introduced. Assessed **low**.
- **Not a correctness stage:** no rendering/behaviour change. If a tuning finding requires a non-trivial rendering change, it is deferred, not done here.
- **Merge risk is the real cost:** the whole point of rule 4 is that these are the shared files with the highest upstream churn (`viewport.cpp`, `openttd.cpp`). Keeping every site a 1-token macro call is the mitigation; a reviewer should reject any reintroduced inline chrono block.

## Files

| File | Status | Change |
|---|---|---|
| `src/video/gles_perf.h` | EDIT | add `GLES_PERF_SCOPE(field)` RAII timer macro (+ guard struct, `<chrono>`) under the `WALLPAPER_PERF` branch (header currently has no includes); OFF branch = `do {} while(0)` |
| `src/viewport.cpp` | EDIT | ViewportDoDraw (L1820) phase timers L1840/1841/1854/1860/1861 + tiles-iterated ~L1264 + count sites (≤5 hunks, macros only); skip signs/text `*_us` (guard-split, ≈0 in wallpaper) |
| `src/openttd.cpp` | EDIT | StateGameLoop (L1268) `gameloop_us` ~L1290 + `tileloop_us` L1324 + `vehicletick_us` L1325 + vehicle-count block; `gameloop_ticks` single-producer per Q3.1 (do NOT double-add) (≤4 hunks) |
| `src/landscape.cpp` | EDIT | single `GLES_PERF_COUNT(_gles_perf.tileloop_count += count)` after RunTileLoop L822 |
| `src/openttd.cpp` (opt.) | EDIT | map-load `Debug(...ms)` line — Open decision (a) |
| `src/openttd.cpp` (opt.) | EDIT | config-load `#ifndef WALLPAPER_BUILD` revisit — Open decision (b) / Q2.1 |
| `src/openttd.cpp` (hardening) | EDIT | gate `LoadWallpaperGame()` case bodies at L1040 + L1213-1215 behind `#ifdef WALLPAPER_BUILD` — Q3.2 |
| `src/wallpaper.cpp` (hardening) | EDIT | gate GLES refs / registration behind `WALLPAPER_BUILD` (non-Android build) — Q3.2 |
| `src/random_access_file.cpp` (hardening) | EDIT | empty-file nullptr-sentinel fix |
| `src/spritecache.cpp` (hardening) | EDIT | `_sprite_file_cache` key: add size/hash (basename-only stale-buffer bug) |
| `src/video/gles_sprite.cpp` (opt.) | EDIT | lazy-alloc atlas layers — only if occupancy warrants (atlas-VRAM item) |
| `tools/perf_monitor.py` | NEW (port) | live TUI dashboard from `gles` |
| `tools/perf_stats.py` | NEW (port) | offline stats from `gles` |
| `tools/perf_collect.sh` | NEW (port) | collect+breakdown wrapper from `gles` |
| `tools/extract_vehicles.py` | NEW (port), optional | GRF sprite dump — Open decision (c) |

No CMake change (all touched `.cpp` already registered; `gles_perf.h` already listed). Build gate: gradle APK (`BUILD SUCCESSFUL`) for `WALLPAPER_PERF=ON` (default). **DECIDED (iter 4, Q2.5): the `WALLPAPER_PERF=OFF` build check is DROPPED as a gate** — the wallpaper always ships PERF-ON and there is no OFF build in CI; the grep proof (Verification gate #3 — only macro-mediated sites, zero bare `_gles_perf.` writes, zero stage-added `steady_clock`) is the sufficient guarantee that the macros vanish under OFF.

## Verification gate (physical arm64 device — Pixel 10 Pro)

1. **PERF-ON build** (default): install, launch wallpaper. logcat shows PERF/VP/TICK/CPU/EXTRA/GPU_SNAP lines at 500 ms cadence with the §2 fields **non-zero** (`vp_land_us`, `vp_draw_us`, `tileloop_us`, `vehicletick_us`, `gameloop_us`, `vehicle_*` all > 0 during a steady run).
2. **Parsers consume it:** `python3 tools/perf_stats.py 20` prints TICK/VP/game-thread breakdowns with sane numbers (no "no data" / all-zero sections); `python3 tools/perf_monitor.py` draws live graphs and its adb-broadcast remote (screen 0) drives map/POI; `tools/perf_collect.sh 20` prints its three breakdown tables.
3. **Zero perf sites leak into an OFF build (grep proof; the OFF-*build* gate itself is DROPPED per Q2.5 — ON always ships):** Grep proof (folded Q1.8): `git grep -n '_gles_perf\.' src/viewport.cpp src/openttd.cpp src/landscape.cpp` must show **only** `GLES_PERF_COUNT`/`GLES_PERF_SCOPE`-wrapped sites (no bare writes), and `git grep GLES_PERF_SCOPE` finds only macro-mediated sites. **Do NOT** grep bare `steady_clock` as a proof — `openttd.cpp` already contains pre-existing upstream `steady_clock` uses (verified 2026-07-08: `_game_session_stats.start_time` at **L528** and **L1113**; autosave `last_time`/`now` at **L1412-1413**) that are false positives; instead prove no chrono leaked from *this stage* via `git diff <stage5-base>..HEAD -- <files> | grep -c 'steady_clock'` == 0 (all timing lives inside the header macro).
4. **simpleperf:** a profile over a steady run shows the expected hotspots (GL replay/paint on the GL thread; sim/tileloop/vehicletick on the game thread) and no surprise CPU sink; captured as the tuning baseline.
5. **Extended stability:** multi-minute run with repeated `SWITCH_MAP` (atlas clear each time) → no crash, no growing GL memory, no persistent artifacts; `gpu_sprites_repacked`/occupancy settle after each reload (Q1.7 confirmation, records shipped clear path).
6. **Idle:** screen-off → game thread parked (≈0% CPU via `top`/simpleperf), `idle_blits` climbing, `swap_us`/`full_renders` ≈0; screen-on resumes cleanly.

## Out of scope (later / not this stage)

- Open-ended rendering optimization beyond the bounded §4 pass (any large refactor of the batch/replay path).
- New perf counters or a new PERF-log line format (would break the matched-pair parsers; only wire the fields that already exist).
- Non-Android / desktop perf paths.
- Settings persistence UX itself (Stage 4) — this stage only decides whether config *load* is re-enabled, not the settings surface.

## Open decisions (recommend + tradeoffs)

> plan-confidence iter 1 self-assessed & **folded**: (a) fixed stop-condition, (c) port perf-triad only, (d) observation-first, (e) keep macros separate. **iter 4 (human-ratified, interactive):** (b) config-load → minimal set gated [Q2.1]; (f) `gameloop_ticks` → single producer in `StateGameLoop` [Q3.1]; (g) desktop build-break → in Stage 5 hardening [Q3.2]; counter fidelity → per-call + document delta [Q2.3]; `gameloop_us` extent → whole-body/normal-only [Q2.4]; OFF-build gate → dropped, grep proof only [Q2.5]. **All Open decisions DECIDED; no open questions remain.**

**(a) Perf-tuning scope / stop condition.** This stage is instrumentation + tooling + a *bounded* tuning pass, but optimization is open-ended. Where is the line, and is there a target metric?
- *Recommend:* fixed stop condition — sustained ≥ device tick rate with headroom, idle CPU ≈0, jank_count≈0/p99≤~2×p50 over a multi-minute run; apply only localized, clearly-attributable findings; anything structural becomes a follow-up. Trade-off: a soft target risks scope creep if treated as an SLA; a hard SLA risks over-investing before real-world battery data exists. Also folds in the optional map-load `Debug(...ms)` timing line — include it only if map-switch latency is a tuning target.
  - **DECIDED (plan-confidence iter 1, self-assessed): adopt the fixed stop-condition; treat as a stop signal, not an SLA. Map-load `Debug(...ms)` line deferred unless map-switch latency surfaces as a tuning target.**

**(b) Config-load re-enable.** Full `LoadFromConfig` vs a minimal wallpaper-specific config vs leave defaults-only?
- *Recommend:* **minimal wallpaper-specific config** — persist only the handful of wallpaper settings (brightness, POI cadence, map rotation) that Stage 3/4 expose, still behind the `#ifndef WALLPAPER_BUILD` gate pattern. Trade-off: full `LoadFromConfig` drags in hotkeys/window-desc/highscore machinery the wallpaper never uses (battery + surface area, and re-opens upstream-merge exposure); defaults-only means user settings don't survive a device reboot. Depends on how Stage 3/4 chose to store settings (Android prefs vs openttd.cfg) — reconcile with those specs before deciding.
  - **DECIDED (plan-confidence iter 4, human-ratified Q2.1): persist a minimal wallpaper-specific set (brightness, POI cadence, map rotation) behind the `#ifndef WALLPAPER_BUILD` gate; NOT full `LoadFromConfig`. The final field wiring is finalized once Stage 3/4's settings-storage (Android prefs vs `openttd.cfg`) lands — that is an execution-time reconciliation, not an open architectural decision.**

**(c) Which python tools to port.** All four parsers vs only what `run_android.py` needs?
- *Recommend:* port `perf_monitor.py`, `perf_stats.py`, `perf_collect.sh` (the perf triad — small, matched to the log format, high dev value). `extract_vehicles.py` is a sprite-decode debug aid unrelated to perf; port it only if atlas/decode debugging is still active — otherwise defer to keep the tree lean (no dead scaffolding). Trade-off: skipping it now means re-porting later if a sprite bug appears; it is self-contained and cheap to bring back.
  - **DECIDED (plan-confidence iter 1, self-assessed): port `perf_monitor.py`/`perf_stats.py`/`perf_collect.sh`; defer `extract_vehicles.py` (no active sprite-decode debugging).**

**(d) Atlas-clear stress test.** Dedicated memory-pressure stress test, or observation during normal `SWITCH_MAP` runs?
- *Recommend:* **observation during normal runs first** (repeated `SWITCH_MAP` + extended runtime is already a real clear+reload workout); add a dedicated stress harness only if occupancy/repack counters don't settle or artifacts appear. Trade-off: a real stress test (rapid forced clears, low-memory simulation) gives stronger evidence for the delete+realloc-vs-in-place decision but is extra tooling the primary path may not need.
  - **DECIDED (plan-confidence iter 1, self-assessed): observation during normal `SWITCH_MAP` runs first; add a dedicated stress harness only if occupancy/repack counters fail to settle or artifacts appear.**

**(e) One macro vs split counter/timer macros.** Keep `GLES_PERF_SCOPE` separate from `GLES_PERF_COUNT`, or unify?
- *Recommend:* **keep them separate.** `GLES_PERF_COUNT(stmt)` is a statement-wrapper (increments/assignments/`max`/blocks); `GLES_PERF_SCOPE(field)` is an RAII timer with different lifetime semantics and a different argument shape (a field name, not a statement). Merging them would overload one name with two behaviours and muddy call-site intent. Trade-off: two macros is marginally more surface in the header, but each reads unambiguously at the call site and both share the single `WALLPAPER_PERF` gate.
  - **DECIDED (plan-confidence iter 1, self-assessed): keep `GLES_PERF_SCOPE` (RAII timer) separate from `GLES_PERF_COUNT` (statement-wrapper).**

**(f) `gameloop_ticks` single producer (NEW — surfaced iter 3).** The Stage-1 driver already increments `gameloop_ticks` in `RecordSnapshot()` (`sdl2_gles_v.cpp:450`); the `gles` reference increments it in `StateGameLoop`. Adding the §2 openttd.cpp site as originally planned would double-count.
- *Recommend:* **(B) single producer in `StateGameLoop`** (next to `tileloop_us`/`vehicletick_us`, matching `gles:openttd.cpp:1307`), and delete the driver's L450 increment — keeps numerator and divisor written in one scope so per-tick averages are coherent. Trade-off vs (A) keep-driver-increment-and-drop-openttd: (A) is fewer edits but counts *recorded snapshots* not *executed ticks* (can diverge when the draw thread lags).
  - **DECIDED (plan-confidence iter 4, human-ratified Q3.1): (B) — single `gameloop_ticks++` producer in `StateGameLoop` next to `tileloop_us`/`vehicletick_us`; DELETE the driver's `sdl2_gles_v.cpp:450` increment.**

**(g) Desktop / non-`WALLPAPER_BUILD` build-break scope (NEW — surfaced iter 3).** Stage 1 left the non-Android build broken (ungated `LoadWallpaperGame()` at `openttd.cpp:1040`/`1213-1215`; `wallpaper.cpp` unconditional GLES refs). Does the fix land in Stage 5?
- *Recommend:* **include in Stage 5 hardening** — cheap `#ifdef`-only, no logic change, and `engine-changes.md`'s mergeability rule wants a clean upstream build. Trade-off: it does not block Android (the shipping target), so it is legitimately deferrable to a dedicated desktop-restore task if Stage 5 must stay Android-only.
  - **DECIDED (plan-confidence iter 4, human-ratified Q3.2): include in Stage 5 hardening — `#ifdef WALLPAPER_BUILD` gates around the `LoadWallpaperGame()` case bodies (`openttd.cpp:1040` + `1213-1215`) and the `wallpaper.cpp` GLES refs/registration.**

## Confidence Survey

*(No open questions. All iteration-2 and iteration-3 questions — Q2.1, Q2.3, Q2.4, Q2.5, Q3.1, Q3.2 — were answered interactively and dissolved at iteration 4; see the Reconciliation Log and the DECIDED markers folded into the body above. Q2.2 was dissolved at iteration 3 as overtaken by events.)*

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-07 (file-based, non-interactive)
- **Confidence:** 68% (cap from Readiness + universal "decide-between/vs" token cap on the Open-decisions section).
- **Resolved:** none (first pass).
- **Verified against current tree (`lwp2`):** `GLES_PERF_COUNT(stmt)` present in `gles_perf.h` (L174-177); **`GLES_PERF_SCOPE` absent** across `src/`; all §2 target fields exist in `GLESPerfCounters` but are written by nothing (driver prints 0). **Stage 1 NOT done**: no `src/video/sdl2_gles_v.*`, no `GM_WALLPAPER` in `src/`, no `Debug(driver,3)` PERF-log block. `perf_monitor.py`/`perf_stats.py`/`perf_collect.sh`/`extract_vehicles.py` exist **only** in `gles`; `run_android.py`+`test_wallpaper.sh` on `lwp2`. Reference sites confirmed (`viewport.cpp` 1846-1889, `openttd.cpp` StateGameLoop 1242-1324, `landscape.cpp:844`) — and their counters use **overlapping spans**, not per-call scopes. Verification-gate #3 grep proof false-positives on pre-existing `steady_clock` in `openttd.cpp`.
- **Findings driving the gap:** (1) five Open decisions (a-e) "not decided here" → Readiness ≤75% + universal ≤70% "vs/decide" tokens; (2) hard dependency on Stages 1-4 **unmet** — §2 sites attach to a non-existent driver/guards [Q2.2]; (3) counter-semantics discrepancy (tidy scopes ≠ reference overlapping spans) [Q2.3/Q2.4]; (4) grep-proof false positives [folded Q1.8].
- **Still uncertain:** Readiness (unmet deps + open decisions) dominates; minor Unknowns (counter fidelity).
- **New questions:** Q1.1(=a) … Q1.5(=e), Q1.6(fidelity), Q1.7(sequencing), Q1.8(grep proof).

### Iteration 2 — 2026-07-07 (file-based, non-interactive)
- **Confidence:** 80% (cap from **Readiness** — Stages 1-4 not green; §2 sites cannot be finalized against a post-Stage-1 tree that does not exist).
- **Resolved (self-assessed → folded → dissolved):**
  - Q1.1(a) → fixed stop-condition (stop signal, not SLA); map-load `Debug(...ms)` deferred → Open decision (a) marked DECIDED.
  - Q1.3(c) → port perf-triad, defer `extract_vehicles.py` → (c) DECIDED.
  - Q1.4(d) → observation-first, dedicated harness only on failure → (d) DECIDED.
  - Q1.5(e) → keep `GLES_PERF_SCOPE`/`GLES_PERF_COUNT` separate → (e) DECIDED.
  - Q1.8 → drop bare `steady_clock` from the grep proof (pre-existing upstream uses); prove via `git diff …| grep -c steady_clock == 0` → Verification gate #3 rewritten.
  - Q1.6 → documented the per-call-vs-reference-span delta as a §2 caveat; residual decision carried → Q2.3/Q2.4.
- **Still uncertain / carried open:** Q1.2(b) config-load [readiness-blocked on Stage 3/4] → Q2.1; sequencing + site re-derivation → Q2.2; counter fidelity → Q2.3/Q2.4; OFF-build gate hardness → Q2.5.
- **Honesty note on the cap:** folding (a)/(c)/(d)/(e) lifted the universal "decide-between" ≤70% token cap; the binding cap is now **Readiness**. Stage 5 cannot be executed, and its §2 site line/context cannot be finalized, until Stages 1-4 (esp. Stage 1's driver PERF-log block + `GM_WALLPAPER` guards) land. This is a genuine dependency gap, not a spec defect — the spec itself is high-quality and thorough. Confidence therefore **plateaus at 80%**; it cannot reach 90% by editing this document alone. No downstream skill invoked (HARD-GATE): survey still open, and the readiness cap is external.
- **New questions:** Q2.1 … Q2.5.

### Iteration 3 — 2026-07-08 (file-based, non-interactive; Stage-1-complete reality update)
- **Confidence:** 88% (cap from **Readiness/Unknowns tie**, not the old Stage-1 readiness gate). Entry-readiness cap **LIFTED** — Stage 1 is complete + device-verified (`lwp2` tip `634dd21703`); the driver + PERF-log block + `GameMode::Wallpaper` guards exist, so §2 sites are now pinned to real file:line. Residual 12% is genuine and not editable away in this doc: (1) §4 tuning findings are empirical (device run); (2) config-load (b) is externally blocked on Stage 3/4 [Q2.1]; (3) a cluster of recommended-but-unratified decisions (Q2.3/Q2.4 fidelity, Q3.1 producer, Q3.2 build-break, Q2.5 OFF-gate).
- **Verified against current tree (all confirmed 2026-07-08):** `sdl2_gles_v.cpp` present; PERF cadence L867; six `Debug(driver,3)` lines L873/885/893/931/938/944; whole block wrapped `GLES_PERF_COUNT({…})` L841-955 incl. `p={}` reset L951 (Q1.8 invariant now proven, not assumed). `gles_perf.h`: `GLES_PERF_COUNT` L173-177, **no `GLES_PERF_SCOPE`**, header has no `#include`. `viewport.cpp`/`openttd.cpp`/`landscape.cpp` still zero `_gles_perf` writes. §2 sites pinned: viewport ViewportDoDraw L1820 (land L1840, vehicles L1841, tilesprites L1854, sort L1860, draw L1861, tiles_iterated ~L1264); openttd StateGameLoop L1268 (gameloop ~L1290, tileloop L1324, vehtick L1325); landscape RunTileLoop L804 (count L822). Scoped enums: `GameMode::Wallpaper`/`GameMode::Editor`, `SwitchMode::Wallpaper`/`SwitchMode::None` (all `GM_*` stale). steady_clock false-positives moved to L528/L1113/L1412-1413.
- **New findings folded:** (i) `gameloop_ticks` already produced by driver `RecordSnapshot()` L450 → §2 openttd.cpp must not double-add; recommend single producer in StateGameLoop [Q3.1]. (ii) Stage-1 `#ifdef WALLPAPER_BUILD` split duplicated the signs/text calls → recommend dropping those `*_us` sub-timers (≈0 in wallpaper, else 2 hunks each). (iii) Hardening scope expanded with three Stage-1 deferrals: atlas 4-layer pre-alloc (~80MB@2048, device-OK, evaluate lazy-alloc); random_access_file empty-file nullptr + basename-only sprite-cache key (robustness, in-scope); desktop non-WALLPAPER_BUILD build breaks (openttd.cpp 1040/1213-1215 ungated + wallpaper.cpp GLES refs) → mergeability, recommend in-scope [Q3.2].
- **Resolved / dissolved:** Q2.2 (Stage-1-incomplete sequencing) → OBE: Stage 1 complete, sites pinned; removed from survey.
- **Still uncertain / carried open:** Q2.1 (config-load, Stage 3/4-blocked), Q2.3 (counter fidelity), Q2.4 (gameloop_us extent — §2 now recommends whole-body/normal-only), Q2.5 (OFF-build gate).
- **New questions:** Q3.1 (`gameloop_ticks` single producer), Q3.2 (desktop build-break scope).
- **Honesty note:** Confidence rose 80→88 because the binding readiness gate (Stage 1 done) dissolved and every §2 site is now pinned to verified code. It stops at 88, not 90, because config-load cannot be finalized without Stage 3/4 and §4's tuning outcomes are empirical — both external to this document. No downstream skill invoked (HARD-GATE: survey open, <90%, no human approval).

### Iteration 4 — 2026-07-08 (interactive — questions asked via prompts, not file checkboxes)
- **Confidence:** 92% (was capped at 88 by a cluster of recommended-but-unratified decisions; that cap is now lifted — all six ratified by the human).
- **Resolved (human-ratified → folded → dissolved):**
  - Q2.1 → minimal wallpaper-specific set behind `#ifndef WALLPAPER_BUILD`, NOT full `LoadFromConfig` → Open decision (b) marked DECIDED; final field wiring reconciled at execution time once Stage 3/4 storage lands (no longer an open architectural decision).
  - Q2.3 → accept per-call semantics, document the delta vs `gles` → §2 counter-semantics caveat marked DECIDED.
  - Q2.4 → `gameloop_us` spans whole tick body (~L1290), normal branch only, skip `GameMode::Editor` → §2 caveat + §2 StateGameLoop table confirmed DECIDED.
  - Q2.5 → **skip the OFF-build check** (user's words): drop the `WALLPAPER_PERF=OFF` build as a gate; grep proof (Verification gate #3) is the sufficient guarantee → Files build-gate note + Verification gate #3 rewritten.
  - Q3.1 → (B) single `gameloop_ticks++` in `StateGameLoop`, delete driver `sdl2_gles_v.cpp:450` → Open decision (f) + §2 marked DECIDED.
  - Q3.2 → include desktop build-break `#ifdef` gates in Stage 5 hardening → Open decision (g) DECIDED; already reflected in Files table.
- **Still uncertain (not spec defects, not editable away here):** §4 tuning findings are empirical (device-run-dependent — resolved during execution); config-load's exact field list is finalized against Stage 3/4's storage choice at implementation time. Neither is an open decision.
- **New questions:** none. Survey empty.
- **Honesty note:** every type-specific tech-spec cap now passes — no open architectural tradeoffs, no "decide-between/vs/TBD" tokens left in the body, all §2 sites pinned, cross-thread risk assessed, in-flight-branch interaction (Stage 1 tip, Stage 3/4 dependency) stated. HARD-GATE still holds: no downstream skill invoked pending explicit user approval of this file.
