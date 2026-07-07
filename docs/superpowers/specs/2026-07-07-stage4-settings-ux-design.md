# Stage 4 — Settings & Content UX (Design Spec)

**Goal:** Finish wiring the half-built user-facing configuration in the (already-ported) Android app: brightness end-to-end, the map-rotation **interval** preference, the **zoom** preference (open decision), and `MapsActivity` title-map import/delete — including making import/delete actually take effect in a running wallpaper. Verified on-device through the live **WallpaperService** (Stage 3 must be green) with `GameActivity` as a secondary check.

**Nature:** Mostly Java completion + small additive native surface. The Java classes, layouts, strings, and `SettingsHelper` prefs ALREADY EXIST (Stage 0 ported them verbatim from `gles`) — brightness is the only setting wired end-to-end. Interval and zoom prefs exist but have **no UI element and no consumer** (confirmed identical in `gles` — they were never wired even in the reference). `MapsActivity` import/delete already work at the file level but do not propagate to the native title-file cache. Stage 4 adds: interval UI + a cadence consumer, a zoom decision, a native title-map rescan trigger, and hardening of the existing brightness path.

## Context / dependencies

- **Hard dependency:** Stages 1–3 complete and green. Stage 4 needs the Stage 1 JNI surface in `src/video/sdl2_gles_v.cpp` (`nativeSetBrightness`, `nativeRotateMap`, `nativeSwitchMap`, `nativeNavigatePOI`, `nativePrepareBackground`, `nativeSetGamePaused`, `nativeScrollCamera`, `nativeSurfaceChanged`) and the Stage 1/3 `src/wallpaper.{cpp,h}` rotation API (`BuildTitleFileList`, `RotateTitleMap`, `LoadNextTitleMap`, `CanRotateTitleMap`, `RequestNextTitleMap`), plus the Stage 3 `OpenTTDWallpaperService` running as a real live wallpaper. None of these files exist on `lwp2` yet — they arrive in Stages 1/3.
- **Stage 1 removed `nativeCycleZoom`** (plan Q1.5 / Step 2b): the `private static native void nativeCycleZoom();` declaration at `OpenTTDWallpaperService.java:31` is deleted and a 1:1 Java-native↔JNI-export assertion is enforced. There is therefore **no zoom native control** to reuse — see zoom scope + open decision (a).
- **Reference (READ-ONLY):** `git show gles:android/app/src/main/java/org/openttd/android/{WallpaperSettingsActivity,MapsActivity,SettingsHelper,OpenTTDWallpaperService,GameActivity,MainActivity}.java`, `git show gles:android/app/src/main/res/layout/activity_wallpaper_settings.xml`, `git show gles:src/wallpaper.cpp`, `git show gles:src/video/sdl2_gles_v.cpp`.
- **Spec authority:** `docs/wallpaper/android-app.md` (Components table, "Wallpaper service flow", "Control surface", "Notes for reimpl") and `docs/wallpaper/wallpaper-mode.md` ("Android control surface", "Loading").
- **Reimpl principles (binding, from `overview.md` + `engine-changes.md`):** no dead scaffolding; additive edits in new/owned files where possible; `#ifdef WALLPAPER_BUILD` / Android-only guards on any upstream-file touch; do not delete upstream code; commit per logical unit. Java-only iterations build with `-PskipNativeBuild`.

## Current state (verified against the ported tree)

| Setting | Pref (SettingsHelper) | UI element | Consumer | Status |
|---|---|---|---|---|
| Brightness | `KEY_BRIGHTNESS`, default 100 | `seekbar_brightness` (`activity_wallpaper_settings.xml`) | `nativeSetBrightness` → `GLESBackend::SetBrightness` | **wired** (with warts, below) |
| Map interval | `KEY_MAP_INTERVAL`, default 2 | **none** (strings exist: `interval_every_switch`/`_10m`/`_30m`/`_2h`/`_24h`) | **none** (read into a local var in `SETTINGS_CHANGED` receiver, only `Log.i`) | **unwired** |
| Map zoom | `KEY_MAP_ZOOM`, default 1, `ZOOM_VALUES={1,2,4}` | **none** (strings `zoom_1x`/`_2x`/`_4x`) | **none** (logged only) | **unwired** |
| Title maps | files in `filesDir/title/*.sav` | `MapsActivity` (list + add + delete) | native `BuildTitleFileList` (static cache) | **file ops work; no live propagation** |

