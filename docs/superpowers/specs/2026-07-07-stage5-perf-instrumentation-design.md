# Stage 5 — Perf Instrumentation, Tooling & Hardening (Design Spec)

**Plan-confidence status:** Draft (iteration 2, confidence 80%). Cap: Readiness — Stage 5 hard-depends on Stages 1–4 green, and Stage 1 is verified **incomplete** on `lwp2` (no `sdl2_gles_v.*` driver, no `GM_WALLPAPER`, no PERF-log block, no `GLES_PERF_SCOPE`). The §2 instrumentation sites attach to code that does not yet exist in-tree; their exact placement/line-context can only be finalized against the post-Stage-1 guarded files. See Confidence Survey Q2.2.

**Goal:** Close the instrumentation gap Stage 1 deferred and make the wallpaper shipping-quality. Add the `GLES_PERF_SCOPE(field)` RAII chrono macro; wire the viewport/gameloop timing **sites** through it and `GLES_PERF_COUNT` as 1-line hunks (mergeability rule 4); port the offline/live perf parsers from `gles` so the C++ PERF log lines are consumable; run a bounded perf-tuning pass against `tools/run_android.py perf/fps` + simpleperf baselines; and land the hardening items Stage 1 punted (atlas-clear verification, config-load decision, battery/idle behaviour).

**Nature:** A mixed stage. Two parts are owned/low-risk (the macro in `src/video/gles_perf.h`; four python tools under `tools/`). One part is the highest-merge-risk work in the fork — timing edits inside shared upstream files (`viewport.cpp`, `openttd.cpp`, `landscape.cpp`) — which is precisely why it is gated to **1-line-per-site** hunks. The tuning and hardening parts are measurement + decision passes, not large code.

## Context / dependencies

- **Hard dependency:** Stages 1–4 complete and green. Stage 5 needs the driver's 500 ms PERF-log block (Stage 1 Task 8, reference `gles:src/video/sdl2_gles_v.cpp:690-772`), the `GM_WALLPAPER` boot + guards (Stage 1 Task 7), the real POI scanner (Stage 2), and the WallpaperService lifecycle (Stage 3) all in place. This stage adds no new rendering behaviour.
- **Reference (read-only):** `gles:src/viewport.cpp` (ViewportDoDraw chrono block, lines 1846-1889), `gles:src/openttd.cpp` (StateGameLoop, lines 1242-1324), `gles:src/landscape.cpp:844`, `gles:src/video/sdl2_gles_v.cpp` (PERF log block + jank ring buffer, ~670-772), `gles:tools/{perf_monitor.py,perf_stats.py,perf_collect.sh,extract_vehicles.py}`. Extract with `git show gles:<path>`; NEVER modify `gles`.
- **Spec authority:** `docs/wallpaper/rendering.md` ("Frame rendering" three frame classes, "CPU-side render optimizations", "Perf counters"), `docs/wallpaper/engine-changes.md` (mergeability rule 4; "Misc" instrumentation sites), `docs/wallpaper/tools.md` (the matched C++/python pair), `docs/wallpaper/overview.md` (instrumentation-gated decision).
- **Current-tree state (verified on `lwp2`, 2026-07-07):**
  - `src/video/gles_perf.h` ships `GLESPerfCounters`, `extern _gles_perf`, and `GLES_PERF_COUNT(stmt)` (no-op unless `WALLPAPER_PERF`). **`GLES_PERF_SCOPE` does not exist.**
  - `src/viewport.cpp`, `src/openttd.cpp`, `src/landscape.cpp` contain **zero** `_gles_perf` writes. Consequently the struct fields `vp_land_us`, `vp_vehicles_us`, `vp_sort_us`, `vp_draw_us`, `vp_tiles_iterated`, `vp_parent_sprites`, `vp_child_sprites`, `vp_calls`, `vp_area_w/h`, `tileloop_us`, `tileloop_count`, `vehicletick_us`, `gameloop_us`, `gameloop_ticks`, `vehicle_trains/road/ships/aircraft` are populated by nothing — the driver PERF block prints them as **0**. Wiring these is the core of this stage.
  - `GLES_PERF_COUNT` is already used in `gles_backend.cpp`, `gles_sprite.cpp` (GL-thread render/atlas counters were done in Stage 1). No bare `_gles_perf.` writes exist outside the macro anywhere in `src/`.
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

**`src/viewport.cpp` — `ViewportDoDraw`** (reference block: `gles:src/viewport.cpp:1846-1889`). Each phase call gets wrapped in a brace-scoped `GLES_PERF_SCOPE`:

