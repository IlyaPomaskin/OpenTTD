# Stage 2 — POI Camera (Design Spec)

**Plan-confidence status:** Ready for implementation plan (iteration 3, confidence 90% — Stage 1 hard entry gate now MET; residual cap from Risk: real-scanner behaviour is device-verified only when Stage 2 runs)

**Goal:** In wallpaper / GameActivity mode the camera visits scored map locations (stations, rail-junction clusters, towns, lighthouses) and cycles through them, replacing the Stage 1 stub's fixed map-center. Verified on-device through **GameActivity** (the live WallpaperService path is Stage 3).

**Nature:** A selective re-port, and now literally a file-body swap — the premise is **true as of lwp2 HEAD `634dd21703`**. `src/video/gles_poi.cpp` currently holds the Stage 1 stub (`PrepareBackground` centers the camera on the geometric map center via `ScrollWindowToTile(TileXY(Map::SizeX()/2, Map::SizeY()/2), …)`, `gles_poi.cpp:20-26`; the other four entry points delegate to it or no-op, `gles_poi.cpp:28-31`). Stage 2 replaces that body with the real scanner from branch `gles`, minus vehicle-follow. The `gles_poi.h` API is unchanged except pruning the now-dead `follow_vehicle` field. No renderer/driver/CMake changes: Stage 1 already shipped and device-verified the JNI natives, `ProcessOverlayActions` (`src/video/sdl2_gles_v.cpp:454`), the `DrawPOIMarkers` call site (`src/viewport.cpp:1884`), and source registration.

## Context / dependencies

- **Hard dependency (MET):** Stage 1 complete, green, and device-verified — `gles_poi.cpp` stub in place, GLES renderer working, `wallpaper.cpp` title-map rotation (`CanRotateTitleMap` / `RequestNextTitleMap`), wallpaper-mode boot, and the GameActivity on-device gate passing.
  - **STATUS 2026-07-08 (verified against lwp2 HEAD `634dd21703`):** this dependency is **MET**. Stage 1 Task 9 device gate **ALL PASS on Pixel 10 Pro arm64** (`.superpowers/sdd/progress.md`: renders across 3 distinct scenes, map switch + pause/resume verified, no crash). Every symbol this spec relies on now exists and is cited at real `file:line`: the stub `PrepareBackground`/`NavigatePOI`/`RecenterOnCurrentPOI`/`InvalidatePOIs`/`DrawPOIMarkers` (`src/video/gles_poi.cpp:20-31`), `src/wallpaper.{cpp,h}` with `CanRotateTitleMap` (`wallpaper.cpp:79`) + `RequestNextTitleMap` (`wallpaper.cpp:84`), `ProcessOverlayActions` (`sdl2_gles_v.cpp:454`, invoked from `SnapshotTick` at `:496`), the `DrawPOIMarkers(vp)` call site (`viewport.cpp:1884`, `#ifdef WALLPAPER_BUILD`, `GameMode::Menu||GameMode::Wallpaper`), `PrepareBackground()` invocations (`wallpaper.cpp:155` in `LoadWallpaperGame`, `openttd.cpp:351`, and the JNI native path via `ProcessOverlayActions:471`), and the scoped `GameMode`/`SwitchMode` wallpaper members (`openttd.h:23/28/43`). The "file-body swap of the Stage 1 stub" framing is therefore **now actionable**: the stub and all wiring it swaps into exist. The former Readiness cap is **lifted**.
  - **Device-gate deltas from Stage 1 that Stage 2 inherits (verified):** (1) recording coords shipped as **pointer-math** (explicit `bp.sprite_x/sprite_y` were reverted; `BlitterParams` keeps only `sprite_id`/`pal`) — not consumed by POI camera code, and the port must not reintroduce any explicit-coords assumption. (2) `ProcessOverlayActions` **already holds `game_state_mutex`** around every scan-triggering call (`sdl2_gles_v.cpp:467`, device-gate fix in `634dd21703`) — see Known risk; this is **incidental** Stage-1 coverage, not a Stage-2-owned mitigation (Q1.3/Q2.3 ratified "port as-is, no mutex reliance"). (3) audio is disabled under `WALLPAPER_BUILD` (`misc.cpp`) — unrelated to POI.