Key facts driving the design:
- **Brightness path** (`SettingsHelper.setBrightness` → `notifySettingsChanged` broadcast → service `mSettingsChangedReceiver` → `pushBrightness(int)` → `nativeSetBrightness(value/100f)`). The **500 ms delayed push exists in both** consumers: `OpenTTDWallpaperService.onSurfaceCreated` (`postDelayed(() -> pushBrightness(), 500)`) and `GameActivity.pushBrightnessDelayed()`. `pushBrightness` is guarded by `sLibrariesLoaded && sSDLInitialized`; the JNI is guarded by `if (GLESBackend::Get() != nullptr)`. Brightness is also re-pushed on `onVisibilityChanged(true)`.
- **Interval / zoom** are broadcast as intent extras by `SettingsHelper.notifySettingsChanged` but the service receiver (`OpenTTDWallpaperService.java:135-149`) only logs them.
- **Auto map rotation is currently native, POI-wrap-driven only:** `PrepareBackground` advances the POI index and, on wrap to 0 (auto mode), calls `RequestNextTitleMap()`. There is **no time-based rotation anywhere** today. `nativeSwitchMap` = `RotateTitleMap(1)`.
- **Title file list is a static cache:** `wallpaper.cpp` holds `static std::vector<...> _title_files`; `BuildTitleFileList()` clears+rescans `filesDir/title/*.sav` (+ bundled `opntitle.dat`); `LoadNextTitleMap()` rebuilds **only if the list is empty**; `RotateTitleMap`/`nativeSwitchMap` do **not** rebuild. So an import/delete does not reach a running wallpaper until the list is rebuilt.
- **`copyAssetsStatic` overwrites unconditionally:** `MainActivity.copyAssetDir` re-copies bundled `title/*.sav` from APK assets to `filesDir/title/` on every launch (no existence check). Deleting an **imported** map persists; deleting a **bundled** map reappears on next process launch.
- **Process boundary:** `WallpaperSettingsActivity` + `MapsActivity` run in the app's default process; `OpenTTDWallpaperService` in `:wallpaper`; `GameActivity` in `:game` (per `AndroidManifest.xml`). Native is loaded only in `:wallpaper`/`:game`, so settings/maps UIs **cannot call native directly** — they cross the boundary via `SharedPreferences` (persisted, shared file) + broadcasts (live signal). `filesDir/title/` is shared across all processes in the app sandbox.

## Scope

### 1. Brightness — harden the existing end-to-end path

- Keep the wired path as-is. Fixes:
  - **SeekBar range wart:** `seekbar_brightness android:max="99"` but `DEFAULT_BRIGHTNESS=100` and `pushBrightness` divides by 100 → the slider caps at 0.99 brightness and clamps the default down to 99 on display. Set `max="100"` (or scale by the seekbar max) so 100% → 1.0 and the default renders correctly.
  - **Single-shot delayed push is not a real retry:** `GameActivity.pushBrightnessDelayed`'s comment says "Retry until it accepts" but it fires **once** at 500 ms; if `GLESBackend` is still null, brightness silently stays at default until the next visibility/settings change. Replace with a bounded retry (e.g. re-post every 250 ms up to ~2 s until `GLESBackend::Get()` is non-null — requires either a native "is-ready" query or an idempotent re-push). Apply the same bounded retry in the service `onSurfaceCreated` push. Keep it minimal; brightness re-push on `onVisibilityChanged(true)` already covers the steady state.
- No new native. No CMake change.

### 2. Map-rotation interval — add UI + a cadence consumer

