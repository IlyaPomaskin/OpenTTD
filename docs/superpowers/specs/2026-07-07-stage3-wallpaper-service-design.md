# Stage 3 — Live Wallpaper Service (Design Spec)

**Goal:** Make OpenTTD render as an actual Android live wallpaper on the home screen — set via `WallpaperSettingsActivity`, hosted by `OpenTTDWallpaperService` in the `:wallpaper` process, driven through the SDL service-mode glue. Stage 1 proved only the GameActivity debug path (`:game`); this stage proves the real service path and its lifecycle (preview↔live, rotation, screen-off/on, brightness).

**Nature:** Primarily **on-device wire-up + lifecycle verification + targeted bugfixing** — NOT greenfield authoring. The Java `WallpaperService`, its `OpenTTDEngine`, and the vendored SDL service-mode fork were ported verbatim in Stage 0 (from a working `gles` reference); Stage 1 exported the service JNI natives via `src/video/sdl2_gles_v.cpp`. Stage 3 connects those already-present pieces on a physical device and fixes bugs only where the **reimpl'd C++** (new `sdl-gles` driver, surface recovery, pause CV) meets the **ported Java** (surface injection, visibility, brightness). It also folds in the asset-packaging build fix so a single clean build is asset-complete.

## Context / dependencies

- **Hard dependency:** Stages 1 and 2 complete and green.
  - Stage 1 must have landed `src/video/sdl2_gles_v.cpp` with **all** JNI natives (both `GameActivity` and `OpenTTDWallpaperService`), `RecoverContextIfLost()` (manual `eglCreateWindowSurface` over `SDLActivity.getNativeSurface()`), `_gles_surface_changed` handling, `SetGameThreadPaused()` + game-pause CV, `ProcessOverlayActions()`, `SetBrightness()` blit-shader uniform, and the `GM_WALLPAPER`/`SM_WALLPAPER` boot in `wallpaper.cpp`. On branch `lwp2` at spec-write time Stage 1 is only through Task 4 (`sdl2_gles_v.cpp` does not yet exist) — Stage 3 cannot begin until Stage 1 Task 8/9 are done.
  - Stage 2 must have landed the real POI scanner (`gles_poi.cpp`) so that `nativePrepareBackground()` (jump-to-next-POI on hide) and resume-at-fresh-POI are observable.
- **Reference (READ-ONLY):** `git show gles:<path>` / `git diff e24f92ce82..gles -- <files>`. Never modify `gles`.
- **Spec authorities:** `docs/wallpaper/android-app.md` ("Wallpaper service flow", "Manifest / processes", "Notes for reimpl"), `docs/wallpaper/sdl-android-changes.md` (whole file), `docs/wallpaper/rendering.md` ("Context loss / surface recovery"), `docs/wallpaper/wallpaper-mode.md` ("Android control surface", "Lifecycle summary").
- **Reimpl principles (binding, `overview.md` + `engine-changes.md`):** no dead scaffolding; hooks over rewrites; one-hunk `#ifdef WALLPAPER_BUILD` guards; additive interface edits, gate-don't-delete; instrumentation gated behind `WALLPAPER_PERF`; wallpaper-only + Android-only, emulator not a target (no gfxstream workarounds).
- **Build gate:** `JAVA_HOME=…/temurin-25.jdk/… ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug` → `BUILD SUCCESSFUL`. Target: physical arm64-v8a (Pixel 10 Pro). Python: `/usr/bin/python3`. Device control: `adb shell am broadcast -a org.openttd.android.{JUMP_POI,SWITCH_MAP,NEXT_POI,PREV_POI,NEXT_MAP,PREV_MAP,SCROLL_CAMERA,SETTINGS_CHANGED}` and `/usr/bin/python3 tools/run_android.py all`.

## What already exists (verified on `lwp2`) — do not re-author

These are ported and correct as-is; Stage 3 verifies them and only edits on a proven bug.

