# Stage 2 — POI Camera (Design Spec)

**Plan-confidence status:** Draft (iteration 2, confidence 75% — cap from Readiness: Stage 1 hard dependency not yet green)

**Goal:** In wallpaper / GameActivity mode the camera visits scored map locations (stations, rail-junction clusters, towns, lighthouses) and cycles through them, replacing the Stage 1 stub's fixed map-center. Verified on-device through **GameActivity** (the live WallpaperService path is Stage 3).

**Nature:** A selective re-port. `src/video/gles_poi.cpp` is a file-body swap — the Stage 1 stub is replaced by the real scanner from branch `gles`, minus vehicle-follow. The `gles_poi.h` API is unchanged except pruning the now-dead `follow_vehicle` field. No renderer/driver/CMake changes: Stage 1 already wired the JNI natives, `ProcessOverlayActions`, the `DrawPOIMarkers` call site, and source registration.

## Context / dependencies

- **Hard dependency:** Stage 1 complete and green — `gles_poi.cpp` stub in place, GLES renderer working, `wallpaper.cpp` title-map rotation (`CanRotateTitleMap` / `RequestNextTitleMap`), wallpaper-mode boot, and the GameActivity on-device gate passing. Stage 2 cannot begin until then.
  - **STATUS 2026-07-07 (Q1.1 — verified against lwp2 HEAD):** this dependency is **NOT met**. Stage 1 is at **Task 5 of 9** (last commit "GLES: snapshot recording blitter"). None of `src/video/gles_poi.{h,cpp}`, `src/wallpaper.{cpp,h}`, `CanRotateTitleMap`/`RequestNextTitleMap`, `ProcessOverlayActions`, the `DrawPOIMarkers` call site, or the `GameMode`/`SwitchMode` wallpaper members exist in the tree yet (all are created by Stage 1 Tasks 6–9). Consequently the spec's framing as a "file-body swap of the Stage 1 stub" is **forward-looking, not currently actionable** — there is no stub to swap and no wiring to rely on. This spec is a ratified design that stays **BLOCKED on Stage 1 green**; it is not ready to hand to an implementation-plan / execution step until Stage 1 Task 9 passes (Q2.1). This is the binding cap on confidence.
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
  - `PrepareBackground` — rescan if map changed or unscanned; on fresh scan show POI[0]; otherwise advance index, and on wrap to 0 (auto mode, `SM_NONE`, `CanRotateTitleMap()`) call `RequestNextTitleMap()` instead of showing.
  - `NavigatePOI(delta)` — manual browse (sets `_poi_manual_browse`), rescan if needed, step index modulo list, `ShowCurrentPOI`.
  - `RecenterOnCurrentPOI` — reposition current POI without advancing (called after resize).
  - `ShowCurrentPOI` — recenter-only: `RemapCoords` of fractional map pos → set `vp.scrollpos_*` and `vp.dest_scrollpos_*`, `MarkWholeScreenDirty`.
  - `InvalidatePOIs` — clear list + reset scan guard.
  - `DrawPOIMarkers(vp)` — debug overlay: per-POI red rect (white for current) + yellow influence lines. Kept as the on-device placement-verification aid.
- **Rescan trigger:** `_gles_poi_map_tiles` (map tile count at last scan) — a change forces rescan.

### Drop (user decision 2026-07-07 + reimpl "no dead scaffolding")

- `ScanVehiclePOIs` (entire function).
- `GlesPOI.follow_vehicle` field and the commented-out follow branch in `ShowCurrentPOI`.
- Follow special-cases in the edge filter and top-50 dedup (they existed only to exempt moving vehicles).
- Vehicle includes no longer needed: `vehicle_base.h`, `aircraft.h`, and `vehicle_type.h` (from `gles_poi.h`).

`GlesPOI.follow_vehicle` is referenced only inside `gles_poi.cpp`, so removing it from the header is safe.