- **UI (`WallpaperSettingsActivity` + `activity_wallpaper_settings.xml`):** add a labelled selector (Spinner or click-to-dialog row, styled like the existing `row_title_maps`) using `map_interval_title` + the five `interval_*` strings, bound to `SettingsHelper.get/setMapUpdateInterval`. Index semantics fixed by the existing strings: `0 = every home-screen switch`, `1 = 10 min`, `2 = 30 min` (current default), `3 = 2 h`, `4 = 24 h`. Refresh selection in `refreshAll()`.
- **Consumer (in `:wallpaper`):** the `SETTINGS_CHANGED` receiver must (re)configure rotation cadence from the `interval` extra instead of just logging it.
  - `index 0` ("every home-screen switch"): rotate the title map on each `onVisibilityChanged(true)` (home-screen resume) rather than only after the POI list wraps. Implement by calling `nativeRotateMap(1)` (or a dedicated advance) on resume when interval==0; suppress the native POI-wrap auto-rotation in this mode, or accept both (see open decision b).
  - `index 1–4`: a periodic timer at 10 m / 30 m / 2 h / 24 h that triggers a map rotation. **Location is an open decision (b)** — default recommendation: a Java `Handler` in the service, armed on `onVisibilityChanged(true)` / `SETTINGS_CHANGED` and cancelled on `onVisibilityChanged(false)` (don't rotate while hidden; re-arm on next resume). This keeps rotation off-screen-cheap and needs no new native.
- The existing native POI-wrap `RequestNextTitleMap()` behavior stays for the "cinematic" default; the interval preference layers on top of / overrides it per the chosen index semantics.

### 3. Map zoom — resolve and wire OR drop (open decision a)

- The zoom pref (`KEY_MAP_ZOOM`, `ZOOM_VALUES={1,2,4}`, `zoom_*` strings) has no UI and no consumer, and its only ex-consumer (`nativeCycleZoom`) was removed in Stage 1. POI zoom is chosen per-POI natively (`In4x`/`In2x`, cluster-driven).
- **Recommendation: drop the pref** (delete `KEY_MAP_ZOOM`/`DEFAULT_MAP_ZOOM`/`ZOOM_VALUES`/getters/setters/`zoom_*` strings/`map_zoom_title`, and the `zoom` extra in `notifySettingsChanged` + the service receiver) — a global zoom override fights the POI scanner's own zoom-out decisions and there is no native control for it, so wiring it is net-new scope for little value. This also satisfies "no dead scaffolding."
- **If kept instead:** add a new `nativeSetZoom(int)` JNI + a global zoom bias applied in the POI camera (`ShowCurrentPOI`/`RecenterOnCurrentPOI`), plus a UI selector mirroring the interval selector. This is a genuine new native feature, not a rewire — surfaced as decision (a).

### 4. MapsActivity — propagate import/delete to a running wallpaper + hardening

- **Import/delete file ops already work** (SAF `GetContent` picker → copy stream to `filesDir/title/<DISPLAY_NAME>`; `AlertDialog` confirm delete; `canDelete = mapFiles.size() > 1` guard). Keep them.
- **Live propagation (new, required by the verification gate):** add a `TITLE_MAPS_CHANGED` broadcast that `MapsActivity` sends after a successful import or delete. The `:wallpaper` service registers a receiver that calls a **new native `nativeRefreshTitleMaps()`** → rebuilds `_title_files` via `BuildTitleFileList()` (clamp/reset `_title_file_idx`). This is the only way a change made in the default-process `MapsActivity` reaches the native static cache in `:wallpaper` without a process restart. Add the matching JNI export in `sdl2_gles_v.cpp` (drain via the existing atomics/`ProcessOverlayActions` pattern to stay on the GL/game thread, consistent with the other controls) and keep the 1:1 Java-native↔JNI-export invariant.
- **Bundled-vs-imported delete wart:** because `copyAssetsStatic` re-copies bundled maps every launch, deleting a bundled map does not persist. Pick one (decision d-adjacent, recommend the first): (i) make asset copy **copy-once** (skip if the dest file already exists, or gate on a `assets_copied` marker pref) so deletes stick for everything; or (ii) treat bundled maps as permanent defaults and only allow deleting imported files (hide/disable delete on names that match a bundled asset). Whichever is chosen, the "can't delete the last map" guard stays (rotation always has bundled `opntitle.dat` as a floor regardless).
- **Import validation (light hardening):** enforce/append `.sav` (the list filter already hides non-`.sav`, so a mis-picked file silently vanishes today); handle name collisions (overwrite is acceptable but should refresh + not duplicate). Deeper management (rename/reorder/preview) is out of scope — decision (c).

### 5. SETTINGS_CHANGED propagation & persistence (cross-process)

- `SettingsHelper.notifySettingsChanged` stays the single write+broadcast entry point (persist via `SharedPreferences.apply()`, then `sendBroadcast` with the extras). After the zoom decision it carries brightness + interval (+ zoom only if kept).
- The `:wallpaper` `SETTINGS_CHANGED` receiver applies **all** live-relevant settings (brightness push + interval re-arm), not just brightness. On process (re)start the service reads persisted values fresh (`SettingsHelper.getX`) — do this in `onCreate`/first `onSurfaceCreated` so a wallpaper started cold honors saved prefs without needing a broadcast.
- Do **not** rely on cross-process in-memory `SharedPreferences` consistency (`MODE_MULTI_PROCESS` is deprecated/unreliable): the broadcast is the live signal; the persisted file is the source of truth read on start.

## Known risk

- **`nativeRefreshTitleMaps` touches game/loader state:** rebuilding `_title_files` is a filesystem scan (cheap, no map mutation) but it must run on the correct thread and not race an in-flight `LoadNextTitleMap`. **Mitigation:** route it through the existing GL-thread atomic/`ProcessOverlayActions` drain like the other controls (do not call `BuildTitleFileList()` directly from the JNI thread). Deleting the currently-loaded map is safe (Android/Linux keeps the open inode; next rotation skips the now-absent path).
- **Interval timer vs POI-wrap rotation double-fire:** if both the native POI-wrap auto-rotation and the new interval timer are active, maps could rotate twice. Resolve per decision (b) — pick one authority for cadence, or explicitly define that interval==0 defers to native and 1–4 suppress native wrap-rotation.
- **Delete not persisting for bundled maps** (copyAssetsStatic overwrite) — addressed in scope §4; call out in verification so the tester deletes an *imported* map (or a bundled one after the copy-once fix).

## Files

| File | Status | Change |
|---|---|---|
| `android/app/src/main/java/.../WallpaperSettingsActivity.java` | EDIT | add interval selector wiring (+ zoom selector iff kept); refresh in `refreshAll()` |
| `android/app/src/main/res/layout/activity_wallpaper_settings.xml` | EDIT | add interval row/spinner (+ zoom iff kept); `seekbar_brightness` `max=100` |
| `android/app/src/main/java/.../OpenTTDWallpaperService.java` | EDIT | apply interval in `SETTINGS_CHANGED`; interval timer (Handler) arm/cancel on visibility; `TITLE_MAPS_CHANGED` receiver → `nativeRefreshTitleMaps`; bounded brightness retry; cold-start prefs read |
| `android/app/src/main/java/.../GameActivity.java` | EDIT | bounded brightness retry (replace single-shot) |
| `android/app/src/main/java/.../MapsActivity.java` | EDIT | send `TITLE_MAPS_CHANGED` after import/delete; `.sav` enforcement; (bundled-delete handling per §4) |
| `android/app/src/main/java/.../MainActivity.java` | EDIT (iff copy-once chosen) | make `copyAssetsStatic` skip existing title files |
| `android/app/src/main/java/.../SettingsHelper.java` | EDIT | drop zoom pref (recommended) OR keep; interval unchanged |
| `android/app/src/main/res/values/strings.xml` | EDIT | remove `zoom_*`/`map_zoom_title` iff zoom dropped; add any new labels |
| `src/video/sdl2_gles_v.cpp` | EDIT (native) | add `nativeRefreshTitleMaps` JNI (+ `nativeSetZoom` iff zoom kept); keep 1:1 export invariant |
| `src/wallpaper.{cpp,h}` | EDIT (native) | expose a rescan entry (`BuildTitleFileList` re-run) drained via `ProcessOverlayActions`; (global zoom bias iff zoom kept) |

Build gate: `JAVA_HOME=.../temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug` → `BUILD SUCCESSFUL`. Java-only iterations add `-PskipNativeBuild`.

## Verification gate (physical arm64 device, live WallpaperService primary)

1. Build + install APK; set the live wallpaper (`WallpaperSettingsActivity` → Set Wallpaper). No crash, no `UnsatisfiedLinkError`.
2. **Brightness:** drag the slider 100→0 → wallpaper dims live; 0 ≈ black, 100 = full (verify the `max=100` fix — full is not capped at 0.99). Kill+relaunch the wallpaper (`onSurfaceCreated` push) → brightness restored from prefs without touching the slider.
3. **Interval:** set "Every home screen switch" → each home→app→home cycle rotates the map (logcat `BuildTitleFileList`/rotate + visibly new map). Set a short interval (test with the smallest, 10 min, or a temporary debug value) → map auto-rotates on that cadence while visible and not while hidden; changing the interval re-arms observably.
4. **Import:** `MapsActivity` → Add → pick a `.sav` → it lists; on the running wallpaper it enters rotation (`TITLE_MAPS_CHANGED` → `nativeRefreshTitleMaps` → logcat shows the new file in the `BuildTitleFileList` dump; rotate reaches it).
5. **Delete:** delete an *imported* map → removed from list and from rotation after refresh; (if copy-once chosen) delete persists across process restart.
6. **Persistence:** all prefs survive killing the `:wallpaper` process (force-stop / re-set wallpaper) — values read back from `SharedPreferences`.
7. **Zoom:** if dropped — confirm no zoom UI and POI zoom still behaves (In4x/In2x). If kept — selector changes the rendered zoom level live.
8. **GameActivity secondary:** brightness delayed-retry still applies on launch (`-PskipNativeBuild` fine for Java-only UI iterations).

## Out of scope (later / other stages)

- POI scoring/zoom tuning and rotation-cadence perf instrumentation (Stage 5).
- Rich map management: rename, reorder, thumbnail/preview, per-map enable toggles (decision c — minimal SAF import+delete this stage).
- Any settings for the debug `GameActivity` overlay controls (dev-only).
- WallpaperService lifecycle itself (Stage 3).

## Open decisions

- **(a) Zoom preference — drop vs new native control.** *Recommend: drop.* Zoom is POI-driven natively (In4x/In2x) and `nativeCycleZoom` was removed in Stage 1; a global override has no native hook and conflicts with the scanner's cluster zoom-out. Keeping it means net-new native (`nativeSetZoom` + a global zoom bias in the POI camera) for marginal value. Tradeoff: dropping removes a user knob some may expect; adding it is real feature work, not a rewire.
- **(b) Interval timer location — Java `Handler` in the service vs native timer.** *Recommend: Java `Handler` in `:wallpaper`.* Simplest, visibility-aware (arm on resume, cancel on hide), no new native, and process-death just re-arms on next resume. Tradeoff: not wall-clock-accurate across screen-off gaps and dies with the process (acceptable for a wallpaper). A native tick-based timer would be more self-contained and survive within the process but adds native surface and must still respect pause/visibility — and still needs to reconcile with the existing POI-wrap auto-rotation (define which authority owns cadence for each interval index).
- **(c) MapsActivity import UX depth — minimal SAF picker + delete vs richer management.** *Recommend: minimal* (current import/delete + `.sav` enforcement + live refresh). Tradeoff: minimal is fast and matches the "backgrounds" framing in `maps_note`; richer (rename/reorder/preview/enable-toggle) is a larger UI effort better deferred.
- **(d) Bundled-map delete semantics — copy-once vs permanent-defaults.** *Recommend: copy-once* (skip asset copy when the dest file already exists / gate on a marker pref) so every delete persists uniformly and the code path is simple. Tradeoff: copy-once means a corrupted/edited bundled file won't self-heal on relaunch; the alternative (treat bundled as undeletable defaults) preserves self-heal but needs bundled-vs-imported detection in the UI and confuses "delete" semantics.
- **(e) How much ported wiring is reusable as-is.** `SettingsHelper` (prefs + broadcast) and `MapsActivity` file ops are reusable with only additive changes; `WallpaperSettingsActivity` needs the interval UI added; the service `SETTINGS_CHANGED` receiver needs its interval branch implemented (currently log-only) and a new `TITLE_MAPS_CHANGED` receiver. No rework of the existing brightness path beyond the two warts (SeekBar max, one-shot retry).