- **Two-process model** — `AndroidManifest.xml`: `OpenTTDWallpaperService android:process=":wallpaper"` with `android:permission="android.permission.BIND_WALLPAPER"` and the `android.service.wallpaper.WallpaperService` intent-filter + `@xml/wallpaper` meta-data; `GameActivity android:process=":game"`. Each process owns its own SDL/native state; brightness passed via intent extras, never statics across processes.
- **Set-live-wallpaper flow** — `WallpaperSettingsActivity` `btn_set_wallpaper` → `Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER)` + `EXTRA_LIVE_WALLPAPER_COMPONENT = ComponentName(this, OpenTTDWallpaperService.class)`. `res/xml/wallpaper.xml` sets `settingsActivity="org.openttd.android.WallpaperSettingsActivity"`.
- **`OpenTTDEngine` lifecycle** (`OpenTTDWallpaperService.java`): `onCreate` (load `LIBRARIES` once per process, guarded by `sLibrariesLoaded`), `onSurfaceCreated` (first-time: `copyAssetsStatic` + `setenv OPENTTD_DATA_PATH` + `SDLActivity.initForService()` + set `sOverrideLibrary/Function/Arguments`, guarded by `sSDLInitialized`; then surface injection), `onSurfaceChanged`, `onSurfaceDestroyed`, `onVisibilityChanged`.
- **Surface injection** — `sOverrideSurface = holder.getSurface()` → `SDLActivity.onNativeSurfaceCreated()` → `nativeSurfaceChanged()` (tells GL thread to rebind EGL). Previous surface destroyed first (`onNativeSurfaceDestroyed()`) when `sOverrideSurface != null`.
- **Stale-engine handling** — `onSurfaceDestroyed` pauses (nulls `sOverrideSurface`, `onNativeSurfaceDestroyed`, force `PAUSED`) **only if `sOverrideSurface == mEngineSurface`** (this engine owns the active surface); stale engines skip pause to avoid blocking the new surface.
- **Visibility** — hide: `nativePrepareBackground()` (jump next POI **while still visible**) then a 150 ms `Handler.postDelayed` → `nativeSetGamePaused(true)` (only if still hidden). Show: re-inject surface if `sOverrideSurface == null` (recompute resolution from `getSurfaceFrame()`), `nativeSetGamePaused(false)`, `pushBrightness()`, `RESUMED`.
- **Brightness push with 500 ms retry** — `onSurfaceCreated` posts a 500 ms delayed `pushBrightness()` (GLESBackend not ready until the SDL thread has rendered); `pushBrightness` early-returns unless `sLibrariesLoaded && sSDLInitialized`; `SETTINGS_CHANGED` receiver also pushes.
- **Control surface** — 7 broadcast receivers (`JUMP_POI/SWITCH_MAP/NEXT_POI/PREV_POI/NEXT_MAP/PREV_MAP/SCROLL_CAMERA`) + `SETTINGS_CHANGED`, registered `RECEIVER_EXPORTED` in service `onCreate`, all funnel to native atomics via the JNI natives.
- **SDL service-mode fork** (`org/libsdl/app/SDLActivity.java`, verified line refs): statics `sOverrideSurface`(208) `sServiceMode`(209) `mServiceContext`(210) `sOverrideLibrary/Function/Arguments`(211–213); `initForService(Context)`(333); `initialize()` resets the service statics(319–326); `getNativeSurface()` returns `sOverrideSurface` first(1436–1443) — the core hook SDL's native EGL creation uses; `handleNativeState()` RESUMED condition in service mode = `sOverrideSurface != null`(739–740); `SDLMain.run()` service branch uses `sOverride*`(1935) and on exit only nulls `mSDLThread`, no `finish()`(1958). `SDLSurface.surfaceChanged()` early-returns when `sServiceMode`(111).

## Scope

### 1. On-device wire-up + lifecycle verification (the bulk of the stage)

Drive the full lifecycle on the physical device via `WallpaperSettingsActivity` and the broadcast control surface, confirming each transition against the ported Java + reimpl C++. This is the primary deliverable; expect zero or few code edits if Stages 1–2 are truly green.