| Field (`_gles_perf.`) | Site |
|---|---|
| `vp_land_us` | scope around `ViewportAddLandscape()` |
| `vp_vehicles_us` | scope around `ViewportAddVehicles(&_vd.dpi)` |
| `vp_tilesprites_us` (+ `vp_signs_tiles_us`) | scope around `ViewportDrawTileSprites(...)` |
| `vp_sort_us` | scope around `_vp_sprite_sorter(...)` |
| `vp_draw_us` | scope around `ViewportDrawParentSprites(...)` |

Count fields via `GLES_PERF_COUNT` (one call each): `vp_calls++`, `vp_tile_sprites`, `vp_strings_queued`, `vp_sprites_generated`, `vp_parent_sprites`, `vp_child_sprites`, `vp_area_w`/`vp_area_h` (`= std::max(...)`). The `vp_tiles_iterated++` site is a single `GLES_PERF_COUNT` inside `ViewportAddLandscape` (reference line 1324).
Note: `vp_kdtree_us`, `vp_texteff_us`, `vp_kdtree_found`, `vp_strings_queued` sit inside the `if (_game_mode != GM_WALLPAPER)` branch Stage 1 already guards — in wallpaper they read ≈0. Keep the sites (harmless, and correct for a non-wallpaper/upstream build); do not special-case them.

**`src/openttd.cpp` — `StateGameLoop`** (reference: `gles:src/openttd.cpp:1242-1324`):