- **Reference:** `git show gles:src/video/gles_poi.cpp` (648 lines) and `gles:src/video/gles_poi.h`.
- **Spec:** `docs/wallpaper/wallpaper-mode.md` ("POI camera" section) is the authority for scoring behavior.
- **Reimpl principles (binding, from `docs/wallpaper/overview.md` + `engine-changes.md`):** no dead scaffolding; instrumentation kept but `WALLPAPER_PERF`-gated; upstream mergeability (this file is new/owned code, zero conflict risk); commit per logical unit.

## Scope

### Port faithfully (adapt to ~114-day upstream drift from the `gles` base)

- **Scanners**
  - `ScanStationPOIs` — score train +5 (small stations gated by `IsSmallStationInteresting`), real airport +3 (non-heli, ≥10 nearby buildings), dock +2 (`IsDockInteresting`), bus +1, truck +1 (only with ≥2 other transport facilities); town-population bonus `min(5, pop/500)`; station-cluster +3 and zoom-out (`In2x`) when clustered.
  - `ScanLighthousePOIs` — collect `OBJECT_LIGHTHOUSE` tiles, 5% chance to emit one (score 100).
  - `ScanJunctionPOIs` — plain-rail tiles with ≥3 track bits, BFS-cluster within Manhattan 5 (depth ≤5), clusters of ≥5 junctions emit a POI at the centroid (score 5, or 8 + `In2x` zoom when ≥8 junctions).
  - `ScanTownPOIs` — towns with pop ≥500 not within 30 tiles of an existing station POI; score `min(5, pop/500)`.
- **Helpers:** `CountTownBuildings`, `IsRealAirport`, `IsSmallStationInteresting`, `IsDockInteresting`, `IsStationCluster`, `PickStationPOITile`, `CollectStationInfluences`, `TileToFxy`.
- **Selection (`ScanMapPOIs`):** run scanners → 20-tile edge filter → sort by score desc → build top-50 pool skipping candidates within 10 tiles of an already-selected one → Fisher-Yates pick 20 (or take all if ≤20). Reset index to 0. Keep the reference `std::rand()` seeding as-is (Q1.5: a non-deterministic top-pool subset is acceptable for a wallpaper; no reseeding required). Keep the debug `Debug(driver, …)` breakdown lines (candidate counts, per-POI score/reason) — they are the primary verification signal, and the `driver` channel already surfaces in on-device logcat (Q1.6, verified against the reference).
- **Cycling / camera:**
  - `PrepareBackground` — rescan if map changed or unscanned; on fresh scan show POI[0]; otherwise advance index, and on wrap to 0 (auto mode: `_switch_mode == SwitchMode::None && CanRotateTitleMap()`) call `RequestNextTitleMap()` instead of showing.
  - `NavigatePOI(delta)` — manual browse (sets `_poi_manual_browse`), rescan if needed, step index modulo list, `ShowCurrentPOI`.
  - `RecenterOnCurrentPOI` — reposition current POI without advancing (called after resize).
  - `ShowCurrentPOI` — recenter-only: `RemapCoords` of fractional map pos → set `vp.scrollpos_*` and `vp.dest_scrollpos_*`, `MarkWholeScreenDirty`.
  - `InvalidatePOIs` — clear list + reset scan guard.
  - `DrawPOIMarkers(vp)` — debug overlay: per-POI red rect (white for current) + yellow influence lines. Kept as the on-device placement-verification aid.
- **Rescan trigger:** `_gles_poi_map_tiles` (map tile count at last scan) — a change forces rescan.

### Drop (user decision 2026-07-07 + reimpl "no dead scaffolding")

- `ScanVehiclePOIs` (entire function).
- `GlesPOI.follow_vehicle` field (confirmed present at `gles_poi.h:31`, ported verbatim by Stage 1) and the commented-out follow branch in `ShowCurrentPOI`.
- Follow special-cases in the edge filter and top-50 dedup (they existed only to exempt moving vehicles).
- Vehicle includes no longer needed: **the header's `#include "../vehicle_type.h"`** (`gles_poi.h:17`, the only vehicle include currently in the tree — pulled in solely for `VehicleID` on the dropped `follow_vehicle` field), plus `vehicle_base.h` / `aircraft.h`, which the reference `gles:src/video/gles_poi.cpp` includes for `ScanVehiclePOIs` — the newly ported `.cpp` simply never adds them once that function is dropped.