- **Set-live flow:** launcher → tap "Set wallpaper" → system live-wallpaper preview → confirm → home screen. Verify the `:wallpaper` process starts, libs load once, `SDL_main` runs once (`sSDLInitialized`), map renders and animates on the actual home screen (behind icons).
- **Preview↔live overlap:** the system creates overlapping engines during preview→home. Verify the stale-engine guard (`sOverrideSurface == mEngineSurface`) prevents the outgoing engine from pausing/destroying the incoming engine's surface, and that exactly one engine ends up driving.
- **Rotation → `SM_WALLPAPER` full reload:** portrait↔landscape triggers `onSurfaceChanged` (new dimensions) → `nativeSetScreenResolution` + `onNativeResize` + `onNativeSurfaceChanged` + `nativeSurfaceChanged()`; the engine-side dimension-change path forces `_switch_mode = SM_WALLPAPER` (map reload, fixes stale-snapshot-at-old-size). Verify no stretched/torn frame persists.
- **Visibility pause/resume (≈0 CPU when hidden):** screen-off / launcher-open → hide → `nativePrepareBackground()` renders the next POI while still visible, then +150 ms → `nativeSetGamePaused(true)` parks the game thread on the pause CV (GL thread idle-skips swaps) → CPU ≈ 0. Screen-on → show → re-inject surface if lost → unpause (5 warm-up ticks then resume) → brightness re-pushed → renders at the fresh POI.
- **Surface recovery:** on every surface (re)injection the GL thread must rebind EGL — `_gles_surface_changed` → `RecoverContextIfLost()` manually `eglCreateWindowSurface` over the new `ANativeWindow` from `getNativeSurface()`, destroy old, make-current, resize FBO (SDL never rebinds the wallpaper surface itself). Full context loss → `RecoverGPUState()` + `SM_WALLPAPER` reload. Verify no black screen / context-loss loop across transitions.
- **Brightness:** move the `WallpaperSettingsActivity` slider (0–100) → `SETTINGS_CHANGED` broadcast → `nativeSetBrightness(v/100)` → blit-shader `u_brightness` dims the live wallpaper. Confirm the 500 ms retry lands brightness on a freshly-created surface (before the first render completes).

### 2. Service-only JNI natives 1:1 with Java declarations (verification + assert)

Stage 1 Task 8 already exports these and removes `nativeCycleZoom` (Q1.5). Stage 3 asserts the invariant holds and fixes any drift.

- `OpenTTDWallpaperService.java` currently declares **9** `private static native` methods (`grep -c` = 9): `nativePrepareBackground, nativeCycleZoom, nativeSwitchMap, nativeNavigatePOI, nativeRotateMap, nativeScrollCamera, nativeSetGamePaused, nativeSurfaceChanged, nativeSetBrightness`.
- `nativeCycleZoom` has **no** JNI export in the `gles` reference and **no** caller — Stage 1 removes the Java declaration. After that, the service must have **8** natives, each with a matching `Java_org_openttd_android_OpenTTDWallpaperService_*` export. Reference export set (verified in `gles:src/video/sdl2_gles_v.cpp`): `nativePrepareBackground, nativeSwitchMap, nativeNavigatePOI, nativeRotateMap, nativeScrollCamera, nativeSetGamePaused, nativeSetBrightness, nativeSurfaceChanged`.
- **Acceptance:** `grep "private static native" .../OpenTTDWallpaperService.java` count == number of `Java_..._OpenTTDWallpaperService_native*` exports in `sdl2_gles_v.cpp`; no orphan in either direction. Same 1:1 check for `GameActivity` (5 natives: `nativeRotateMap, nativeNavigatePOI, nativeScrollCamera, nativeSetGamePaused, nativeSetBrightness`). Any `UnsatisfiedLinkError` in the `:wallpaper` process at boot is a 1:1 violation → fix in `sdl2_gles_v.cpp`, not by stubbing Java.

### 3. Asset-packaging build fix (fold in from Stage 1 Q1.1 — single clean build is asset-complete)

