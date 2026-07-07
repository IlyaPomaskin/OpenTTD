# Stage 3 — Live Wallpaper Service (Design Spec)

**Plan-confidence status:** Draft (iteration 3, confidence 88%) — cap: Risk. The former binding blocker (Stage 1 Task 8/9 `sdl2_gles_v.cpp` + the 1:1 JNI natives) is now **RESOLVED**: Stage 1 is complete and device-verified (Pixel 10 Pro, `lwp2` @ `634dd21703`). The `sdl-gles` driver, JNI natives (13/13 1:1), `RecoverContextIfLost()`, the game-pause CV, and the mutex-guarded `ProcessOverlayActions()` all exist in-tree (citations in Context). The residual cap is the **overlapping-engine surface-handover race** — medium, device-only, and *not* exercised by Stage 1's single-engine `:game` gate — plus an offline-unverifiable AGP asset-task name. Stage 2 POI is a STUB (not a hard dependency); the service runs on it.

**Goal:** Make OpenTTD render as an actual Android live wallpaper on the home screen — set via `WallpaperSettingsActivity`, hosted by `OpenTTDWallpaperService` in the `:wallpaper` process, driven through the SDL service-mode glue. Stage 1 proved only the GameActivity debug path (`:game`); this stage proves the real service path and its lifecycle (preview↔live, rotation, screen-off/on, brightness).

**Nature:** Primarily **on-device wire-up + lifecycle verification + targeted bugfixing** — NOT greenfield authoring. The Java `WallpaperService`, its `OpenTTDEngine`, and the vendored SDL service-mode fork were ported verbatim in Stage 0 (from a working `gles` reference); Stage 1 exported the service JNI natives via `src/video/sdl2_gles_v.cpp`. Stage 3 connects those already-present pieces on a physical device and fixes bugs only where the **reimpl'd C++** (new `sdl-gles` driver, surface recovery, pause CV) meets the **ported Java** (surface injection, visibility, brightness). It also folds in the asset-packaging build fix so a single clean build is asset-complete.

## Context / dependencies