`GlesPOI.follow_vehicle` is referenced only inside `gles_poi.cpp`, so removing it from the header is safe.

**Keep (Q1.4):** the active `vp.follow_vehicle = VehicleID::Invalid();` reset inside `ShowCurrentPOI` (and `RecenterOnCurrentPOI`) stays. `ViewportData::follow_vehicle` (declared in `src/window_gui.h`, verified present) is an upstream field unrelated to the dropped `GlesPOI` field; resetting it to `Invalid()` is what guarantees the camera stays static at the POI rather than inheriting a stale follow target. Only the commented-out follow *branch* is removed, not this reset line.

## Known risk

`ProcessOverlayActions` (Stage 1, GL thread) drains JNI atomics and calls `NavigatePOI` / `PrepareBackground` / `RotateTitleMap`, which in Stage 2 will run `ScanMapPOIs` reading game state (`Station::Iterate()`, `Town::Iterate()`, tile reads). The original concern was a latent race with game-thread map mutation if this ran **without holding `game_state_mutex`**.

**Decision (Q1.3/Q2.3 — interactive ratification, iteration 4): port as-is, no mutex reliance.** The scanner ports **verbatim from the reference with no locking logic of its own**, and Stage 2 does **not** treat any surrounding lock as its correctness guarantee. The residual scan/mutation race is **accepted as-is** for this stage.

`ProcessOverlayActions` does happen to hold `std::lock_guard<std::mutex> lock(this->game_state_mutex)` (`sdl2_gles_v.cpp:467`, Stage-1 device-gate code) around every scan-triggering call — after a lock-free "nothing pending" fast path (`:459-462`). That lock is **incidental** coverage, not a Stage-2-owned mitigation: it stays because it's Stage-1 code, and it means the two current entry points are in practice serialized against game-thread map mutation —
- **Draw-thread / JNI-triggered** (jump / navigate / rotate) → funnel through `ProcessOverlayActions`.
- **Game-thread-triggered** (`LoadWallpaperGame` → `PrepareBackground`, `wallpaper.cpp:155`; `openttd.cpp:351`) → run on the thread that owns the state.

— but Stage 2 does not depend on that and adds no guard of its own. If the accepted race manifests (crash/corruption under extended run + repeated `SWITCH_MAP`/`NEXT_POI`), or a *new* scan path bypasses `ProcessOverlayActions`, revisit and guard `ScanMapPOIs` explicitly. Verify on the device gate: no crash over extended run + repeated broadcasts, and the longer real scan does not visibly stall the game thread.

## Files

| File | Status | Change |
|---|---|---|
| `src/video/gles_poi.cpp` | REPLACE | stub body → real scanner, no vehicle code |
| `src/video/gles_poi.h` | EDIT | drop `follow_vehicle` field + `vehicle_type.h` include |

No CMake change (registered in Stage 1). Build gate: gradle APK (`BUILD SUCCESSFUL`).

## Upstream-drift checklist (verify each symbol against current tree during port)

> **Verification pass 2026-07-08 (against lwp2 HEAD `634dd21703`):** every game-state / gfx / viewport symbol below was confirmed present. `facilities` and `train_station` live in `BaseStation` (`src/base_station_base.h:69/84`), so `st->facilities`/`st->train_station` still resolve through `Station`'s inheritance. `GfxDrawLine`/`GfxFillRect` now take `PixelColour` / `std::variant<PixelColour,PaletteID>` and the `PC_*` constants are `PixelColour`, so `DrawPOIMarkers` ports unchanged. The scoped enum-class drift below is now fully resolved: Stage 1 landed the wallpaper members, so the exact spellings are **known and in-tree** (no longer owned by an unwritten Stage 1 task).