| Field | Site |
|---|---|
| `gameloop_us` | scope spanning the tick body (from first sim call to end), `GLES_PERF_SCOPE(gameloop_us)` at top of the active branch |
| `tileloop_us` | scope around `RunTileLoop()` |
| `vehicletick_us` | scope around `CallVehicleTicks()` |
| `gameloop_ticks` | `GLES_PERF_COUNT(_gles_perf.gameloop_ticks++)` |
| `vehicle_trains/road/ships/aircraft` | the `Vehicle::Iterate()` counting loop wrapped as **one** `GLES_PERF_COUNT({ ...block... })` hunk (the macro's `do { stmt; } while(0)` form accepts a braced block with no top-level commas) |

The reference instruments both the `GM_EDITOR` and normal branches; the wallpaper only ever runs the normal branch, so instrument that one (add the editor branch too only if trivially symmetric — otherwise skip it, no wallpaper impact). This keeps `openttd.cpp` to ~4 hunks.

**`src/landscape.cpp`** — single `GLES_PERF_COUNT(_gles_perf.tileloop_count += 1 << (Map::LogX() + Map::LogY() - TILE_UPDATE_FREQUENCY_LOG))` at reference line 844 (inside `RunTileLoop`).

**Map-load / saveload timing (optional, secondary):** engine-changes.md "Misc" lists saveload timing, but `gles` has **no `_gles_perf` field** for it — the reference logs map-load duration as a plain `Debug(...)` line in `LoadIntroGame` (gles openttd.cpp ~333-362). Recommendation: port that as a single `Debug(driver, 1, ...ms)` line (not a counter) if map-switch latency needs a number; otherwise drop it. Flagged in Open decision (a) as tuning-scope.

**Counter-semantics caveat (verified against `gles`, still-open Q2.3/Q2.4).** The reference does NOT time these phases with tidy per-call scopes; it uses overlapping `_t*`/`gl_t*` spans: `tileloop_us` = `gl_t0..gl_t1` (spans `AnimateAnimatedTiles` + `TimerManager::Elapsed` + `RunTileLoop`, **not** `RunTileLoop` alone); `gameloop_us` `gl_t0` is set *before* the `GM_EDITOR`/normal branch (spans the whole tick body); `vp_signs_tiles_us` = `_t2.._t3` (overlaps `vp_tilesprites_us` = `_t2b.._t3`). A literal "scope around `RunTileLoop()`" per §2 therefore yields a **narrower** number than the reference. Whether the bounded tuning pass needs reference-comparable values (→ wrap the broader spans) or the tidier per-call semantics are fine (→ document the delta) is Confidence Survey Q2.3; the `gameloop_us` extent + whether to instrument the `GM_EDITOR` branch is Q2.4.

**Invariant to preserve:** the driver's PERF-log block already reads all of the above fields (Stage 1 Task 8). This stage only makes them non-zero. No change to the log block itself except confirming every field it prints now has a producer, and confirming the whole block + every `_gles_perf.*` read in it remains wrapped in `GLES_PERF_COUNT` (Q1.8) so an OFF build drops it entirely.

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

## Known risk / non-goals

- **Race (same as Stage 2's known risk):** the timing sites in `ViewportDoDraw` run on the draw thread; the `StateGameLoop`/`landscape.cpp` sites run on the game thread under `game_state_mutex`. The macros only read the clock and `+=` into distinct fields; the driver resets the struct at period end. Cross-thread `+=` on `_gles_perf` is already the reference's accepted model (single-writer-per-field in practice, best-effort counters — not required to be exact). No new synchronization is introduced. Assessed **low**.
- **Not a correctness stage:** no rendering/behaviour change. If a tuning finding requires a non-trivial rendering change, it is deferred, not done here.
- **Merge risk is the real cost:** the whole point of rule 4 is that these are the shared files with the highest upstream churn (`viewport.cpp`, `openttd.cpp`). Keeping every site a 1-token macro call is the mitigation; a reviewer should reject any reintroduced inline chrono block.

## Files

| File | Status | Change |
|---|---|---|
| `src/video/gles_perf.h` | EDIT | add `GLES_PERF_SCOPE(field)` RAII timer macro (+ guard struct, `<chrono>`) under the `WALLPAPER_PERF` branch; OFF branch = `do {} while(0)` |
| `src/viewport.cpp` | EDIT | ViewportDoDraw phase timers + count sites (≤5 hunks, macros only) |
| `src/openttd.cpp` | EDIT | StateGameLoop tileloop/vehicletick/gameloop timers + vehicle-count block + `gameloop_ticks` (≤4 hunks) |
| `src/landscape.cpp` | EDIT | single `tileloop_count` `GLES_PERF_COUNT` in RunTileLoop |
| `src/openttd.cpp` (opt.) | EDIT | map-load `Debug(...ms)` line — Open decision (a) |
| `src/openttd.cpp` (opt.) | EDIT | config-load `#ifndef WALLPAPER_BUILD` revisit — Open decision (b) |
| `tools/perf_monitor.py` | NEW (port) | live TUI dashboard from `gles` |
| `tools/perf_stats.py` | NEW (port) | offline stats from `gles` |
| `tools/perf_collect.sh` | NEW (port) | collect+breakdown wrapper from `gles` |
| `tools/extract_vehicles.py` | NEW (port), optional | GRF sprite dump — Open decision (c) |

No CMake change (all touched `.cpp` already registered; `gles_perf.h` already listed). Build gate: gradle APK (`BUILD SUCCESSFUL`) for both `WALLPAPER_PERF=ON` (default) and a spot-check `WALLPAPER_PERF=OFF` build.

## Verification gate (physical arm64 device — Pixel 10 Pro)

1. **PERF-ON build** (default): install, launch wallpaper. logcat shows PERF/VP/TICK/CPU/EXTRA/GPU_SNAP lines at 500 ms cadence with the §2 fields **non-zero** (`vp_land_us`, `vp_draw_us`, `tileloop_us`, `vehicletick_us`, `gameloop_us`, `vehicle_*` all > 0 during a steady run).
2. **Parsers consume it:** `python3 tools/perf_stats.py 20` prints TICK/VP/game-thread breakdowns with sane numbers (no "no data" / all-zero sections); `python3 tools/perf_monitor.py` draws live graphs and its adb-broadcast remote (screen 0) drives map/POI; `tools/perf_collect.sh 20` prints its three breakdown tables.
3. **PERF-OFF build compiles clean with zero perf sites:** a `-DWALLPAPER_PERF` -absent build is `BUILD SUCCESSFUL`. Grep proof (folded Q1.8): `git grep -n '_gles_perf\.' src/viewport.cpp src/openttd.cpp src/landscape.cpp` must show **only** `GLES_PERF_COUNT`/`GLES_PERF_SCOPE`-wrapped sites (no bare writes), and `git grep GLES_PERF_SCOPE` finds only macro-mediated sites. **Do NOT** grep bare `steady_clock` as a proof — `openttd.cpp` already contains pre-existing upstream `steady_clock` uses (`_game_session_stats.start_time` ~L498/L1057, autosave `last_time`/`now` ~L1351) that are false positives; instead prove no chrono leaked from *this stage* via `git diff <stage5-base>..HEAD -- <files> | grep -c 'steady_clock'` == 0 (all timing lives inside the header macro).
4. **simpleperf:** a profile over a steady run shows the expected hotspots (GL replay/paint on the GL thread; sim/tileloop/vehicletick on the game thread) and no surprise CPU sink; captured as the tuning baseline.
5. **Extended stability:** multi-minute run with repeated `SWITCH_MAP` (atlas clear each time) → no crash, no growing GL memory, no persistent artifacts; `gpu_sprites_repacked`/occupancy settle after each reload (Q1.7 confirmation, records shipped clear path).
6. **Idle:** screen-off → game thread parked (≈0% CPU via `top`/simpleperf), `idle_blits` climbing, `swap_us`/`full_renders` ≈0; screen-on resumes cleanly.

## Out of scope (later / not this stage)

- Open-ended rendering optimization beyond the bounded §4 pass (any large refactor of the batch/replay path).
- New perf counters or a new PERF-log line format (would break the matched-pair parsers; only wire the fields that already exist).
- Non-Android / desktop perf paths.
- Settings persistence UX itself (Stage 4) — this stage only decides whether config *load* is re-enabled, not the settings surface.

## Open decisions (recommend + tradeoffs)

> plan-confidence iter 1 self-assessed & **folded**: (a) fixed stop-condition, (c) port perf-triad only, (d) observation-first, (e) keep macros separate. **Still open** (carried to Confidence Survey): (b) config-load — blocked on Stage 3/4 settings-storage → Q2.1.

**(a) Perf-tuning scope / stop condition.** This stage is instrumentation + tooling + a *bounded* tuning pass, but optimization is open-ended. Where is the line, and is there a target metric?
- *Recommend:* fixed stop condition — sustained ≥ device tick rate with headroom, idle CPU ≈0, jank_count≈0/p99≤~2×p50 over a multi-minute run; apply only localized, clearly-attributable findings; anything structural becomes a follow-up. Trade-off: a soft target risks scope creep if treated as an SLA; a hard SLA risks over-investing before real-world battery data exists. Also folds in the optional map-load `Debug(...ms)` timing line — include it only if map-switch latency is a tuning target.
  - **DECIDED (plan-confidence iter 1, self-assessed): adopt the fixed stop-condition; treat as a stop signal, not an SLA. Map-load `Debug(...ms)` line deferred unless map-switch latency surfaces as a tuning target.**

**(b) Config-load re-enable.** Full `LoadFromConfig` vs a minimal wallpaper-specific config vs leave defaults-only?
- *Recommend:* **minimal wallpaper-specific config** — persist only the handful of wallpaper settings (brightness, POI cadence, map rotation) that Stage 3/4 expose, still behind the `#ifndef WALLPAPER_BUILD` gate pattern. Trade-off: full `LoadFromConfig` drags in hotkeys/window-desc/highscore machinery the wallpaper never uses (battery + surface area, and re-opens upstream-merge exposure); defaults-only means user settings don't survive a device reboot. Depends on how Stage 3/4 chose to store settings (Android prefs vs openttd.cfg) — reconcile with those specs before deciding.
  - **STILL OPEN (readiness-blocked): the deciding input — Stage 3/4's settings-storage mechanism — does not yet exist in-tree. Cannot fold without it. → Confidence Survey Q2.1.**

**(c) Which python tools to port.** All four parsers vs only what `run_android.py` needs?
- *Recommend:* port `perf_monitor.py`, `perf_stats.py`, `perf_collect.sh` (the perf triad — small, matched to the log format, high dev value). `extract_vehicles.py` is a sprite-decode debug aid unrelated to perf; port it only if atlas/decode debugging is still active — otherwise defer to keep the tree lean (no dead scaffolding). Trade-off: skipping it now means re-porting later if a sprite bug appears; it is self-contained and cheap to bring back.
  - **DECIDED (plan-confidence iter 1, self-assessed): port `perf_monitor.py`/`perf_stats.py`/`perf_collect.sh`; defer `extract_vehicles.py` (no active sprite-decode debugging).**

**(d) Atlas-clear stress test.** Dedicated memory-pressure stress test, or observation during normal `SWITCH_MAP` runs?
- *Recommend:* **observation during normal runs first** (repeated `SWITCH_MAP` + extended runtime is already a real clear+reload workout); add a dedicated stress harness only if occupancy/repack counters don't settle or artifacts appear. Trade-off: a real stress test (rapid forced clears, low-memory simulation) gives stronger evidence for the delete+realloc-vs-in-place decision but is extra tooling the primary path may not need.
  - **DECIDED (plan-confidence iter 1, self-assessed): observation during normal `SWITCH_MAP` runs first; add a dedicated stress harness only if occupancy/repack counters fail to settle or artifacts appear.**

**(e) One macro vs split counter/timer macros.** Keep `GLES_PERF_SCOPE` separate from `GLES_PERF_COUNT`, or unify?
- *Recommend:* **keep them separate.** `GLES_PERF_COUNT(stmt)` is a statement-wrapper (increments/assignments/`max`/blocks); `GLES_PERF_SCOPE(field)` is an RAII timer with different lifetime semantics and a different argument shape (a field name, not a statement). Merging them would overload one name with two behaviours and muddy call-site intent. Trade-off: two macros is marginally more surface in the header, but each reads unambiguously at the call site and both share the single `WALLPAPER_PERF` gate.
  - **DECIDED (plan-confidence iter 1, self-assessed): keep `GLES_PERF_SCOPE` (RAII timer) separate from `GLES_PERF_COUNT` (statement-wrapper).**

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely. (Ran file-based / non-interactive: iter-1 questions were self-assessed and folded; the questions below are the ones that remain genuinely open — most are **readiness-blocked** on work not yet in-tree, so they are left unchecked pending a human call.)

### Iteration 2 — 2026-07-07

#### Q2.1. Config-load re-enable (Open decision (b)) is blocked: the deciding input — Stage 3/4's settings-storage mechanism (Android prefs vs `openttd.cfg`) — does not exist in-tree yet. What is the Stage-5 position on config *load*?
- [ ] Persist a minimal wallpaper-specific set (brightness, POI cadence, map rotation) behind `#ifndef WALLPAPER_BUILD`, finalized once Stage 3/4 lands  *(Recommended)*
- [ ] Re-enable full `LoadFromConfig`/`LoadHotkeysFromConfig`/`WindowDesc::LoadFromConfig`
- [ ] Leave defaults-only (no persistence); revisit post-ship
- [ ] Remove config-load from Stage 5 scope entirely; fold it into Stage 4 (settings surface)

#### Q2.2. Stage 1 is verified incomplete on `lwp2` (no `sdl2_gles_v.*`, no `GM_WALLPAPER`, no PERF-log block, no `GLES_PERF_SCOPE`). The §2 sites attach to the guarded, post-Stage-1 forms of `viewport.cpp`/`openttd.cpp`, which don't exist yet. How should this spec proceed?
- [ ] Gate Stage 5 start on Stages 1–4 green; treat every `gles:` line number as a template and re-derive each §2 site against the post-Stage-1 guarded files before writing the impl plan  *(Recommended)*
- [ ] Write the Stage 5 impl plan now using the reference line numbers as-is; fix drift during execution
- [ ] Split Stage 5: do the owned/low-risk parts (macro + tools port) now; defer the shared-file instrumentation until Stages 1–4 land
- [ ] Move §2 instrumentation into Stage 1 Task 8 (co-locate with the driver PERF block); Stage 5 keeps only tooling + tuning + hardening

#### Q2.3. The reference times phases with overlapping `_t*`/`gl_t*` spans; tidy per-call `GLES_PERF_SCOPE` changes what several counters measure (`tileloop_us` = `RunTileLoop` only, not `gl_t0..gl_t1`; `vp_signs_tiles_us` subsumed by `vp_tilesprites_us`). How to reconcile?
- [ ] Accept per-call semantics; document the delta vs `gles` — exact parity isn't needed for a bounded tuning pass  *(Recommended)*
- [ ] Reproduce the reference spans exactly (wrap the broader `gl_t0..gl_t1` / `_t2.._t3` regions) so `perf_stats.py` numbers match historical `gles` baselines
- [ ] Emit both a narrow per-call scope and the broad span into distinct fields; compare once, then keep one
- [ ] Drop the affected sub-counters (`tileloop_us`, `vp_signs_tiles_us`); rely on `gameloop_us` + `vehicletick_us` only

#### Q2.4. `gameloop_us` extent + editor branch: the reference sets `gl_t0` *before* the `GM_EDITOR`/normal branch (spans the whole tick body) and instruments both branches; §2 currently says "at top of the active branch". Which shape?
- [ ] `gameloop_us` spans the whole tick body (match reference); instrument the normal branch only (skip `GM_EDITOR` — never runs in wallpaper)  *(Recommended)*
- [ ] Scope `gameloop_us` to the active branch only (as §2 text reads now), accepting a narrower number
- [ ] Instrument both `GM_EDITOR` and normal branches symmetrically for full reference parity
- [ ] Defer `gameloop_us` until the tuning pass shows the tick body is a hotspot

#### Q2.5. The build gate wants both `WALLPAPER_PERF=ON` and a spot-check `=OFF` build green, but no OFF build exists in CI and the wallpaper always ships PERF-ON for now. How hard is the OFF gate?
- [ ] Best-effort spot-check: build OFF once during Stage 5 to confirm the macros vanish; not per-commit  *(Recommended)*
- [ ] Hard gate: every Stage 5 commit must build green under both ON and OFF
- [ ] Drop the OFF build; rely on the grep proof (no bare perf sites) alone
- [ ] Add an OFF build to CI as a Stage 5 deliverable

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