**Keep (Q1.4):** the active `vp.follow_vehicle = VehicleID::Invalid();` reset inside `ShowCurrentPOI` (and `RecenterOnCurrentPOI`) stays. `ViewportData::follow_vehicle` (declared in `src/window_gui.h`, verified present) is an upstream field unrelated to the dropped `GlesPOI` field; resetting it to `Invalid()` is what guarantees the camera stays static at the POI rather than inheriting a stale follow target. Only the commented-out follow *branch* is removed, not this reset line.

## Known risk

`ProcessOverlayActions` (Stage 1, GL thread) drains JNI atomics and calls `NavigatePOI` / `PrepareBackground`, which may run `ScanMapPOIs` reading game state (`Station::Iterate()`, `Town::Iterate()`, tile reads) **without holding `game_state_mutex`** — a latent race with game-thread map mutation. In practice rescans fire almost exclusively at map load (the tile-count-change trigger), when the sim is quiescent; steady spectator sim on a title map rarely creates/removes stations. Risk assessed **low**.

**Mitigation ladder (plan carries these; ship the lowest that passes the device gate):**
1. Port as-is; verify no crash over extended run + repeated `SWITCH_MAP`/`NEXT_POI`. **(Q1.3: ratified as the shipping default — rung 1 goes first.)**
2. If crashes appear: guard `ScanMapPOIs` (and the scan-triggering paths) with `game_state_mutex`. Kept in **Stage 2** scope as the fallback, not punted to Stage 5 (open confirmation Q2.3).

## Files

| File | Status | Change |
|---|---|---|
| `src/video/gles_poi.cpp` | REPLACE | stub body → real scanner, no vehicle code |
| `src/video/gles_poi.h` | EDIT | drop `follow_vehicle` field + `vehicle_type.h` include |

No CMake change (registered in Stage 1). Build gate: gradle APK (`BUILD SUCCESSFUL`).

## Upstream-drift checklist (verify each symbol against current tree during port)

> **Verification pass 2026-07-07 (against lwp2 HEAD):** every game-state / gfx / viewport symbol below was confirmed present. `facilities` and `train_station` live in `BaseStation` (`src/base_station_base.h:69/84`), so `st->facilities`/`st->train_station` still resolve through `Station`'s inheritance. `GfxDrawLine`/`GfxFillRect` now take `PixelColour` / `std::variant<PixelColour,PaletteID>` and the `PC_*` constants are `PixelColour`, so `DrawPOIMarkers` ports unchanged. **The one real drift is the enum-class change below (Q1.2).**

- **Station:** `st->facilities.Test(StationFacility::{Train,Airport,Dock,BusStop,TruckStop})`, `st->train_station.{w,h,tile}` + `GetCenterTile()`, `st->airport.{type,tile}` + `GetCenterTile()`, `st->docking_station`, `st->bus_station`, `st->truck_station`, `Station::Iterate()`, `INVALID_TILE`.
- **Airport types:** `AT_HELIPORT`, `AT_HELIDEPOT`, `AT_HELISTATION`.
- **Town:** `Town::{Get,Iterate}`, `GetTownIndex`, `t->cache.population`, `t->xy`, `TownID`.
- **Rail:** `IsPlainRailTile`, `GetTrackBits`, `GetRailType`, `CountBits`, `RailType`.
- **Object:** `IsObjectTypeTile(tile, OBJECT_LIGHTHOUSE)`.
- **Tile/map:** `TileXY`, `TileX/Y`, `Map::SizeX/Y`, `IsTileType(t, TileType::{House,Road})`, `GetSlopePixelZ`, `DistanceManhattan`, `TILE_SIZE`.
- **Viewport/window:** `GetMainWindow`, `w->viewport` (`ViewportData`), `vp.{scrollpos_x,scrollpos_y,dest_scrollpos_x,dest_scrollpos_y,virtual_width,virtual_height,virtual_left,virtual_top,left,top,zoom}`, `RemapCoords`, `UnScaleByZoom`, `MarkWholeScreenDirty`, `_cur_dpi` / current `DrawPixelInfo` access in `DrawPOIMarkers`, `GfxDrawLine`, `GfxFillRect`, palette colour consts (`PC_YELLOW/RED/WHITE`).
- **Wallpaper/mode (DRIFT — Q1.2):** upstream now uses scoped **`enum class SwitchMode`/`enum class GameMode`** (`src/openttd.h:18/26`) with unprefixed members. The reference's `SM_NONE` becomes **`SwitchMode::None`**; the reference `GM_WALLPAPER`/`SM_WALLPAPER` values do **not** exist yet — Stage 1 Task 7 adds them (the exact spelling, e.g. `GameMode::Wallpaper`/`SwitchMode::Wallpaper`, is owned by Stage 1 — see Q2.2). The port consumes whatever Stage 1 names. `CanRotateTitleMap()`/`RequestNextTitleMap()` come from Stage 1 `wallpaper.{cpp,h}`, which is **not yet in the tree** (Stage 1 is at Task 5/9 — see Readiness caveat below).

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