- **Entry gate — MET.** Stage 3's only *functional* hard dependency is **Stage 1 (complete, device-verified on Pixel 10 Pro, `lwp2` @ `634dd21703`)**. It does **not** depend on Stage 2: the POI scanner is still a Stage-1 STUB (`src/video/gles_poi.cpp:8`, "camera centers on map") and the service runs fine on it — so the entry gate is satisfied *now*.
  - **Verified present in-tree (Stage 1 landed):** `src/video/sdl2_gles_v.{cpp,h}` exist. Driver class `VideoDriver_SDL_GLES` (`sdl2_gles_v.h:17`, name `"sdl-gles"`). JNI natives 1:1 (13/13): 8 service exports (`sdl2_gles_v.cpp:103,109,115,121,164,171,181,189`) + 5 GameActivity exports (`:127,133,139,146,156`) matching 8 + 5 Java declarations; `nativeCycleZoom` **removed** from `OpenTTDWallpaperService.java` (Task 8 Step 2b done). `RecoverContextIfLost()` — manual `eglCreateWindowSurface` over the new `ANativeWindow` — defined `sdl2_gles_v.cpp:669`, called `:497`; full-loss path calls `RecoverGPUState()` + `SwitchMode::Wallpaper` reload `:826,828`. `_gles_surface_changed` atomic `:63`, handled in the surface-swap block `:678`. Game-pause CV: `SetGameThreadPaused()` (`video_driver.hpp:207`) drives `game_thread_paused` (`:413`) + `game_pause_cv` (`:415`); game thread waits `video_driver.cpp:49–58`; JNI entry `sdl2_gles_v.cpp:171–177` (service) / `:146–152` (game). `ProcessOverlayActions()` defined `:454`, called `:496`, **guarded by `game_state_mutex`** `:467` (Stage-1 device-gate fix for the GL-thread overlay race — blocker C). `SetBrightness()` via `GLESBackend::Get()->SetBrightness()` `:184` (service) / `:159` (game). Wallpaper boot uses scoped enums: `LoadWallpaperGame()` sets `_game_mode = GameMode::Wallpaper` (`wallpaper.cpp:134`), `_switch_mode = SwitchMode::Wallpaper` (`:94`, `openttd.cpp:548`).
  - **POI = stub (NOT a gate):** `nativePrepareBackground()` sets `_gles_jump_waypoint` (`sdl2_gles_v.cpp:105`) → stub `PrepareBackground()` recenters on map centre. Jump-on-hide + resume are *observable as mechanism* now, but "resume at a **distinct** fresh POI" only becomes true when Stage 2 lands the real scanner. Stage 3 verifies the mechanism and defers distinct-POI acceptance to Stage 2 (see Q3.3).
  - **Stage-1 device-gate deltas folded in (durable logs: `.superpowers/sdd/progress.md`, this plan's sibling `docs/superpowers/plans/2026-07-05-stage1-gles-renderer.md`, `docs/wallpaper/engine-changes.md`):** audio disabled in wallpaper mode (`InitializeSound`/`InitializeMusic` gated under `#ifndef WALLPAPER_BUILD` — blocker A); recording coords shipped as reference **pointer-math** (explicit-coords reverted — blocker B); atlas pre-allocates 4 layers (~80 MB @2048, ran fine on device) and clears via delete+realloc.
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

Drive the full lifecycle on the physical device via `WallpaperSettingsActivity` and the broadcast control surface, confirming each transition against the ported Java + reimpl C++. This is the primary deliverable; expect zero or few code edits now that Stage 1 is green (Stage 2 POI stub accepted) — bar the overlapping-engine handover (Q3.1).

- **Set-live flow:** launcher → tap "Set wallpaper" → system live-wallpaper preview → confirm → home screen. Verify the `:wallpaper` process starts, libs load once, `SDL_main` runs once (`sSDLInitialized`), map renders and animates on the actual home screen (behind icons).
- **Preview↔live overlap:** the system creates overlapping engines during preview→home. Verify the stale-engine guard (`sOverrideSurface == mEngineSurface`) prevents the outgoing engine from pausing/destroying the incoming engine's surface, and that exactly one engine ends up driving.
- **Rotation → `SwitchMode::Wallpaper` full reload:** portrait↔landscape triggers `onSurfaceChanged` (new dimensions) → `nativeSetScreenResolution` + `onNativeResize` + `onNativeSurfaceChanged` + `nativeSurfaceChanged()`; the engine-side dimension-change path forces `_switch_mode = SwitchMode::Wallpaper` (`main_gui.cpp:644`; map reload, fixes stale-snapshot-at-old-size). Verify no stretched/torn frame persists.
- **Visibility pause/resume (≈0 CPU when hidden):** screen-off / launcher-open → hide → `nativePrepareBackground()` renders the next POI while still visible, then +150 ms → `nativeSetGamePaused(true)` parks the game thread on the pause CV (GL thread idle-skips swaps) → CPU ≈ 0. Screen-on → show → re-inject surface if lost → unpause (5 warm-up ticks then resume) → brightness re-pushed → renders at the fresh POI.
- **Surface recovery:** on every surface (re)injection the GL thread must rebind EGL — `_gles_surface_changed` → `RecoverContextIfLost()` manually `eglCreateWindowSurface` over the new `ANativeWindow` from `getNativeSurface()`, destroy old, make-current, resize FBO (SDL never rebinds the wallpaper surface itself). Full context loss → `RecoverGPUState()` + `SwitchMode::Wallpaper` reload. Verify no black screen / context-loss loop across transitions.
- **Brightness:** move the `WallpaperSettingsActivity` slider (0–100) → `SETTINGS_CHANGED` broadcast → `nativeSetBrightness(v/100)` → blit-shader `u_brightness` dims the live wallpaper. Confirm the 500 ms retry lands brightness on a freshly-created surface (before the first render completes).

### 2. Service-only JNI natives 1:1 with Java declarations (verification + assert)

Stage 1 Task 8 **already** exported these and removed `nativeCycleZoom` (verified on `lwp2` @ `634dd21703`). Stage 3 asserts the 1:1 invariant still holds *as a boot entry-gate* and fixes any drift only if a later edit breaks it.

- `OpenTTDWallpaperService.java` declares **8** `private static native` methods (lines 29–43): `nativePrepareBackground, nativeSwitchMap, nativeNavigatePOI, nativeRotateMap, nativeScrollCamera, nativeSetGamePaused, nativeSurfaceChanged, nativeSetBrightness` — `nativeCycleZoom` no longer present.
- Each of the 8 has a matching `Java_org_openttd_android_OpenTTDWallpaperService_*` export in `src/video/sdl2_gles_v.cpp` (`:103, :109, :115, :121, :164, :171, :181, :189`). **8↔8, no orphan in either direction.**
- **Acceptance (currently PASSING on `lwp2` @ `634dd21703`, 13/13):** `grep "private static native" .../OpenTTDWallpaperService.java` count == number of `Java_..._OpenTTDWallpaperService_native*` exports in `sdl2_gles_v.cpp` (8 == 8); same 1:1 for `GameActivity` (5 natives `nativeRotateMap, nativeNavigatePOI, nativeScrollCamera, nativeSetGamePaused, nativeSetBrightness` == 5 exports `:127–156`); no orphan either direction. Any `UnsatisfiedLinkError` in the `:wallpaper` process at boot is a 1:1 violation → fix in `sdl2_gles_v.cpp`, not by stubbing Java.

### 3. Asset-packaging build fix (fold in from Stage 1 Q1.1 — single clean build is asset-complete)

The current wart: `copy_assets` (wrapper `CMakeLists.txt:166`) copies `baseset/` + `lang/` from the CMake build dir into `${CMAKE_SOURCE_DIR}/assets` (= `android/app/src/main/assets`, gradle's default asset source dir). It `add_dependencies(copy_assets openttd)` and is passed as a CMake target in `build.gradle` (`targets 'openttd', 'copy_assets'`). But gradle's `merge*Assets`/package step is **not ordered after** the external native build, so a truly clean build packages an APK *before* the assets exist → assetless APK → launch fails with `Error extracting baseset assets` → forced second build.

- **Fix:** make asset merge/packaging depend on the native build (which runs `copy_assets`) so one clean `assembleDebug` is asset-complete. See Open decisions (c) for the mechanism tradeoff; recommendation is the AGP variant hook `mergeAssetsProvider.dependsOn(externalNativeBuild task)`.
- **Constraint:** the fix lives entirely in `android/app/build.gradle` (and/or the wrapper `CMakeLists.txt`) — no engine C++ change. It must survive `skipNativeBuild` (Java-only iterations must not error trying to depend on a non-existent native task).
- **Verify:** from a clean tree (`rm -rf android/app/.cxx android/app/build android/app/src/main/assets`), a single `assembleDebug` produces an APK containing `assets/baseset/*` and `assets/lang/*` (`unzip -l app-debug.apk | grep -c baseset` > 0), and the wallpaper launches without the asset-extraction error on first install.

### 4. GameActivity (debug path) — frozen

`GameActivity` (`:game`) and its 5 natives are unchanged this stage (see Open decisions (d)). It stays available as the Stage 1/2 debug viewer. Only touch it if the asset-packaging fix or a shared JNI change demonstrably breaks it.

## Known risk

- **Overlapping-engine surface-handover race (medium — the binding residual).** Preview→home creates two live `OpenTTDEngine` instances briefly sharing the single-process SDL/GL state. Stage 1 device-verified surface recovery for a **single** engine reinjecting its surface in the `:game` process; it did **not** exercise two overlapping engines handing off in `:wallpaper`. The stale-engine guard (`sOverrideSurface == mEngineSurface`) is the only thing preventing the outgoing engine from tearing down the incoming surface, and it is unverified on-device. *Note:* the Stage-1 `game_state_mutex` guard on `ProcessOverlayActions()` (`sdl2_gles_v.cpp:467`) fixed the *broadcast-action* GL-thread/viewport race (blocker C) but does **not** cover the EGL surface handover between two engines. If transitions flicker/black-out, the fix budget is the Java engine (guard timing / re-injection in `onVisibilityChanged`) and/or the C++ `RecoverContextIfLost()` surface-swap ordering — **not** a redesign. Mitigation ladder: (1) verify as-ported; (2) add logging around `sOverrideSurface` ownership transitions; (3) tighten the guard or the C++ EGL rebind sequence. See Q3.1.
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
| `src/video/sdl2_gles_v.{cpp,h}` | VERIFY / bugfix-only | service JNI natives + surface recovery + pause CV + mutex-guarded `ProcessOverlayActions()` **present and device-verified (Stage 1)**; assert 1:1 as entry gate, fix interaction bugs only (surface-handover ordering per Q3.1). |
| `docs/wallpaper/android-app.md`, this spec/plan | EDIT (Task-9-style) | log deviations + Stage 3 result at the end. |

**No greenfield Java or C++ authoring is planned.** New code appears only if a verification step exposes a bug that cannot be fixed by ordering/guarding existing code.

## Verification gate (physical arm64 device — Pixel 10 Pro)

Use `/usr/bin/python3 tools/run_android.py all` for the build/install/activate/log cycle; supplement with targeted `adb` where noted.

1. **Single clean build is asset-complete:** from a clean tree, one `assembleDebug`; APK contains `assets/baseset/*`+`assets/lang/*`; no second build required; install + first launch shows no `Error extracting baseset assets`.
2. **Set as live wallpaper:** `WallpaperSettingsActivity` → "Set wallpaper" → confirm → home screen renders an isometric map and **animates** (water/vehicles) behind the launcher; logcat shows `:wallpaper` process, libs loaded once, no `UnsatisfiedLinkError`, no `FATAL`.
3. **Preview↔live transition:** enter the system wallpaper picker preview, then apply; verify a single engine ends up rendering, no persistent black frame, logcat shows the stale-engine "skipping pause" path and a clean surface handover.
4. **Screen rotation:** rotate device; wallpaper reloads (`SwitchMode::Wallpaper`), re-fits new dimensions, no stretched/torn frame persists.
5. **Screen-off/on:** turn screen off → within ~150 ms the game thread parks (logcat `game_thread_sleep: entering pause wait`), CPU ≈ 0 (confirm via `top`/simpleperf idle); turn on → resumes (`resumed`), renders again. NOTE: with the Stage-1 POI **stub**, `nativePrepareBackground()` recenters on the map centre rather than jumping to a *distinct* POI — verify the pause/resume + jump-on-hide **mechanism** here; distinct-fresh-POI acceptance is deferred to Stage 2 (Q3.3).
6. **Brightness:** move the slider to ~30% → live wallpaper visibly dims; to 100% → full; confirm it applies on a just-created surface (kill+relaunch service, set brightness immediately, verify the 500 ms retry lands).
7. **Control surface:** `JUMP_POI`/`NEXT_POI`/`PREV_POI` recenter (stub POI), `SWITCH_MAP`/`NEXT_MAP`/`PREV_MAP` reload a new map (`RotateTitleMap`), `SCROLL_CAMERA` pans — all act, no crash. Watch `nativeSwitchMap`: it calls `RotateTitleMap(1)` **synchronously on the JNI binder thread** (`sdl2_gles_v.cpp:111`) — a Stage-1-tracked latent race; if `SWITCH_MAP` misbehaves, route it via a `_gles_switch_map` atomic drained in `ProcessOverlayActions()`.
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
- **Recommendation (ratified by plan-confidence i2):** keep the fork for Stage 3 (do not perturb the thing under test); schedule the patch extraction as a separate, independently-verified cleanup after the Stage 3 gate is green. It is not on this stage's critical path.

**(b) Scope boundary — pure verification vs. how much bugfix budget for reimpl-C++ ↔ ported-Java interaction.**
- *Pure verification* — assume Stages 1–2 green; only assert 1:1 natives + run the gate. Risk: real transition bugs (overlapping engines, surface rebind, warm-up ordering) surface exactly at this integration point and would block the gate with no budget.
- *Verification + bounded bugfix* — allow edits confined to (i) the Java engine lifecycle ordering/guards and (ii) the C++ surface-recovery / pause sequencing in `sdl2_gles_v.cpp`, following the Known-risk mitigation ladders.
- **Recommendation (ratified by plan-confidence i2; reaffirmed i3):** verification + bounded bugfix, capped at those two surfaces; anything requiring renderer/POI redesign bounces back to Stage 1/2. Bug-first, minimal-diff, gate-driven. Stage-1 reality narrows the budget: the C++ surfaces (`RecoverContextIfLost`, pause CV, `ProcessOverlayActions`) are now present and device-proven for the **single-engine** `:game` path, so the only realistic C++ bugfix target left is the **two-engine surface-handover ordering** in `:wallpaper` (Q3.1).

**(c) Asset-packaging fix mechanism.**
- *Gradle variant hook* — `applicationVariants.all { mergeAssetsProvider.configure { dependsOn(<externalNativeBuild task>) } }` (or `tasks.named("merge${Variant}Assets")`). Directly expresses "assets after native build". Cost: AGP task-name/provider API is version-sensitive and must no-op under `skipNativeBuild`.
- *CMake-side* — have `copy_assets` write somewhere gradle already treats as a generated-asset input, or wire the copy as a build step gradle consumes. Cost: fights AGP's asset-merge model; brittle.
- *Unconditional build-twice* — drop the fix, always build twice. Cost: violates the stage goal (single clean build must be asset-complete); rejected.
- **Recommendation (ratified by plan-confidence i2; reaffirmed i3):** the gradle variant hook (`mergeAssetsProvider.dependsOn`), wrapped so it is skipped when `project.hasProperty('skipNativeBuild')`. Keep it a few lines in `build.gradle`; verify against the exact AGP version in use (task name offline-unverifiable — Q3.2). The Stage-1 device gate ran with a **scripted build-twice/asset-precondition workaround** (Stage-1 plan Q2.3), confirming the assetless-first-build wart is real and reproducible — this fix retires that workaround.

**(d) Does GameActivity (debug) need parallel changes, or is it frozen for this stage.**
- *Frozen* — leave `:game` and its 5 natives untouched; it remains the Stage 1/2 debug viewer.
- *Parallel changes* — mirror any brightness-retry / surface-handling refinement into `GameActivity` for consistency.
- **Recommendation (ratified by plan-confidence i2):** frozen. GameActivity already passed Stage 1/2 on its own path; touch it only if the asset fix or a shared JNI change breaks it. Consistency refactors belong to a later cleanup, not the service-enablement stage.

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely. (Iterations 1–2 folded into the body — see Reconciliation Log. Iteration 2's Q2.1 entry-gate question is now moot: Stage 1 landed and is device-verified, so the gate is MET; the survey below holds only the residual items no document edit can close.)

### Iteration 3 — 2026-07-08

#### Q3.1. Stage 1 device-verified surface recovery only for a **single** engine in `:game`; the preview→home **two-engine surface handover** in `:wallpaper` (guarded solely by `sOverrideSurface == mEngineSurface`) is still unverified on-device, and it is the binding residual risk. How much to invest before the first on-device service run?
- [ ] Ship as-ported but add `sOverrideSurface`-ownership transition logging (mitigation-ladder step 2) up front so the first handover is diagnosable; escalate to guard/EGL-rebind ordering only on observed flicker/black-out  *(Recommended)*
- [ ] Pure as-ported, no pre-emptive logging — add only if a bug appears
- [ ] Pre-emptively tighten the stale-engine guard / C++ EGL-rebind ordering before the first run
- [ ] Redesign the surface-handover path (rejected — out of scope)

#### Q3.2. The recommended asset-fix (`mergeAssetsProvider.dependsOn(<externalNativeBuild task>)`) uses an AGP-version-sensitive task/provider name that cannot be confirmed offline. How to de-risk the mechanism?
- [ ] Resolve the exact task name against the in-use AGP version (`./gradlew :app:tasks`) first, guard it under `skipNativeBuild`, and prove it with the clean-tree repro (`rm -rf .cxx build src/main/assets`) before relying on it  *(Recommended)*
- [ ] Hardcode `tasks.named("mergeDebugAssets")` and fix only if it throws
- [ ] Do the ordering fix CMake-side (have `copy_assets` write where gradle already consumes) to avoid the AGP API entirely
- [ ] Keep building twice; drop the single-clean-build goal

#### Q3.3. POI is a Stage-1 STUB (`gles_poi.cpp` recenters on map centre); the real scanner lands in Stage 2. Verification steps 5 & 7 reference POI behavior Stage 3 can only exercise as *stub recenter*. How should the POI-dependent acceptance be framed?
- [ ] Verify the **mechanism** only (jump-on-hide fires, resume renders, `JUMP_POI`/`NEXT_POI` recenter without crash) and explicitly defer distinct-fresh-POI acceptance to Stage 2 — do NOT gate the Stage 3 pass on POI quality  *(Recommended)*
- [ ] Gate Stage 3 on distinct-POI behavior anyway (would force pulling the Stage 2 scanner forward)
- [ ] Drop the POI-touching verification steps from Stage 3 entirely until Stage 2 lands
- [ ] Land a minimal real POI scanner inside Stage 3 to make the steps meaningful

#### Q3.4. Stage 1 disabled audio in wallpaper mode (`InitializeSound`/`InitializeMusic` gated `#ifndef WALLPAPER_BUILD`). The `:wallpaper` process boots the same init path Stage 1 exercised in `:game`. What is Stage 3's obligation here?
- [ ] Informational only — Stage 3 confirms no audio-init crash at *service* boot (blocker A did not recur) but does not re-open audio; audio stays a Stage-1 decision  *(Recommended)*
- [ ] Re-verify the full audio-disable path from scratch in the service process as a first-class gate item
- [ ] Re-enable audio for the wallpaper and add a mute toggle (out of scope — Stage 4/5)
- [ ] Ignore audio entirely; it is fully closed by Stage 1

#### Q3.5. The atlas pre-allocates 4 layers (~80 MB @2048) and ran fine on Pixel 10 Pro in `:game`. `:wallpaper` is a second SDL/GL context; during debug both processes can be alive. Does two-process VRAM need a Stage-3 check?
- [ ] Note it as a Stage-3 endurance watch item (normally only one process renders; both coexist only during debug) and defer memory-pressure hardening to Stage 5  *(Recommended)*
- [ ] Add a dedicated two-process VRAM stress test to the Stage 3 gate
- [ ] Reduce the pre-alloc to 2 layers for the wallpaper process now
- [ ] Not a concern — single active process at runtime, close it

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-07
- **Confidence:** 68% (cap from Readiness — four undecided open-tradeoffs + unmet Stage 1/2 hard dependency)
- **Resolved:** none (first pass)
- **Verified against current tree (`lwp2`):** two-process manifest exact (`GameActivity` `:game` / `OpenTTDWallpaperService` `:wallpaper` + `BIND_WALLPAPER` + `android.service.wallpaper.WallpaperService` filter + `@xml/wallpaper`) ✓; `res/xml/wallpaper.xml` `settingsActivity` ✓; `SDLActivity` service-mode line refs accurate (`sOverrideSurface`/`sServiceMode`/`mServiceContext`/`sOverride*` 208–213, `initialize()` reset 321–326, `initForService()` 333, `getNativeSurface()` 1436–1443, RESUMED `sServiceMode` 739–740, `SDLMain.run` 1935/1957) ✓; `SDLSurface.surfaceChanged()` `sServiceMode` early-return 111 ✓; `GameActivity` = 5 natives ✓; `copy_assets` = `android/app/src/main/CMakeLists.txt:166` + `add_dependencies(copy_assets openttd)`:173 ✓; `build.gradle` `targets 'openttd','copy_assets'`:20 + `skipNativeBuild` guards ✓; `gles` service JNI exports = 8 (no `nativeCycleZoom` export) ✓; `gles` GameActivity exports = 5 ✓.
- **Findings driving the gap:** (1) `sdl2_gles_v.cpp` ABSENT — Stage 1 Task 8/9 not landed; the driver/JNI/surface-recovery/pause-CV the whole stage verifies do not exist in tree [Q1.7→Q2.1]; (2) `OpenTTDWallpaperService.java` still declares 9 natives incl. `nativeCycleZoom` with 0 JNI exports — the spec's 1:1 (8-native) invariant cannot yet hold [Q1.7→Q2.1]; (3) Stage 2 real POI scanner (`gles_poi.cpp`) not landed — only the Stage 2 spec exists [Q2.1]; (4) four open decisions (a–d) undecided by design [Q1.1–Q1.4]; (5) asset-fix mechanism AGP-version-sensitive [Q1.3→Q2.2]; (6) spec stated Stage 1 "only through Task 4" but tree is through Task 5 (snapshot recording blitter) [Q1.5]; (7) overlapping-engine race only observable on-device [Q2.3].
- **Still uncertain:** Readiness (unmet Stage 1/2 dependency) and Unknowns (undecided open-tradeoffs) drive the gap.
- **New questions:** Q1.1 … Q1.7

### Iteration 2 — 2026-07-07
- **Confidence:** 82% (cap from Readiness — Stage 1 Task 8/9 + Stage 2 POI are genuine, external, unmet preconditions that no spec edit can close; the design itself is internally consistent and its Java/manifest/SDL premises are verified).
- **Resolved (Q1.1–Q1.7 folded, dissolved from survey):**
  - Q1.1 → keep the SDL Java fork as-is for Stage 3; schedule patch-extraction as a later independent cleanup → Open decision (a) ratified.
  - Q1.2 → verification + bounded bugfix, capped at Java lifecycle ordering/guards and C++ surface-recovery/pause sequencing → Open decision (b) ratified.
  - Q1.3 → gradle variant hook (`mergeAssetsProvider.dependsOn`), `skipNativeBuild`-guarded → Open decision (c) ratified; version-sensitivity carried forward → Q2.2.
  - Q1.4 → GameActivity frozen → Open decision (d) ratified.
  - Q1.5 → tree is through Task 5, not Task 4 → Context/dependencies staleness corrected.
  - Q1.6 → forward-looking "landed Stage 1" / "already exports" claims re-marked as unmet entry preconditions → Scope §2 + Files table + Context clarified.
  - Q1.7 → the 1:1 native invariant is a boot entry gate, not achievable in-stage until Stage 1 removes `nativeCycleZoom` + adds the 8 exports → refinement → Q2.1.
- **Honesty note on the cap:** unlike a normal tech-spec, this stage's readiness is bounded by *external build state*, not document quality. `sdl2_gles_v.cpp` is absent and the service still has 9 natives / 0 JNI exports; Stage 2's POI scanner is spec-only. Folding survey answers cannot lift this — it lifts only when Stage 1 Task 8/9 and Stage 2 land. Confidence is held at 82% and does NOT reach ≥90%. This plateau is the honest ceiling for the current tree.
- **Still uncertain:** Readiness (Q2.1 external dependency is the binding blocker; Q2.2 AGP API needs offline-unverifiable confirmation; Q2.3 race is on-device-only).
- **New questions:** Q2.1 … Q2.3

### Iteration 3 — 2026-07-08
- **Trigger:** external state change, not a survey fold — **Stage 1 is now complete and device-verified** (Pixel 10 Pro, `lwp2` @ `634dd21703`). Re-checked every Stage-1-dependent premise against the current tree and folded reality into the body.
- **Confidence:** 88% (cap from **Risk** — the overlapping-engine surface-handover race in `:wallpaper` is medium-severity and device-only, and Stage 1 exercised only the single-engine `:game` path; secondary: Readiness, the AGP asset-task name is offline-unverifiable). Up from 82%: the former binding **Readiness** blocker (missing driver + broken 1:1 natives) is cleared.
- **Verified against current tree (`lwp2` @ `634dd21703`):** `src/video/sdl2_gles_v.{cpp,h}` EXIST ✓; driver `VideoDriver_SDL_GLES` name `"sdl-gles"` (`.h:17`) ✓; JNI **13/13 1:1** — 8 service exports (`sdl2_gles_v.cpp:103,109,115,121,164,171,181,189`) ↔ 8 Java natives (`OpenTTDWallpaperService.java:29–43`), 5 GameActivity exports (`:127–156`) ↔ 5 Java natives ✓; `nativeCycleZoom` **removed** from service Java (0 refs) ✓; `RecoverContextIfLost()` def `:669` / call `:497`, full-loss `RecoverGPUState()`+reload `:826,828` ✓; `_gles_surface_changed` `:63`/`:678` ✓; pause CV `video_driver.hpp:207/413/415` + game-thread wait `video_driver.cpp:49–58` ✓; `ProcessOverlayActions()` `:454`, mutex-guarded `:467` (blocker-C fix) ✓; `SetBrightness` `:159/:184` ✓; scoped enums `GameMode::Wallpaper`/`SwitchMode::Wallpaper/None` throughout, **no** `GM_`/`SM_` macros in tree ✓; POI = STUB (`gles_poi.cpp:8`) ✓.
- **Resolved / folded from Stage-1 reality:**
  - Entry gate re-framed from "Stages 1 **and** 2 hard-gated / driver absent" → "**MET**: depends on Stage 1 (done); Stage 2 POI is a stub, not a gate" → Context/dependencies rewritten.
  - Q2.1 (i2) **dissolved** — its premise (driver absent, 9 natives/0 exports) is now false; the 1:1 check moves from "boot precondition that cannot yet hold" to "currently PASSING (13/13)" → Scope §2 rewritten, acceptance marked passing.
  - Scoped-enum drift (`SM_WALLPAPER` → `SwitchMode::Wallpaper`) corrected in Scope §1 (rotation reload, surface recovery).
  - Files table: `sdl2_gles_v.{cpp,h}` re-marked present + device-verified.
  - Known-risk "overlapping-engine" sharpened: Stage 1 proved single-engine recovery only; `ProcessOverlayActions` mutex fix covers broadcast-action race, NOT surface handover.
  - Stage-1 device-gate deltas folded (audio disabled; pointer-math coords; 4-layer/~80 MB atlas; delete+realloc clear).
  - Open decisions (b) + (c) reaffirmed with Stage-1-narrowed framing (C++ bugfix budget now ≈ two-engine handover only; asset workaround confirmed real by the Stage-1 gate).
  - Verification steps 5 & 7 caveated for the POI stub + the `nativeSwitchMap` binder-thread latent race.
- **Still uncertain (why not ≥90%):** Risk — the two-engine surface handover is unexercised and only observable on the device (Q3.1); Readiness — the AGP merge-assets task name can't be confirmed offline (Q3.2). Both are on-device / external and no document edit closes them; this is the honest ceiling until the Stage 3 device run.
- **New questions:** Q3.1 … Q3.5 (Q3.2 carries i2's Q2.2; Q3.1 carries+sharpens i2's Q2.3; Q3.3–Q3.5 are new residuals surfaced by Stage-1 reality: POI-stub acceptance, audio-disable obligation, two-process VRAM).
