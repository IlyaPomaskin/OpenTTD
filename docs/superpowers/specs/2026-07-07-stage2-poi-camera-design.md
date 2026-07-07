# Stage 2 — POI Camera (Design Spec)

**Goal:** In wallpaper / GameActivity mode the camera visits scored map locations (stations, rail-junction clusters, towns, lighthouses) and cycles through them, replacing the Stage 1 stub's fixed map-center. Verified on-device through **GameActivity** (the live WallpaperService path is Stage 3).

**Nature:** A selective re-port. `src/video/gles_poi.cpp` is a file-body swap — the Stage 1 stub is replaced by the real scanner from branch `gles`, minus vehicle-follow. The `gles_poi.h` API is unchanged except pruning the now-dead `follow_vehicle` field. No renderer/driver/CMake changes: Stage 1 already wired the JNI natives, `ProcessOverlayActions`, the `DrawPOIMarkers` call site, and source registration.

## Context / dependencies

- **Hard dependency:** Stage 1 complete and green — `gles_poi.cpp` stub in place, GLES renderer working, `wallpaper.cpp` title-map rotation (`CanRotateTitleMap` / `RequestNextTitleMap`), `GM_WALLPAPER` boot, and the GameActivity on-device gate passing. Stage 2 cannot begin until then.
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
- **Selection (`ScanMapPOIs`):** run scanners → 20-tile edge filter → sort by score desc → build top-50 pool skipping candidates within 10 tiles of an already-selected one → Fisher-Yates pick 20 (or take all if ≤20). Reset index to 0. Keep the debug `Debug(driver, …)` breakdown lines (candidate counts, per-POI score/reason) — they are the primary verification signal.
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

## Known risk

`ProcessOverlayActions` (Stage 1, GL thread) drains JNI atomics and calls `NavigatePOI` / `PrepareBackground`, which may run `ScanMapPOIs` reading game state (`Station::Iterate()`, `Town::Iterate()`, tile reads) **without holding `game_state_mutex`** — a latent race with game-thread map mutation. In practice rescans fire almost exclusively at map load (the tile-count-change trigger), when the sim is quiescent; steady spectator sim on a title map rarely creates/removes stations. Risk assessed **low**.

**Mitigation ladder (plan carries these; ship the lowest that passes the device gate):**
1. Port as-is; verify no crash over extended run + repeated `SWITCH_MAP`/`NEXT_POI`.
2. If crashes appear: guard `ScanMapPOIs` (and the scan-triggering paths) with `game_state_mutex`.

## Files

| File | Status | Change |
|---|---|---|
| `src/video/gles_poi.cpp` | REPLACE | stub body → real scanner, no vehicle code |
| `src/video/gles_poi.h` | EDIT | drop `follow_vehicle` field + `vehicle_type.h` include |

No CMake change (registered in Stage 1). Build gate: gradle APK (`BUILD SUCCESSFUL`).

## Upstream-drift checklist (verify each symbol against current tree during port)

- **Station:** `st->facilities.Test(StationFacility::{Train,Airport,Dock,BusStop,TruckStop})`, `st->train_station.{w,h,tile}` + `GetCenterTile()`, `st->airport.{type,tile}` + `GetCenterTile()`, `st->docking_station`, `st->bus_station`, `st->truck_station`, `Station::Iterate()`, `INVALID_TILE`.
- **Airport types:** `AT_HELIPORT`, `AT_HELIDEPOT`, `AT_HELISTATION`.
- **Town:** `Town::{Get,Iterate}`, `GetTownIndex`, `t->cache.population`, `t->xy`, `TownID`.
- **Rail:** `IsPlainRailTile`, `GetTrackBits`, `GetRailType`, `CountBits`, `RailType`.
- **Object:** `IsObjectTypeTile(tile, OBJECT_LIGHTHOUSE)`.
- **Tile/map:** `TileXY`, `TileX/Y`, `Map::SizeX/Y`, `IsTileType(t, TileType::{House,Road})`, `GetSlopePixelZ`, `DistanceManhattan`, `TILE_SIZE`.
- **Viewport/window:** `GetMainWindow`, `w->viewport` (`ViewportData`), `vp.{scrollpos_x,scrollpos_y,dest_scrollpos_x,dest_scrollpos_y,virtual_width,virtual_height,virtual_left,virtual_top,left,top,zoom}`, `RemapCoords`, `UnScaleByZoom`, `MarkWholeScreenDirty`, `_cur_dpi` / current `DrawPixelInfo` access in `DrawPOIMarkers`, `GfxDrawLine`, `GfxFillRect`, palette colour consts (`PC_YELLOW/RED/WHITE`).
- **Wallpaper/mode:** `_switch_mode`, `SM_NONE`, `CanRotateTitleMap()`, `RequestNextTitleMap()` (Stage 1 `wallpaper.{cpp,h}`).

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