- **Station:** `st->facilities.Test(StationFacility::{Train,Airport,Dock,BusStop,TruckStop})`, `st->train_station.{w,h,tile}` + `GetCenterTile()`, `st->airport.{type,tile}` + `GetCenterTile()`, `st->docking_station`, `st->bus_station`, `st->truck_station`, `Station::Iterate()`, `INVALID_TILE`.
- **Airport types:** `AT_HELIPORT`, `AT_HELIDEPOT`, `AT_HELISTATION`.
- **Town:** `Town::{Get,Iterate}`, `GetTownIndex`, `t->cache.population`, `t->xy`, `TownID`.
- **Rail:** `IsPlainRailTile`, `GetTrackBits`, `GetRailType`, `CountBits`, `RailType`.
- **Object:** `IsObjectTypeTile(tile, OBJECT_LIGHTHOUSE)`.
- **Tile/map:** `TileXY`, `TileX/Y`, `Map::SizeX/Y`, `IsTileType(t, TileType::{House,Road})`, `GetSlopePixelZ`, `DistanceManhattan`, `TILE_SIZE`.
- **Viewport/window:** `GetMainWindow`, `w->viewport` (`ViewportData`), `vp.{scrollpos_x,scrollpos_y,dest_scrollpos_x,dest_scrollpos_y,virtual_width,virtual_height,virtual_left,virtual_top,left,top,zoom}`, `RemapCoords`, `UnScaleByZoom`, `MarkWholeScreenDirty`, `_cur_dpi` / current `DrawPixelInfo` access in `DrawPOIMarkers`, `GfxDrawLine`, `GfxFillRect`, palette colour consts (`PC_YELLOW/RED/WHITE`).
- **Wallpaper/mode (drift resolved — use these exact scoped names):** upstream uses scoped **`enum class GameMode`** (`src/openttd.h:18`) and **`enum class SwitchMode`** (`src/openttd.h:27`) with unprefixed members. Stage 1 landed the wallpaper members, verified in-tree: **`GameMode::Wallpaper`** (`openttd.h:23`), **`SwitchMode::Wallpaper`** (`openttd.h:43`), and the reference's `SM_NONE` → **`SwitchMode::None`** (`openttd.h:28`). The reference's `GM_WALLPAPER`/`SM_WALLPAPER`/`SM_NONE` tokens are all stale — the port must use the scoped forms throughout (mirroring `wallpaper.cpp:94`, which already writes `GameMode::Wallpaper` / `SwitchMode::Wallpaper`). `CanRotateTitleMap()` (`wallpaper.cpp:79`) / `RequestNextTitleMap()` (`wallpaper.cpp:84`) are present in Stage 1's `wallpaper.{cpp,h}`.

## Verification gate (GameActivity, physical arm64 device)

1. Build APK (gradle), install, launch GameActivity; no crash, no `UnsatisfiedLinkError`.
2. logcat: `GLES ScanMapPOIs: N POIs (map WxH)` with N>0, the candidate breakdown line, and per-POI `score/fx/fy/zoom/reason` lines.
3. Screencaps over time show the camera at **distinct** locations (not fixed map-center); `NEXT_POI` / `PREV_POI` / `JUMP_POI` broadcasts advance the camera visibly; cluster POIs render zoomed out (`In2x`).
4. `SWITCH_MAP` → new map → logcat shows a fresh scan with a new POI list.
5. Edge cases: empty/tiny map (64×64 empty fallback) or a map with no scorable features → no crash, camera holds gracefully (0 POIs handled).
6. Optional: `DrawPOIMarkers` overlay screencap confirms markers land on real features and influence lines connect to contributors.

## Out of scope (later stages)

- Live vehicle-follow camera (dropped this stage; could return as a Stage 3+ enhancement if desired).
- WallpaperService lifecycle / on-device service verification (Stage 3).
- POI scoring tuning against real title maps and perf instrumentation of the scan (Stage 5).

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely.

> This survey was run **file-based / non-interactively** throughout. Iteration-1 questions settleable from the repo tree + `gles` reference were folded into the body. Iteration-2's Q2.1–Q2.3 were the "cannot resolve until Stage 1 is green" plateau; Stage 1 is **now green and device-verified** (`634dd21703`), so all three are resolved from Stage-1 reality and dissolved (see Reconciliation Log iteration 3). **No open blocking questions remain.**

