# Wallpaper Mode (GM_WALLPAPER)

Spectator-only mode; full simulation runs (tile loop, vehicles, landscape ticks all kept — needed for water anim, tree growth, vehicle movement). No UI except main viewport.

## Loading (src/wallpaper.cpp)

- `BuildTitleFileList()`: `opntitle.dat` (baseset) + `title/*.sav` from `OPENTTD_DATA_PATH` and search paths.
- `RotateTitleMap(delta)` → sets `SM_WALLPAPER`/`SM_MENU`; `LoadNextTitleMap()` tries files round-robin until one loads.
- `LoadWallpaperGame()`: set GM_WALLPAPER → InvalidatePOIs → request atlas clear → ResetWindowSystem → load save (fallback: empty 64x64 world) → COMPANY_SPECTATOR, unpause → `FixTitleGameZoom(-1)` → `PrepareBackground()` (POI camera).

## POI camera (src/video/gles_poi.cpp)

- Scan on map change (`_gles_poi_map_tiles` guard):
  - **Stations**: train +5, airport +3 (real airports only), dock +2 (if interesting), bus +1, truck +1 (if ≥2 facilities); nearby town population bonus; station-cluster +3. Influence points collected for zoom-out decisions.
  - **Rail junction clusters**: score 5–8 by junction count.
  - **Towns**: min(5, pop/500).
  - Lighthouses; vehicle-follow POIs (score 50, follow camera, currently follow disabled — recenters only).
- Keep 20 random from top-50 by score, cycle in order. Each POI: fractional map pos, zoom (In4x/In2x), delay_ms.
- `PrepareBackground()` = advance to next POI + scroll there; `NavigatePOI(±1)` manual browse; `RecenterOnCurrentPOI()` after resize; `DrawPOIMarkers` debug overlay in menu/wallpaper.

## Android control surface (JNI, sdl2_gles_v.cpp)

All JNI callbacks write atomics; consumed on GL thread in `ProcessOverlayActions()` (thread safety):

| JNI (WallpaperService + GameActivity) | Effect |
|---|---|
| nativePrepareBackground | `_gles_jump_waypoint` → next POI on hide |
| nativeSwitchMap / nativeRotateMap(d) | RotateTitleMap |
| nativeNavigatePOI(d) | NavigatePOI |
| nativeScrollCamera(dx,dy) | dest_scrollpos += ScaleByZoom |
| nativeSetGamePaused(b) | game thread CV pause (5 warm-up ticks first) |
| nativeSetBrightness(f) | blit shader uniform 0..1 |
| nativeSurfaceChanged | `_gles_surface_changed` → manual EGL surface rebind |

## Lifecycle summary

- Screen off / hidden → SDL background event or nativeSetGamePaused → game thread parked (sim frozen, zero CPU); GL thread idles (idle-blit skip → no swaps).
- Show → resume + `_gles_jump_waypoint` → camera at fresh POI.
- Surface recreate (preview↔live, rotation) → surface-changed path or full context recovery + `SM_WALLPAPER` reload (see rendering.md).