> This survey was run **file-based / non-interactively**: iteration-1 questions that could be settled from the repo tree and the `gles` reference were self-assessed and folded into the body (see Reconciliation Log). The questions below are the ones that genuinely need a human decision or that cannot resolve until Stage 1 is green — they are the confidence plateau.

### Iteration 2 — 2026-07-07

#### Q2.1. Stage 1 is at Task 5 of 9; every hard dependency this spec names (POI stub + header, `wallpaper.{cpp,h}`, `ProcessOverlayActions`, the `DrawPOIMarkers` call site, the `GameMode`/`SwitchMode` wallpaper members) is absent from the tree. When does Stage 2 unblock?
- [ ] Strictly gate: do not begin Stage 2 until Stage 1 Task 9 (on-device GameActivity gate) is green; the scanner port is a "file-body swap" only once the stub + wiring exist  *(Recommended)*
- [ ] Begin the pure game-thread C++ scanner port now against a temporary throwaway stub/header, integrating once Stage 1 lands the real wiring
- [ ] Fold the POI stub + wiring creation into Stage 1's remaining tasks so Stage 2 starts from a real (not stub) baseline
- [ ] Reframe this spec as "create `gles_poi.cpp` from scratch" (drop the swap framing) and sequence it immediately after Stage 1 Task 7

#### Q2.2. The exact wallpaper enum member names (`GameMode::Wallpaper` / `SwitchMode::Wallpaper` vs some other spelling) and the "auto mode" gate (`_switch_mode == SwitchMode::None && CanRotateTitleMap()`) depend on Stage 1 Task 7, which is unwritten. How should this spec pin them?
- [ ] Defer — Stage 2 consumes whatever identifiers Stage 1 Task 7 introduces; this spec only records the drift (`SM_NONE` → `SwitchMode::None`)  *(Recommended)*
- [ ] Pin provisional names now (`GameMode::Wallpaper`, `SwitchMode::Wallpaper`) and require Stage 1 to match them
- [ ] Add a shared "wallpaper enum contract" note to the Stage 1 spec so both stages reference one source of truth
- [ ] Leave the reference's `GM_WALLPAPER`/`SM_NONE` spelling in place and rely on a compatibility alias

#### Q2.3. If rung-1 (no mutex) shows no crash but rare visual glitches on repeated `SWITCH_MAP`, is rung-2 (`game_state_mutex`-guard `ScanMapPOIs`) implemented within Stage 2 or deferred to Stage 5 hardening?
- [ ] Keep rung-2 in Stage 2 scope as the defined fallback; ship it in Stage 2 only if the device gate demands it  *(Recommended)*
- [ ] Always implement the mutex guard in Stage 2 regardless of the gate result (pre-emptive correctness)
- [ ] Defer any mutex work to Stage 5; Stage 2 ships rung-1 unconditionally and logs the risk
- [ ] Escalate to a design review of the GL-thread/game-thread state ownership before deciding

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