The current wart: `copy_assets` (wrapper `CMakeLists.txt:166`) copies `baseset/` + `lang/` from the CMake build dir into `${CMAKE_SOURCE_DIR}/assets` (= `android/app/src/main/assets`, gradle's default asset source dir). It `add_dependencies(copy_assets openttd)` and is passed as a CMake target in `build.gradle` (`targets 'openttd', 'copy_assets'`). But gradle's `merge*Assets`/package step is **not ordered after** the external native build, so a truly clean build packages an APK *before* the assets exist → assetless APK → launch fails with `Error extracting baseset assets` → forced second build.

- **Fix:** make asset merge/packaging depend on the native build (which runs `copy_assets`) so one clean `assembleDebug` is asset-complete. See Open decisions (c) for the mechanism tradeoff; recommendation is the AGP variant hook `mergeAssetsProvider.dependsOn(externalNativeBuild task)`.
- **Constraint:** the fix lives entirely in `android/app/build.gradle` (and/or the wrapper `CMakeLists.txt`) — no engine C++ change. It must survive `skipNativeBuild` (Java-only iterations must not error trying to depend on a non-existent native task).
- **Verify:** from a clean tree (`rm -rf android/app/.cxx android/app/build android/app/src/main/assets`), a single `assembleDebug` produces an APK containing `assets/baseset/*` and `assets/lang/*` (`unzip -l app-debug.apk | grep -c baseset` > 0), and the wallpaper launches without the asset-extraction error on first install.

### 4. GameActivity (debug path) — frozen

`GameActivity` (`:game`) and its 5 natives are unchanged this stage (see Open decisions (d)). It stays available as the Stage 1/2 debug viewer. Only touch it if the asset-packaging fix or a shared JNI change demonstrably breaks it.

## Known risk

- **Overlapping-engine surface races (medium).** Preview→home creates two live `OpenTTDEngine` instances briefly sharing the single-process SDL/GL state. The stale-engine guard is the only thing preventing the outgoing engine from tearing down the incoming surface. If transitions flicker/black-out, the fix budget is in the Java engine (guard timing / re-injection in `onVisibilityChanged`) and/or the C++ `RecoverContextIfLost()` surface-swap ordering — **not** a redesign. Mitigation ladder: (1) verify as-ported; (2) add logging around `sOverrideSurface` ownership transitions; (3) tighten the guard or the C++ EGL rebind sequence.
- **Pause/resume warm-up correctness (low–medium).** Resume runs 5 warm-up ticks before unblocking (warms the atlas at the new POI). If resume shows a black/stale frame or a CPU spike, inspect the ordering of surface re-injection vs `nativeSetGamePaused(false)` vs `RESUMED` in `onVisibilityChanged`.
- **Brightness timing (low).** The 500 ms retry is a race hack against GLESBackend readiness. If brightness intermittently fails to apply on a fresh surface, the retry cadence (or gating on a "backend ready" flag) is the lever.
- **Two-process state bleed (low).** Confirm nothing shares native statics across `:wallpaper` and `:game`; brightness/args go through intent extras only. A single shared static would corrupt one process's SDL state.

## Files

| File | Status | Change |
|---|---|---|
| `android/app/build.gradle` | EDIT | asset-packaging fix: order asset merge after the native build (`copy_assets`); guard for `skipNativeBuild`. See Open decisions (c). |
| `android/app/src/main/CMakeLists.txt` | EDIT (maybe) | only if the asset fix is done CMake-side instead of gradle-side (alt to build.gradle edit). |
| `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` | VERIFY / bugfix-only | lifecycle already ported; edit only on a proven transition bug. |
| `android/app/src/main/java/org/libsdl/app/SDLActivity.java`, `SDLSurface.java` | VERIFY / bugfix-only | service-mode hooks already present; edit only on a proven bug (or the fork→patch extraction — Open decision (a)). |
| `src/video/sdl2_gles_v.cpp` | VERIFY / bugfix-only | service JNI natives + surface recovery + pause CV (landed Stage 1); assert 1:1, fix interaction bugs only. |
| `docs/wallpaper/android-app.md`, this spec/plan | EDIT (Task-9-style) | log deviations + Stage 3 result at the end. |

**No greenfield Java or C++ authoring is planned.** New code appears only if a verification step exposes a bug that cannot be fixed by ordering/guarding existing code.

## Verification gate (physical arm64 device — Pixel 10 Pro)

Use `/usr/bin/python3 tools/run_android.py all` for the build/install/activate/log cycle; supplement with targeted `adb` where noted.

1. **Single clean build is asset-complete:** from a clean tree, one `assembleDebug`; APK contains `assets/baseset/*`+`assets/lang/*`; no second build required; install + first launch shows no `Error extracting baseset assets`.
2. **Set as live wallpaper:** `WallpaperSettingsActivity` → "Set wallpaper" → confirm → home screen renders an isometric map and **animates** (water/vehicles) behind the launcher; logcat shows `:wallpaper` process, libs loaded once, no `UnsatisfiedLinkError`, no `FATAL`.
3. **Preview↔live transition:** enter the system wallpaper picker preview, then apply; verify a single engine ends up rendering, no persistent black frame, logcat shows the stale-engine "skipping pause" path and a clean surface handover.
4. **Screen rotation:** rotate device; wallpaper reloads (`SM_WALLPAPER`), re-fits new dimensions, no stretched/torn frame persists.
5. **Screen-off/on:** turn screen off → within ~150 ms the game thread parks (logcat `game_thread_sleep: entering pause wait`), CPU ≈ 0 (confirm via `top`/simpleperf idle); turn on → resumes (`resumed`), renders at a **fresh POI** (moved vs pre-hide frame).
6. **Brightness:** move the slider to ~30% → live wallpaper visibly dims; to 100% → full; confirm it applies on a just-created surface (kill+relaunch service, set brightness immediately, verify the 500 ms retry lands).
7. **Control surface:** `JUMP_POI` moves camera, `SWITCH_MAP` reloads a new map + fresh POI scan, `NEXT_POI`/`PREV_POI`/`NEXT_MAP`/`PREV_MAP`/`SCROLL_CAMERA` all act — no crash.
8. **Endurance:** leave the wallpaper live through several sleep/wake + rotation + preview cycles; no accumulating context-loss loop, leak, or crash in logcat.

## Out of scope (later stages)

- Settings UX beyond brightness (map-interval / zoom prefs exist in `SettingsHelper` but are hidden/disabled) — **Stage 4**.
- Perf/battery hardening, `WALLPAPER_PERF`-off shipping build, pause-CPU tuning, atlas memory-pressure under long runs — **Stage 5**.
- Any change to the POI scoring or renderer internals (Stages 2/1 territory).
- GameActivity feature work (frozen — Open decision (d)).

## Open decisions (recommend + tradeoffs; do NOT decide here)

**(a) Keep the ~5700-line SDL Java fork as-is, or extract a ~60-line patch.**
- *Keep the fork* — zero risk now, everything works, changes are all greppable (`sOverride|sServiceMode|initForService|mServiceContext`). Cost: carrying a huge vendored file that obscures the ~60 real edits and complicates future SDL upgrades.
- *Extract a clean patch / subclass* — small, auditable, upstream-mergeable; matches the reimpl "no dead scaffolding" ethos. Cost: real re-engineering + re-verification of the exact path Stage 3 is trying to prove.
- **Recommendation:** keep the fork for Stage 3 (do not perturb the thing under test); schedule the patch extraction as a separate, independently-verified cleanup after the Stage 3 gate is green. It is not on this stage's critical path.

**(b) Scope boundary — pure verification vs. how much bugfix budget for reimpl-C++ ↔ ported-Java interaction.**
- *Pure verification* — assume Stages 1–2 green; only assert 1:1 natives + run the gate. Risk: real transition bugs (overlapping engines, surface rebind, warm-up ordering) surface exactly at this integration point and would block the gate with no budget.
- *Verification + bounded bugfix* — allow edits confined to (i) the Java engine lifecycle ordering/guards and (ii) the C++ surface-recovery / pause sequencing in `sdl2_gles_v.cpp`, following the Known-risk mitigation ladders.
- **Recommendation:** verification + bounded bugfix, capped at those two surfaces; anything requiring renderer/POI redesign bounces back to Stage 1/2. Bug-first, minimal-diff, gate-driven.

**(c) Asset-packaging fix mechanism.**
- *Gradle variant hook* — `applicationVariants.all { mergeAssetsProvider.configure { dependsOn(<externalNativeBuild task>) } }` (or `tasks.named("merge${Variant}Assets")`). Directly expresses "assets after native build". Cost: AGP task-name/provider API is version-sensitive and must no-op under `skipNativeBuild`.
- *CMake-side* — have `copy_assets` write somewhere gradle already treats as a generated-asset input, or wire the copy as a build step gradle consumes. Cost: fights AGP's asset-merge model; brittle.
- *Unconditional build-twice* — drop the fix, always build twice. Cost: violates the stage goal (single clean build must be asset-complete); rejected.
- **Recommendation:** the gradle variant hook (`mergeAssetsProvider.dependsOn`), wrapped so it is skipped when `project.hasProperty('skipNativeBuild')`. Keep it a few lines in `build.gradle`; verify against the exact AGP version in use.

**(d) Does GameActivity (debug) need parallel changes, or is it frozen for this stage.**
- *Frozen* — leave `:game` and its 5 natives untouched; it remains the Stage 1/2 debug viewer.
- *Parallel changes* — mirror any brightness-retry / surface-handling refinement into `GameActivity` for consistency.
- **Recommendation:** frozen. GameActivity already passed Stage 1/2 on its own path; touch it only if the asset fix or a shared JNI change breaks it. Consistency refactors belong to a later cleanup, not the service-enablement stage.