### Iteration 3 — 2026-07-08

_Survey cleared._ The Stage 1 hard entry gate is met, so the former Readiness cap is lifted and Q2.1–Q2.3 fold to fact rather than to a decision:

- **Q2.1 (unblock timing) → UNBLOCKED.** Stage 1 Task 9 device gate passed; the stub + all wiring exist and are cited at `file:line` in Context. Stage 2 may proceed to an implementation plan.
- **Q2.2 (wallpaper enum names) → KNOWN.** `GameMode::Wallpaper` (`openttd.h:23`), `SwitchMode::Wallpaper` (`:43`), `SwitchMode::None` (`:28`) — folded into the drift checklist and Scope.
- **Q2.3 (mutex-guard fallback) → ALREADY IN PLACE + kept in scope.** `ProcessOverlayActions` already holds `game_state_mutex` (`sdl2_gles_v.cpp:467`); rung-2 stays documented in Stage 2 scope for any new bypassing scan path or if the coarse lock proves too broad.

**Residual items are device-gate verification, not spec gaps** (inherent to any pre-execution spec — not resolvable by further file-based iteration): (a) the real 648-line scan runs under `game_state_mutex` on the draw thread — confirm no perceptible game-thread stall; (b) confirm scored POIs land on **real features** (the stub's geometric-center pick can land on ocean — the motivating fix). Both are covered by the Verification gate below.

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-07
- **Detected type:** tech-spec (design spec — named API/components/scanners + file paths, no `### Task N:` / `- [ ] Step N:` blocks).
- **Confidence:** 72% (cap jointly from Readiness + Unknowns).
- **Resolved:** none (first pass).
- **Verified against current tree (lwp2 HEAD) + `gles` reference:**
  - Reference `gles:src/video/gles_poi.cpp` = 648 lines; `ScanVehiclePOIs` + `follow_vehicle` uses at lines 371/398/419/427/447/512–519 (confirms the Drop surface). `gles:src/video/gles_poi.h` carries `follow_vehicle` + `../vehicle_type.h` include as the spec states.
  - **Blocking:** `src/video/gles_poi.{h,cpp}` and `src/wallpaper.{cpp,h}` do **not** exist on lwp2; `CanRotateTitleMap`/`RequestNextTitleMap`/`ProcessOverlayActions`/`DrawPOIMarkers` call sites absent. lwp2 top commit = "GLES: snapshot recording blitter" ⇒ Stage 1 at Task 5/9. The spec's "file-body swap" premise is not yet actionable [Q1.1].
  - **Drift:** `GameMode`/`SwitchMode` are now scoped `enum class` (openttd.h:18/26); `SM_NONE`→`SwitchMode::None`; no `GM_/SM_WALLPAPER` yet [Q1.2].
  - **Confirmed present:** `StationFacility::{Train,TruckStop,BusStop,Airport,Dock}` + `StationFacilities` bitset `.Test/.Any` (base_station_base.h:69), `train_station` (base_station_base.h:84), `airport.type`/`GetCenterTile`/`docking_station`/`bus_station`/`truck_station` (station_base.h), `AT_HELIPORT/HELIDEPOT/HELISTATION` (airport.h), `IsPlainRailTile`/`GetTrackBits`/`GetRailType`/`CountBits`/`DistanceManhattan`, `IsObjectTypeTile`+`OBJECT_LIGHTHOUSE`, `RemapCoords`/`UnScaleByZoom`/`ScaleByZoom`/`GetSlopePixelZ`, ViewportData `scrollpos_*`/`dest_scrollpos_*`/`follow_vehicle` (window_gui.h:252–255 + doc), `Viewport` `virtual_left/width`, `GfxDrawLine`/`GfxFillRect` w/ `PixelColour`, `PC_YELLOW/RED/WHITE`, `_cur_dpi`, `GetMainWindow`, `MarkWholeScreenDirty`, `ScrollWindowToTile`.
- **Findings driving the gap:** (1) hard dependency unmet — Stage 1 not green, no stub/wiring to swap [Q1.1]; (2) scoped-enum drift makes `SM_NONE`/`GM_WALLPAPER` stale in the checklist [Q1.2]; (3) mitigation-ladder rung choice not ratified [Q1.3]; (4) `vp.follow_vehicle` reset-vs-drop ambiguity in `ShowCurrentPOI` [Q1.4]; (5) `std::rand()` determinism unspecified [Q1.5]; (6) verification signal / `Debug(driver,…)` logcat routing unconfirmed [Q1.6]; (7) 0-POI empty-map guards unverified [Q1.7].
- **Still uncertain:** Readiness (Stage 1 red; wallpaper enum member names owned by Stage 1) and Unknowns (drift + unratified detail decisions).
- **New questions:** Q1.1 … Q1.7.

### Iteration 2 — 2026-07-07
- **Confidence:** 75% (cap from **Readiness** — the Stage 1 hard dependency is not green and cannot be lifted by this survey; Unknowns/Risk folds raised those dimensions but Readiness is the min).
- **Resolved (Q1.1–Q1.7 folded, dissolved from survey):**
  - Q1.1 → Stage 1 is at Task 5/9; dependency unmet → Context "STATUS 2026-07-07" note added; spec marked BLOCKED-on-Stage-1 (residual scheduling decision → Q2.1).
  - Q1.2 → scoped `enum class` drift → drift checklist "Wallpaper/mode" bullet rewritten (`SM_NONE`→`SwitchMode::None`; wallpaper members added by Stage 1) (name-pinning → Q2.2).
  - Q1.3 → ratify rung 1 (port as-is, no mutex) as shipping default → Known-risk mitigation ladder annotated (rung-2 scope → Q2.3).
  - Q1.4 → keep the active `vp.follow_vehicle = VehicleID::Invalid();` reset (upstream ViewportData field, unrelated to dropped GlesPOI field) → Drop section "Keep (Q1.4)" note.
  - Q1.5 → keep reference `std::rand()` Fisher-Yates as-is → Selection bullet note.
  - Q1.6 → reuse `Debug(driver,…)` (already in on-device logcat) as verification signal → Selection bullet note.
  - Q1.7 → verified all camera fns (`PrepareBackground`/`ShowCurrentPOI`/`NavigatePOI`/`RecenterOnCurrentPOI`/`DrawPOIMarkers`) early-return on empty list → no body change (already handled; verification gate step 5 stands).
- **Honesty note on the cap:** confidence is held at 75% by an external, un-survey-able fact — Stage 1 is not green, so the exact symbols/wiring the port depends on do not exist and the "swap" cannot be authored or dry-run. The design content itself is strong (thorough scanner spec, explicit drop list, a Known-risk section with a mitigation ladder, empty-map edge case covered), and would clear 85% on Unknowns and ~87% on Risk in isolation. Reporting a Readiness-capped 75% is the honest number; it will not reach 90% until Stage 1 Task 9 passes.
- **Plateau:** confidence plateaus here — remaining Q2.1–Q2.3 are either scheduling calls (Q2.1), Stage-1-gated (Q2.2), or on-device-dependent (Q2.3); none can be resolved by further file-based iteration.
- **Still uncertain:** Readiness (Stage 1 red + wallpaper enum names owned by the unwritten Stage 1 Task 7).
- **New questions:** Q2.1 … Q2.3.
- **No downstream skill invoked (HARD-GATE): confidence < 90% and Stage 1 dependency unmet.**

### Iteration 3 — 2026-07-08 01:30
- **Confidence:** 90% (cap from **Risk** — the residual, and it is the honest ceiling for any pre-execution spec: the real scanner's behaviour, its latency while holding `game_state_mutex`, and POI-placement-on-real-features are verifiable only when Stage 2 runs on device). The Readiness cap that held iterations 1–2 at 72–75% is **lifted** — Stage 1 is green and device-verified.
- **Trigger:** Stage 1 completed and device-verified (Pixel 10 Pro arm64, lwp2 HEAD `634dd21703`; `.superpowers/sdd/progress.md` "TASK 9 DEVICE GATE: ALL PASS"). Re-ran plan-confidence file-based / non-interactively.
- **Verified against current tree (lwp2 `634dd21703`):**
  - Stub present + is a geometric-center pick (can land on ocean): `gles_poi.cpp:20-31`. `gles_poi.h` still carries `follow_vehicle` (`:31`) + the lone `../vehicle_type.h` include (`:17`) — the exact Drop surface, ported verbatim by Stage 1.
  - All swap-target wiring present: `ProcessOverlayActions` (`sdl2_gles_v.cpp:454`, `game_state_mutex` at `:467`, called from `SnapshotTick:496`); `DrawPOIMarkers(vp)` call site (`viewport.cpp:1884`, `WALLPAPER_BUILD`-guarded); `CanRotateTitleMap`/`RequestNextTitleMap` (`wallpaper.cpp:79/84`); `PrepareBackground` invoked at `wallpaper.cpp:155` + `openttd.cpp:351` + JNI path `sdl2_gles_v.cpp:471`.
  - Scoped enums in-tree: `GameMode`@`openttd.h:18` (`Wallpaper`@`:23`); `SwitchMode`@`:27` (`None`@`:28`, `Wallpaper`@`:43`). `wallpaper.cpp:94` already uses the scoped forms.
  - Stage-1 device-gate deltas inherited: coords reverted to pointer-math (`BlitterParams` sprite_x/y dropped — no explicit-coords assumption to reintroduce); overlay `game_state_mutex` added; audio disabled under `WALLPAPER_BUILD`.
- **Resolved (Q2.1–Q2.3 folded, dissolved from survey):**
  - Q2.1 → UNBLOCKED (Stage 1 Task 9 device gate PASS) → Context "STATUS 2026-07-08" note rewritten to MET; top status → "Ready for implementation plan"; Nature reframed to actionable file-body swap.
  - Q2.2 → enum names KNOWN (`GameMode::Wallpaper`/`SwitchMode::Wallpaper`/`SwitchMode::None`) → drift-checklist "Wallpaper/mode" bullet + Scope `PrepareBackground` bullet updated to scoped forms.
  - Q2.3 → rung-2 mutex guard ALREADY IN PLACE (`ProcessOverlayActions` holds `game_state_mutex`, `:467`) and kept in Stage 2 scope for any bypassing path → Known-risk section rewritten (risk now latency, not correctness).
- **Scoped-enum sweep:** replaced the last body `SM_NONE` (Scope `PrepareBackground`) with `SwitchMode::None`; drift bullet lists the exact scoped members. Reconciliation-log entries for iterations 1–2 left verbatim (append-only).
- **Still uncertain:** Risk only — real-scanner latency-under-mutex and POI-lands-on-real-features are device-gate items (Verification gate steps 2/3/6), not authorable in the spec.
- **Status set:** `Ready for implementation plan`. Suggested downstream skill: `superpowers:writing-plans`. **NOT invoked** (HARD-GATE: awaits explicit user review + approval).

### Iteration 4 — 2026-07-08 (interactive ratification)
- **Trigger:** user re-ran the resolved detail decisions as an interactive form (AskUserQuestion). No new questions; ratify/override the file-based folds. Confidence unchanged (90%, Risk-capped).
- **Ratified as-recommended:** Q1.4 (keep `vp.follow_vehicle = Invalid()` reset → camera always static on POI, vehicle-follow removed); Q1.5 (keep `std::rand()` Fisher-Yates as-is); Q1.6 (`Debug(driver,…)` logcat as primary verification signal).
- **OVERRIDE — Q1.3/Q2.3 (locking):** user chose **"port as-is, no mutex reliance"** over "rely on existing mutex." Meaning: the POI scanner ports **verbatim with no locking logic of its own**; the `game_state_mutex` held by `ProcessOverlayActions` (`sdl2_gles_v.cpp:467`) stays as Stage-1 code but is **no longer a Stage-2-owned correctness mitigation** — the residual scan/mutation race is accepted as-is. Known-risk framing shifts back from "correctness resolved" to "port-as-is, race accepted; existing lock incidental."
