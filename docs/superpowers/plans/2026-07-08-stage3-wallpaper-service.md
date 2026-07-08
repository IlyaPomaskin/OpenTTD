# Stage 3 — Live Wallpaper Service Implementation Plan

**Plan-confidence status:** Draft (iteration 4, confidence 88% — plateau). Cap: **Risk** (two-engine preview→home surface handover in `:wallpaper` is device-only and unexercised; the bounded mitigation ladder's sufficiency cannot be proven offline). Secondary: **Readiness** — AGP asset task name `externalNativeBuildDebug` offline-unverifiable (hardcoded + documented fallback). **Correction (iter-4 post-answer):** the iter-4 pass wrongly reported P2 (Stage-2 real POI scanner) unmet — that read a stale tree state. P2 is **MET** in-tree (`gles_poi.cpp` @ `f22e2cb672`, Stage 2 done). Readiness no longer carries an unmet-dependency; the only open Readiness item is the offline-unverifiable task name. The Risk cap (device-only handover) is unchanged and remains the plateau driver — it closes only on the Stage 3 device run. See `## Confidence Survey`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OpenTTD render as a real Android live wallpaper — set via `WallpaperSettingsActivity`, hosted by `OpenTTDWallpaperService` in the `:wallpaper` process — and prove its full lifecycle on a physical device, folding in the one-clean-build asset fix.

**Architecture:** This stage is **on-device wire-up + lifecycle verification + targeted bugfix**, NOT greenfield authoring. The Java `WallpaperService`/`OpenTTDEngine`, the vendored SDL service-mode fork, and the reimpl C++ `sdl-gles` driver (surface recovery, pause CV, mutex-guarded overlay actions) are all present and device-verified for the single-engine `:game` path (Stage 1, `lwp2` @ `634dd21703`). Stage 3 connects them in `:wallpaper`, exercises the two-engine surface handover the `:game` path never hit, and fixes bugs only where reimpl-C++ meets ported-Java. Two real code changes: the gradle asset-ordering hook and two diagnostic log lines around surface ownership.

**Tech Stack:** C++ (OpenTTD engine + `src/video/sdl2_gles_v.cpp` GLES driver), Java (Android wallpaper service + vendored SDL fork), Gradle/AGP 8.11.1 + Gradle 9.0.0, CMake 4.1.2, NDK 27.3.13750724, EGL/OpenGL ES.

## Global Constraints

- **Build gate:** `JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug` → `BUILD SUCCESSFUL`.
- **Target device:** physical arm64-v8a (Pixel 10 Pro). **Emulator is NOT a target** — no gfxstream workarounds.
- **Python:** always `/usr/bin/python3` (homebrew python blocked by sandbox).
- **Driver/toolchain (verbatim):** AGP `8.11.1`, Gradle `9.0.0`, `compileSdk = 36`, `minSdkVersion = 24`, `targetSdkVersion = 36`, `ndkVersion = "27.3.13750724"`, CMake `version = "4.1.2"`.
- **Gradle config-cache is ON** (`org.gradle.configuration-cache=true`, `org.gradle.caching=true`): any task-wiring hook must be config-cache-safe (no `Task.project` access at execution time; wire via `afterEvaluate { tasks.named(...).configure { dependsOn '<string>' } }`).
- **Reimpl principles (binding):** no dead scaffolding; hooks over rewrites; one-hunk `#ifdef WALLPAPER_BUILD` guards; additive interface edits, gate-don't-delete; instrumentation behind `WALLPAPER_PERF`; wallpaper-only + Android-only.
- **Bugfix budget (Open decision (b), ratified):** verification + bounded bugfix only, capped to (i) Java engine lifecycle ordering/guards in `OpenTTDWallpaperService.java`, and (ii) C++ surface-recovery/pause sequencing in `src/video/sdl2_gles_v.cpp`. Anything needing renderer/POI redesign bounces back to Stage 1/2. Bug-first, minimal-diff.
- **Reference branch `gles` is READ-ONLY** — inspect via `git show gles:<path>` / `git diff e24f92ce82..gles -- <files>`; never modify it.
- **Logcat tags:** native `Debug(...)` output → tag **`OpenTTD`**; Java `Log.i(TAG,...)` → tag **`OpenTTDWallpaper`**. `run_android.py logs` filters `-s OpenTTD` only, so device tasks that need Java lifecycle logs MUST use `adb logcat -s OpenTTD:V OpenTTDWallpaper:V`.

## Survey decisions folded in (Confidence Survey, iteration 3)

- **Q3.1 → Log + escalate:** ship the handover as-ported but ensure every `sOverrideSurface` ownership transition is logged *up front* (Task 2); escalate to guard/EGL-rebind tightening only on observed flicker/black-out (contingency in Task 5).
- **Q3.2 → Hardcode task name:** the asset hook hardcodes `mergeDebugAssets` → `externalNativeBuildDebug`; fix only if gradle throws "task not found" (Task 1, fallback documented inline).
- **Q3.3 → Stage 2 POI is a HARD precondition (override):** user guarantees the real Stage 2 scanner lands *before* Stage 3 executes. POI verification (Tasks 7, 10) gates on **distinct-fresh-POI** behavior, not stub recenter. See Entry Preconditions.
- **Q3.4 → Ignore audio:** audio-disable is fully closed by Stage 1. No audio task; explicitly out of scope — do NOT add an audio check.
- **Q3.5 → Two-process VRAM not a concern:** single active render process at runtime. No VRAM task; out of scope.

---

## Entry Preconditions (HARD GATES — verify before Task 1)

Do not start implementation until all three pass. If any fails, stop and report — the stage is not ready.

- [ ] **P1 — Stage 1 complete & device-verified.** `git -C ~/work/OpenTTD log --oneline -1` shows the tree is at/after `634dd21703`; `src/video/sdl2_gles_v.{cpp,h}` exist; APK builds `BUILD SUCCESSFUL`.
- [x] **P2 — Stage 2 real POI scanner landed — MET (Q3.3 override; Q4.2 answer).** Confirmed present in-tree (`lwp2` @ `f22e2cb672`, commit `3dcdcc30c4 Stage 2 done: POI camera scanner on device`): `gles_poi.cpp` implements `ScanStationPOIs`/`ScanJunctionPOIs`/`ScanTownPOIs`/`ScanLighthousePOIs` with scoring, top-50 10-tile dedup, random-20 selection, and `NavigatePOI`/`ShowCurrentPOI`/`DrawPOIMarkers`. No `Stage-1 STUB`, no recenter-on-centre. Per Q4.2 the automated grep is dropped — this is a one-time prose confirmation the real scanner is present. POI verification (Tasks 7, 10) gates on distinct-fresh-POI behavior.
- [ ] **P3 — JNI natives 1:1 (boot entry gate, currently 8/8 + 5/5).** Run:
  ```bash
  echo "wallpaper java:  $(grep -c 'private static native' android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java)"
  echo "wallpaper cpp:   $(grep -c 'Java_org_openttd_android_OpenTTDWallpaperService_native' src/video/sdl2_gles_v.cpp)"
  echo "gameactivity java: $(grep -c 'private static native' android/app/src/main/java/org/openttd/android/GameActivity.java)"
  echo "gameactivity cpp:  $(grep -c 'Java_org_openttd_android_GameActivity_native' src/video/sdl2_gles_v.cpp)"
  ```
  Expected: `8`, `8`, `5`, `5`. Any mismatch is a 1:1 violation — fix in `sdl2_gles_v.cpp`, never by stubbing Java.

---

## File Structure

| File | Responsibility this stage |
|---|---|
| `android/app/build.gradle` | **EDIT (Task 1)** — add config-cache-safe hook: `mergeDebugAssets dependsOn externalNativeBuildDebug`, guarded by `!skipNativeBuild`. |
| `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` | **EDIT (Task 2)** — two `Log.i(TAG,...)` "ownership taken" confirmations after the surface assignments (lines 261, 277). Otherwise VERIFY / bugfix-only. |
| `src/video/sdl2_gles_v.{cpp,h}` | **VERIFY / bugfix-only** — surface-recovery + pause CV + mutex-guarded `ProcessOverlayActions()`; edit only on a proven surface-handover ordering bug (Task 5 escalation). |
| `android/app/src/main/java/org/libsdl/app/SDLActivity.java`, `SDLSurface.java` | **VERIFY-only** — service-mode hooks present; do not touch (fork kept as-is, decision (a)). |
| `android/app/src/main/CMakeLists.txt` | **VERIFY-only** — `copy_assets` target correct as-is (line 166); no edit unless the gradle hook proves unworkable. |
| `docs/wallpaper/android-app.md`, this plan, `.superpowers/sdd/progress.md` | **EDIT (Task 12)** — record deviations + Stage 3 device-gate result. |
| `android/app/src/main/java/org/openttd/android/GameActivity.java` | **FROZEN** (decision (d)) — touch only if a shared change demonstrably breaks `:game`. |

---

## Task 1: Asset-packaging build fix (single clean build is asset-complete)

**Files:**
- Modify: `android/app/build.gradle` (insert at line 55, between `android { }` close and `dependencies {`)

**Interfaces:**
- Consumes: existing `copy_assets` CMake target (`android/app/src/main/CMakeLists.txt:166`, copies `${CMAKE_BINARY_DIR}/{baseset,lang}` → `android/app/src/main/assets/{baseset,lang}`; `add_dependencies(copy_assets openttd)` at :173), already listed in `targets 'openttd', 'copy_assets'` (`build.gradle:20`).
- Produces: nothing consumed by later tasks — this task's deliverable is a clean build that packages assets in one pass.

**Context:** `copy_assets` runs as an `ALL` target during the native build, but AGP schedules `mergeDebugAssets` independently of `externalNativeBuildDebug`, so a truly clean `assembleDebug` can package the APK before assets are copied → assetless APK → `Error extracting baseset assets` on first launch → forced second build. The Stage-1 device gate confirmed this wart is real (it ran a scripted build-twice workaround). Fix = make the asset merge depend on the native build.

- [ ] **Step 1: Reproduce the wart (diagnostic; may be timing-dependent)**

```bash
rm -rf android/app/.cxx android/app/build android/app/src/main/assets
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
unzip -l android/app/build/outputs/apk/debug/app-debug.apk | grep -c 'assets/baseset'
```
Expected (bug present): `BUILD SUCCESSFUL` but the `grep -c` prints `0` (or a number lower than the baseset file count). If it already prints `>0`, the ordering happened to win this run — proceed anyway; the hook makes it deterministic.

- [ ] **Step 2: Add the dependency hook**

Insert at `android/app/build.gradle:55` (the blank line between the `android { }` close at line 54 and `dependencies {` at line 56):

```gradle
// Ensure generated baseset/lang assets (written by the copy_assets CMake
// target during the native build) exist before AGP merges assets into the
// APK, so a single clean assembleDebug is asset-complete. Guarded so
// Java-only iterations (-PskipNativeBuild) don't reference a missing task.
if (!project.hasProperty('skipNativeBuild')) {
    afterEvaluate {
        tasks.named('mergeDebugAssets').configure {
            dependsOn 'externalNativeBuildDebug'
        }
    }
}
```

- [ ] **Step 3: Clean build, verify asset-complete in ONE pass**

```bash
rm -rf android/app/.cxx android/app/build android/app/src/main/assets
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
unzip -l android/app/build/outputs/apk/debug/app-debug.apk | grep -c 'assets/baseset'
unzip -l android/app/build/outputs/apk/debug/app-debug.apk | grep -c 'assets/lang'
```
Expected: `BUILD SUCCESSFUL`; both `grep -c` print `>0`.

**On failure — gradle throws `Task with name 'externalNativeBuildDebug' not found`** (Q3.2 "fix only if it throws"): the per-variant native lifecycle task name differs in this AGP build. Confirm the real name and switch the `dependsOn` string:
```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android :app:tasks --all | grep -iE 'externalNativeBuild|buildCMake'
```
Fallback candidate: `dependsOn 'buildCMakeDebug[arm64-v8a]'` (per-ABI task; ABI matches `abiFilters 'arm64-v8a'` at `build.gradle:24`). Re-run Step 3.

- [ ] **Step 4: Verify `skipNativeBuild` still works (Java-only path must not error)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug -PskipNativeBuild
```
Expected: `BUILD SUCCESSFUL` with no "task not found" error (the `if (!project.hasProperty('skipNativeBuild'))` guard skips the hook).

- [ ] **Step 5: Install + first-launch has no asset error**

```bash
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
sleep 4
adb logcat -d -s OpenTTD | grep -c 'Error extracting baseset'
```
Expected: `0`.

- [ ] **Step 6: Commit**

```bash
git add android/app/build.gradle
git commit -m "android: order mergeDebugAssets after native build so one clean build is asset-complete"
```

---

## Task 2: Surface-ownership transition logging (Q3.1 — diagnose the handover up front)

**Files:**
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` (lines 260-261 and 276-277)

**Interfaces:**
- Consumes: existing `TAG = "OpenTTDWallpaper"` (`:18`), `import android.util.Log;` (`:9`), `mEngineSurface` field (`:206`), external static `SDLActivity.sOverrideSurface`.
- Produces: log lines Task 5 greps to trace which engine owns the surface across preview→home.

**Context:** The engine already logs most ownership transitions: `onSurfaceCreated` READ+compare (`:254-259`), `onSurfaceDestroyed` active/stale branches (`:286-296`), `onVisibilityChanged` states + re-inject (`:303-327`). The gap is an explicit, greppable **ownership** marker on the two **assignments**: `onSurfaceCreated` (`:261`) is immediately followed at `:262` by `"onSurfaceCreated: calling onNativeSurfaceCreated surface=" + mEngineSurface` (logs the surface, but not labelled as ownership taken and keyed on `mEngineSurface`, not `sOverrideSurface`); `onSurfaceChanged` (`:277`) has **no** adjacent log at all before its native calls (`:278+`). Add exactly the two "…ownership" lines below so both assignments are greppable via a single `…ownership` token; do not add redundant logging elsewhere.

- [ ] **Step 1: Add "ownership taken" log in `onSurfaceCreated`**

The current lines 260-261 are:
```java
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
```
Add one line immediately after 261:
```java
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            Log.i(TAG, "onSurfaceCreated: took surface ownership sOverrideSurface=" + SDLActivity.sOverrideSurface);
```

- [ ] **Step 2: Add "ownership taken" log in `onSurfaceChanged`**

The current lines 276-277 are:
```java
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
```
Add one line immediately after 277:
```java
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            Log.i(TAG, "onSurfaceChanged: re-took surface ownership sOverrideSurface=" + SDLActivity.sOverrideSurface);
```

- [ ] **Step 3: Build**

```bash
/usr/bin/python3 tools/run_android.py build
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 4: Confirm every transition is now bracketed**

```bash
grep -n 'took surface ownership\|re-took surface ownership\|active engine\|stale engine\|re-injecting lost surface' \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected: 5 hits — assign (onSurfaceCreated), reassign (onSurfaceChanged), active + stale (onSurfaceDestroyed), re-inject (onVisibilityChanged). Every ASSIGN / NULL / COMPARE of `sOverrideSurface` now has an adjacent log line.

- [ ] **Step 5: Commit**

```bash
git add android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "wallpaper: log surface ownership transitions to diagnose two-engine handover"
```

---

## Task 3: JNI 1:1 entry-gate assertion + clean boot

**Files:** none (verification-only; produces no code — records result in Task 12).

**Interfaces:** consumes the natives verified in P3.

- [ ] **Step 1: Re-run the 1:1 counts (offline)**

```bash
test "$(grep -c 'private static native' android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java)" = 8 && echo OK-wallpaper-java
test "$(grep -c 'Java_org_openttd_android_OpenTTDWallpaperService_native' src/video/sdl2_gles_v.cpp)" = 8 && echo OK-wallpaper-cpp
test "$(grep -c 'private static native' android/app/src/main/java/org/openttd/android/GameActivity.java)" = 5 && echo OK-game-java
test "$(grep -c 'Java_org_openttd_android_GameActivity_native' src/video/sdl2_gles_v.cpp)" = 5 && echo OK-game-cpp
```
Expected: all four `OK-*` printed.

- [ ] **Step 2: Boot the service, assert no `UnsatisfiedLinkError`**

```bash
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
# tap "Set wallpaper" → confirm → home (manual)
sleep 8
adb logcat -d | grep -E 'UnsatisfiedLinkError|FATAL EXCEPTION' | grep -i openttd
```
**Acceptance:** no output. Any `UnsatisfiedLinkError` in `:wallpaper` = 1:1 violation → fix the missing/extra export in `sdl2_gles_v.cpp` (not Java), rebuild, repeat.

---

## Task 4: Set-live wallpaper flow (gate item 2)

**Files:** none unless a bug surfaces (budget: Java lifecycle).

- [ ] **Step 1: Set as live wallpaper**

```bash
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
```
Then manually: tap **"Set wallpaper"** → system live-wallpaper preview → **confirm** → home screen.

- [ ] **Step 2: Capture evidence (both tags)**

```bash
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_setlive.log
grep -E 'onCreate|onSurfaceCreated|took surface ownership|SDL_main|initForService' /tmp/openttd/stage3_setlive.log
```

**Acceptance:**
- `:wallpaper` process starts (`adb shell ps -A | grep ':wallpaper'` non-empty while live).
- Libs load once, `SDL_main`/service init runs once (`sSDLInitialized` path taken a single time — one `initForService` / one `onSurfaceCreated: took surface ownership`).
- Home screen renders an isometric map and **animates** (water/vehicles) behind launcher icons (visual confirmation; supplement with `run_android.py record 5`).
- No `FATAL`, no `UnsatisfiedLinkError`.

**On failure:** if render is black but no crash, jump to Task 8 (surface recovery). If lifecycle mis-orders (double init, etc.), fix within Java-lifecycle budget.

---

## Task 5: Preview↔live two-engine surface handover (gate item 3 — THE binding risk, Q3.1)

**Files:** none unless flicker/black-out is observed; then bounded fix (see escalation).

**Context:** preview→home briefly runs two `OpenTTDEngine` instances sharing single-process SDL/GL state. The only thing stopping the outgoing engine from tearing down the incoming surface is the stale-engine guard `sOverrideSurface == mEngineSurface` (`OpenTTDWallpaperService.java:288`). Stage 1 never exercised two overlapping engines. Task 2's logs make the ownership transfer traceable.

- [ ] **Step 1: Drive the transition with full logging**

```bash
adb logcat -c
# In the system wallpaper picker: enter preview, then Apply/confirm to home.
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_handover.log
```

- [ ] **Step 2: Trace the ownership handoff**

```bash
grep -nE 'took surface ownership|re-took surface ownership|active engine|stale engine|re-injecting|surface_changed|RecoverContextIfLost' \
  /tmp/openttd/stage3_handover.log
```

**Acceptance:**
- Exactly one engine ends up owning `sOverrideSurface` (the log shows the stale engine hitting `onSurfaceDestroyed: stale engine — skipping pause to avoid blocking new surface`, and the incoming engine `took surface ownership`).
- No persistent black frame after the transition settles.
- The native side shows a clean `[CTX] surface_changed: swapped surf ... → ...` for the incoming surface, no `[CTX] surface_changed: create failed` loop.

- [ ] **Step 3 (escalation — ONLY if flicker/black-out observed):** apply the mitigation ladder in order, minimal-diff, re-verify after each:
  1. Already done: ownership logging (Task 2) — use it to pinpoint whether the outgoing engine nulled `sOverrideSurface` after the incoming took it.
  2. **Java:** tighten guard/re-injection timing in `onVisibilityChanged` / `onSurfaceDestroyed` (e.g. ensure the stale engine's null/destroy cannot run after the incoming assign). Budget: Java lifecycle ordering.
  3. **C++:** fix `RecoverContextIfLost()` surface-swap ordering in `sdl2_gles_v.cpp` (def `:669`, call `:497`, swap block `:678`). Budget: C++ surface-recovery sequencing.
  Do NOT redesign the handover path. If a fix is applied, commit with a message describing the observed race and the ladder step taken.

  **If still not fixable within budget (Q4.1 answer):** do NOT block the stage. Record the handover flicker/black-out as a **known limitation**, PASS the remaining gate items (Tasks 4, 6–11), and track the fix as a Stage-3 follow-up (logged in Task 12). A handover redesign is out of this stage's budget.

---

## Task 6: Screen rotation → `SwitchMode::Wallpaper` reload (gate item 4)

**Files:** none unless a stretched/torn frame persists.

- [ ] **Step 1: Rotate with logging**

```bash
adb logcat -c
adb shell settings put system accelerometer_rotation 1
# rotate device portrait↔landscape (physically, or:)
adb shell settings put system user_rotation 1   # landscape
sleep 3
adb shell settings put system user_rotation 0   # portrait
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_rotate.log
grep -nE 'onSurfaceChanged|re-took surface ownership|surface_changed|RecoverContextIfLost: recovery done|switch_mode' /tmp/openttd/stage3_rotate.log
```

**Acceptance:** each rotation triggers `onSurfaceChanged` (new dims) → `re-took surface ownership` → native `[CTX] surface_changed: swapped` → map reloads at new dimensions (`RecoverContextIfLost: recovery done, switch_mode=...`). No stretched/torn frame persists after reload (visual; supplement `run_android.py record 5` mid-rotation).

**On failure:** stale-snapshot-at-old-size → inspect the dimension-change → `SwitchMode::Wallpaper` reload path; budget C++ surface-recovery.

---

## Task 7: Screen-off/on pause & resume — ≈0 CPU when hidden, distinct POI on resume (gate item 5, Q3.3)

**Files:** none unless resume shows black/stale frame or CPU spike.

**Context:** hide → `nativePrepareBackground()` (real POI jump per P2) while still visible → +150 ms → `nativeSetGamePaused(true)` parks the game thread on the pause CV → CPU ≈ 0. Show → re-inject surface if lost → 5 warm-up ticks → resume → brightness re-push.

- [ ] **Step 1: Screen off, confirm the game thread parks**

```bash
adb logcat -c
adb shell input keyevent KEYCODE_SLEEP
sleep 2
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_off.log
grep -nE 'jumping POI, delaying pause for warm-up|warm-up done, pausing game thread|\[LOAD\] game_thread_sleep: entering pause wait' /tmp/openttd/stage3_off.log
```
**Acceptance:** all three lines present in order, within ~150 ms of hide.

- [ ] **Step 2: Confirm CPU ≈ 0 while hidden**

```bash
PID=$(adb shell pgrep -f ':wallpaper')
adb shell top -H -p "$PID" -n 1 -b | head -20
```
**Acceptance:** no thread of the `:wallpaper` process consuming meaningful CPU (game thread blocked on `game_pause_cv`; GL thread idle-skips swaps).

- [ ] **Step 3: Screen on, confirm resume + distinct POI**

```bash
adb logcat -c
adb shell input keyevent KEYCODE_WAKEUP
# unlock if needed
sleep 3
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_on.log
grep -nE 'resuming game thread|\[LOAD\] game_thread_sleep: resumed|GameThread: paused — running 5 warm-up ticks' /tmp/openttd/stage3_on.log
```
**Acceptance:** `resuming game thread` → `[LOAD] game_thread_sleep: resumed` → renders again. With the **real Stage 2 POI scanner** (P2), the camera resumes at a **distinct** location vs. before hide (visual confirmation across two sleep/wake cycles — the pre-hide `nativePrepareBackground()` jump lands a different POI each time). No black/stale frame, no CPU spike on resume.

**On failure:** black/stale on resume or CPU spike → inspect ordering of surface re-injection vs `nativeSetGamePaused(false)` vs `RESUMED` in `onVisibilityChanged` (`:315-332`); budget Java lifecycle.

---

## Task 8: Surface recovery across transitions — no context-loss loop (gate item covering §Scope-1 surface recovery)

**Files:** none unless a context-loss loop appears.

- [ ] **Step 1: Force several surface (re)injections, watch recovery**

```bash
adb logcat -c
# cycle: home → recents → app → home; sleep/wake; rotate once
adb shell input keyevent KEYCODE_HOME
adb shell input keyevent KEYCODE_APP_SWITCH ; sleep 1 ; adb shell input keyevent KEYCODE_HOME
adb shell input keyevent KEYCODE_SLEEP ; sleep 2 ; adb shell input keyevent KEYCODE_WAKEUP
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_recovery.log
grep -cE '\[CTX\] surface_changed: create failed' /tmp/openttd/stage3_recovery.log
grep -nE '\[CTX\] surface_changed: swapped|RecoverContextIfLost: recovery done|RecoverGPUState: done' /tmp/openttd/stage3_recovery.log
```

**Acceptance:** every reinjection produces a clean `[CTX] surface_changed: swapped surf ... → ...` (or `same surface, just rebinding`); `create failed` count is `0` (no context-loss loop); if a full loss occurs, exactly one `RecoverGPUState: done — triggering map reload` per loss, recovering to a rendering state. No black screen persists.

**On failure:** repeated `create failed` = context-loss loop → inspect `RecoverContextIfLost()` EGL rebind (`sdl2_gles_v.cpp:669-830`); budget C++ surface-recovery.

---

## Task 9: Brightness slider + 500 ms retry on fresh surface (gate item 6)

**Files:** none unless brightness intermittently fails to apply.

- [ ] **Step 1: Slider dims the live wallpaper**

```bash
adb logcat -c
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
# move slider to ~30%
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'SETTINGS_CHANGED|Brightness|pushBrightness'
```
**Acceptance:** slider → `SETTINGS_CHANGED` → `nativeSetBrightness(0.30)` → wallpaper visibly dims; 100% → full brightness (visual, supplement `run_android.py record 5`).

- [ ] **Step 2: Confirm the 500 ms retry lands on a just-created surface**

```bash
adb shell am force-stop org.openttd.android
adb logcat -c
# re-set live wallpaper, then move slider immediately (within ~1s of the surface appearing)
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'pushBrightness|GLESBackend'
```
**Acceptance:** brightness applies even when set before the first render completes (the 500 ms delayed `pushBrightness()` retry lands once `sLibrariesLoaded && sSDLInitialized`). No permanently-wrong brightness on a fresh surface.

**On failure:** intermittent → the retry cadence / a "backend ready" gate is the lever (low-risk); budget Java lifecycle.

---

## Task 10: Broadcast control surface — distinct POIs, map switch, scroll (gate item 7, Q3.3)

**Files:** none unless a broadcast crashes or `SWITCH_MAP` races.

**Context:** 7 receivers + `SETTINGS_CHANGED`. `run_android.py` only wraps `JUMP_POI` (`jump`) and `SWITCH_MAP` (`switch`); the rest go via raw `adb`.

- [ ] **Step 1: POI navigation lands DISTINCT POIs (real scanner, P2)**

```bash
adb logcat -c
/usr/bin/python3 tools/run_android.py jump          # JUMP_POI
adb shell am broadcast -a org.openttd.android.NEXT_POI
adb shell am broadcast -a org.openttd.android.PREV_POI
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'JUMP_POI|NEXT_POI|PREV_POI|POI'
```
**Acceptance:** each recenters on a **distinct** POI (not the map centre — P2 real scanner), no crash. `JUMP_POI`/`NEXT_POI` advance; `PREV_POI` goes back.

- [ ] **Step 2: Map switch + scroll**

```bash
adb logcat -c
/usr/bin/python3 tools/run_android.py switch        # SWITCH_MAP
adb shell am broadcast -a org.openttd.android.NEXT_MAP
adb shell am broadcast -a org.openttd.android.PREV_MAP
adb shell am broadcast -a org.openttd.android.SCROLL_CAMERA
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'SWITCH_MAP|NEXT_MAP|PREV_MAP|SCROLL_CAMERA|RotateTitleMap'
```
**Acceptance:** `SWITCH_MAP`/`NEXT_MAP`/`PREV_MAP` reload a new map (`RotateTitleMap`), `SCROLL_CAMERA` pans — all act, no crash.

- [ ] **Step 3 (watch item):** `nativeSwitchMap` calls `RotateTitleMap(1)` **synchronously on the JNI binder thread** (`sdl2_gles_v.cpp:111`) — a Stage-1-tracked latent race. If `SWITCH_MAP` misbehaves (torn frame / crash under rapid repeats), route it via a `_gles_switch_map` atomic drained in `ProcessOverlayActions()` (mirrors the existing overlay-action pattern, `:454-471`). Budget C++. Commit if fixed.

---

## Task 11: Endurance — no accumulating leak/loop/crash (gate item 8)

**Files:** none unless a leak/loop/crash appears.

- [ ] **Step 1: Long run across many transitions**

```bash
adb logcat -c
/usr/bin/python3 tools/run_android.py all 10 6
# additionally, manually cycle sleep/wake + rotation + preview several times over ~5 min
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V > /tmp/openttd/stage3_endurance.log
grep -cE '\[CTX\] surface_changed: create failed|FATAL|UnsatisfiedLinkError' /tmp/openttd/stage3_endurance.log
```
**Acceptance:** the `grep -c` prints `0` — no accumulating context-loss loop, no crash. `dumpsys meminfo` (below) stable across the run:
```bash
PID=$(adb shell pgrep -f ':wallpaper'); adb shell dumpsys meminfo "$PID" | grep -E 'TOTAL|Gfx|EGL|GL '
```
**Acceptance:** GFX/EGL memory does not climb monotonically across cycles (single active render process; two-process VRAM out of scope per Q3.5).

---

## Task 12: Record deviations + Stage 3 result

**Files:**
- Modify: `.superpowers/sdd/progress.md` (append Stage 3 entry)
- Modify: `docs/wallpaper/android-app.md` (Task-9-style deviation log at end)
- Modify: `docs/superpowers/specs/2026-07-07-stage3-wallpaper-service-design.md` (Reconciliation Log — Stage 3 executed entry)

- [ ] **Step 1: Write the result log**

Record, per gate item (Tasks 3–11): PASS/FAIL, the evidence file under `/tmp/openttd/stage3_*.log`, and any bugfix applied (which ladder step, which file, commit hash). Note the asset-fix task name actually used (`externalNativeBuildDebug` vs fallback). Note any `SWITCH_MAP` binder-race mitigation.

- [ ] **Step 2: Commit**

```bash
git add .superpowers/sdd/progress.md docs/wallpaper/android-app.md docs/superpowers/specs/2026-07-07-stage3-wallpaper-service-design.md
git commit -m "docs: Stage 3 device-gate result + deviations"
```

---

## Out of scope (do NOT add tasks for these)

- **Audio (Q3.4):** fully closed by Stage 1 (`InitializeSound`/`InitializeMusic` gated `#ifndef WALLPAPER_BUILD`). No re-verification, no re-enable.
- **Two-process VRAM (Q3.5):** single active render process at runtime; memory-pressure hardening is Stage 5.
- **SDL fork → patch extraction (decision (a)):** later independent cleanup, not this stage.
- **GameActivity feature work (decision (d)):** frozen.
- **Settings UX beyond brightness:** Stage 4. **Perf/battery hardening, `WALLPAPER_PERF`-off build:** Stage 5.
- **POI scoring / renderer internals:** Stage 1/2 territory.

## Self-Review

- **Spec coverage:** Scope §1 lifecycle → Tasks 4–10; §2 JNI 1:1 → P3 + Task 3; §3 asset fix → Task 1; §4 GameActivity frozen → out-of-scope. Verification gate items 1→Task 1, 2→Task 4, 3→Task 5, 4→Task 6, 5→Task 7, 6→Task 9, 7→Task 10, 8→Task 11; surface recovery (§1) → Task 8. Known-risk overlapping-engine → Task 5 (+ Task 2 logging). All survey answers folded (see decisions block). ✓
- **Precondition gap closed:** Q3.3 override converts Stage 2 POI from stub-accept to hard gate P2 — the current tree is still the stub, so P2 will block until Stage 2 lands (by design).
- **Placeholder scan:** all code steps carry verbatim snippets + exact line anchors; all device steps carry exact `adb`/grep commands + expected output. No TBD/TODO. ✓
- **Type/name consistency:** log strings in Task 2 match the greps in Tasks 4/5; native log literals (`[CTX] surface_changed`, `[LOAD] game_thread_sleep`, `RecoverGPUState`) match the source (research-verified line refs). Task names (`mergeDebugAssets`, `externalNativeBuildDebug`) consistent between Task 1 and Global Constraints. ✓
- **Format note:** device-verification tasks use Action → Evidence → Acceptance → On-failure (not the code-authoring TDD cycle) because the deliverable is observed on-device behavior, not a new unit under test. Only Tasks 1 & 2 change code and follow reproduce/fix/verify/commit. (Task 1 opens with a failing-test-first "reproduce the wart" step; Task 2 adds two diagnostic log lines with no meaningful pre-image test — the deliberate TDD deviation for this stage's verification/diagnostic nature.)

---

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely. (Iterations 1–3 live in the sibling design spec `docs/superpowers/specs/2026-07-07-stage3-wallpaper-service-design.md`; Q3.1–Q3.5 are already folded into this plan — see "Survey decisions folded in". The questions below are the plan-layer residuals the first plan-confidence pass on this file surfaced; none can lift the ceiling above the ~88% plateau, but answering them hardens the plan for execution.)

### Iteration 4 — 2026-07-08

#### Q4.1. Task 5's two-engine preview→home surface handover is the binding residual risk and is observable only on-device. If flicker/black-out is NOT fixable within the bounded budget (Java guard-timing + C++ `RecoverContextIfLost` swap ordering) — i.e. it needs a handover redesign — how should the Stage 3 gate resolve?
- [ ] Bounce the handover defect back to Stage 1/2 and BLOCK Stage 3 completion until fixed there (as the plan currently states)  *(Recommended)*
- [x] Record the handover defect as a known limitation, PASS the rest of the gate, and track the fix as a Stage-3 follow-up
- [ ] Widen the Stage 3 bugfix budget to permit a bounded handover redesign confined to `:wallpaper`
- [ ] Add a serialization workaround (defer the incoming engine until the outgoing fully tears down, forcing single-engine) as a first-class Task 5 step

#### Q4.2. The P2 hard gate's automated signal is `grep -c "Stage-1 STUB" src/video/gles_poi.cpp` → 0, but that string currently lives in the file's `@file` doc comment (line 8), not only in stub logic — so the gate keys on a comment. When Stage 2 lands the real scanner, how should P2 robustly detect stub-vs-real?
- [ ] Also key P2 on ABSENCE of the recenter call (`grep -c 'Map::SizeX()/2' src/video/gles_poi.cpp` → 0) alongside the doc-comment grep, so a leftover comment cannot mask a still-stub scanner  *(Recommended)*
- [ ] Keep the `Stage-1 STUB` grep as-is and make removing that string from the `@file` comment part of Stage 2's definition-of-done
- [ ] Replace the grep with a runtime check (broadcast `JUMP_POI` twice, assert two distinct camera tiles) as the P2 gate
- [x] Drop the automated grep; rely on the prose "real scanner, not recenter" judgement at execution time

> **Answer + correction:** the whole premise is stale. The real Stage 2 POI scanner is **already landed** (`gles_poi.cpp` @ `f22e2cb672`: `ScanStationPOIs`/`ScanJunctionPOIs`/`ScanTownPOIs`/`ScanLighthousePOIs`, top-50 dedup, random-20, `NavigatePOI`/`ShowCurrentPOI`/`DrawPOIMarkers`; `grep -c "Stage-1 STUB"` → 0, `grep -c "Map::SizeX()/2"` recenter → 0). P2 is **MET now**, not a blocking external gate. The grep is dropped per the answer; P2 becomes a one-time confirmation that the real scanner is present.

#### Q4.3. Task 6 drives rotation via `adb shell settings put system user_rotation`. On many launchers a live-wallpaper surface does not follow forced `user_rotation` (only foreground activities rotate), so `onSurfaceChanged` with new dims may never fire. How should rotation be exercised so the gate is meaningful?
- [ ] Require physical rotation (or a launcher confirmed to rotate the wallpaper surface); treat absence of `onSurfaceChanged`+new-dims as a test-setup failure, not a pass  *(Recommended)*
- [ ] Keep `settings put user_rotation` and accept whatever the launcher does
- [ ] Add a pre-check that the current launcher propagates rotation to wallpapers before running Task 6
- [ ] Force the configuration change via `adb shell wm` orientation override instead of `user_rotation`

> **Answer (override):** keep it simple — the acceptance keys on `onSurfaceChanged` firing with new dims. Don't add launcher pre-checks or `wm` overrides. If `settings put user_rotation` doesn't make `onSurfaceChanged` fire, rotate the device physically; the signal is the gate, not the trigger mechanism.

#### Q4.4. Task 11 endurance is `run_android.py all 10 6` plus ~5 min of manual cycling. For a leak/loop/crash gate on a long-lived wallpaper, is ~5 min a sufficient soak?
- [x] ~5 min of mixed transitions is enough to catch per-transition leaks/loops for this gate; longer battery/soak testing is Stage 5  *(Recommended)*
- [ ] Extend to a ≥30 min soak with periodic `dumpsys meminfo` sampling to catch slow leaks
- [ ] Add an automated N-cycle loop (e.g. 100× sleep/wake+rotate) with a memory-delta assertion
- [ ] Keep ~5 min but add an explicit `dumpsys meminfo` growth threshold as a hard pass/fail number

#### Q4.5. The sibling design spec still states POI is a stub / not a hard dependency in its BODY (status line, Context, verification steps 5 & 7); its Q3.3 override (Stage 2 is a hard precondition; distinct-fresh-POI acceptance) was recorded but never folded there — so plan and spec now disagree. Should Task 12 reconcile the spec?
- [x] Task 12 folds Q3.3 into the spec BODY (rewrite its steps 5 & 7 + Context + status line to match this plan), not only append to its Reconciliation Log  *(Recommended)*
- [ ] Leave the spec body as-is; the plan is the execution authority and the spec is historical
- [ ] Add only a one-line pointer in the spec noting the plan supersedes its POI-stub language
- [ ] Defer the spec reconciliation to a separate docs cleanup outside Stage 3

> **Answer:** POI is **already implemented** (user-confirmed; verified in `gles_poi.cpp` @ `f22e2cb672`). Task 12 folds this into the sibling spec body — rewrite its POI-stub language (status line, Context, verification steps 5 & 7) to "real scanner present; distinct-fresh-POI acceptance active", not just a log append. The stub framing is obsolete in BOTH docs.

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 4 — 2026-07-08 02:00
- **Lineage:** iterations 1–3 live in the sibling design spec; this plan was authored from spec iteration 3 with Q3.1–Q3.5 pre-folded (see "Survey decisions folded in"). This is the first plan-confidence pass on the PLAN file — no prior `[x]` in this file to dissolve.
- **Confidence:** 88% (plateau) — cap from **Risk** (two-engine surface handover in `:wallpaper` is device-only and unexercised; Stage 1 proved only single-engine `:game` recovery; the bounded mitigation ladder's sufficiency is unprovable offline). Secondary **Readiness**: AGP asset task name `externalNativeBuildDebug` offline-unverifiable (hardcoded + documented fallback), and P2 (Stage-2 real POI scanner) is unmet in-tree today — an external gate, not a plan defect.
- **Verified against tree (`lwp2` @ `0b826b7c39`, engine @ `634dd21703`):** all load-bearing anchors accurate. build.gradle insert point (54 `}` / 55 blank / 56 `dependencies {`) ✓; `targets 'openttd','copy_assets'` :20, `abiFilters 'arm64-v8a'` :24 ✓; Java lifecycle anchors 260-261 / 276-277 exact, stale-engine guard `sOverrideSurface == mEngineSurface` :288 exact ✓; Task-2 grep yields exactly 5 distinct lines ✓; JNI exports at every cited line (service 103/109/115/121/164/171/181/189, game 127/133/139/146/156) ✓; C++ `RecoverContextIfLost` :669 / call :497 / swap :678, `ProcessOverlayActions` :454 / mutex :467 / call :496, `RotateTitleMap(1)` :111, `SetBrightness` :159/:184, `_gles_surface_changed` :63, `_gles_jump_waypoint` :54/:105 ✓; every acceptance-grep log literal exists (warm-up/pause `video_driver.cpp:50/55/61` + Java `:328/:338/:342`; `[CTX] RecoverGPUState: done — triggering map reload` `gles_backend.cpp:381`; `[CTX] surface_changed: swapped/create failed` :736/:745; `initForService` Java:246 / SDLActivity:333) ✓. P3 counts 8/8 + 5/5 ✓. P2 `Stage-1 STUB` grep = 1 (stub still present — gate correctly STOPs today) ✓.
- **Cross-check vs sibling spec:** folded Q3.1–Q3.5 decisions match the spec's answered `[x]` boxes (Q3.1 log+escalate, Q3.2 hardcode task name, Q3.3 override→hard precondition, Q3.4 ignore audio, Q3.5 VRAM not a concern) ✓. Discrepancy: the spec BODY still carries stub-accept POI language its Q3.3 override retired — plan is correct, spec is stale → surfaced as Q4.5.
- **Folded this pass:** (1) Task 2 Context corrected — `onSurfaceCreated` already logs the surface at :262 (keyed on `mEngineSurface`); the genuine absent-log gap is `onSurfaceChanged` :277. The two new lines add an explicit greppable `…ownership` token. (2) Self-Review Format note now states the Task-2 TDD deviation explicitly.
- **Still uncertain (why not ≥90%):** Risk — two-engine handover is on-device-only; Readiness — AGP task name offline-unverifiable + P2 external gate unmet. No document edit closes these; they close only when Stage 2 lands (P2) and the Stage 3 device run executes. Honest ceiling for the current tree is ~88%.
- **New questions:** Q4.1 … Q4.5 (all task-layer: handover-failure disposition, P2 gate robustness, rotation method, endurance soak, plan/spec reconciliation).

### Iteration 4 — 2026-07-08 (post-answer correction)
- **Trigger:** user answered Q4.1–Q4.5 and asserted "POI already implemented." Re-checked the tree directly — the assertion is correct and the iter-4 pass was wrong.
- **CORRECTION — stale-tree read:** iter-4 reported HEAD `0b826b7c39` and `gles_poi.cpp` = Stage-1 stub (`Stage-1 STUB` grep = 1). Real state is HEAD **`f22e2cb672`** with the **full Stage-2 POI scanner landed** (commit `3dcdcc30c4 Stage 2 done: POI camera scanner on device`; `cd83925067 GLES POI: real map scanner`). Direct verify now: `grep -c "Stage-1 STUB" gles_poi.cpp` → **0**, `grep -c "Map::SizeX()/2"` recenter → **0**; file contains `ScanStationPOIs`/`ScanJunctionPOIs`/`ScanTownPOIs`/`ScanLighthousePOIs` + `NavigatePOI`/`ShowCurrentPOI`/`DrawPOIMarkers`. **P2 is MET.** The iter-4 "Readiness: P2 unmet" claim is retracted.
- **Confidence:** unchanged at **88% (plateau)** — the binding cap was always **Risk** (device-only two-engine handover), not P2. Correcting P2 removes a secondary Readiness item but does not lift the Risk plateau. Only the Stage 3 device run closes it.
- **Answers folded:** Q4.1 → known-limitation-pass (Task 5 Step 3 amended: handover defect no longer blocks the stage). Q4.2 → drop the grep (moot; P2 met). Q4.3 → keep rotation simple, acceptance keys on `onSurfaceChanged` firing. Q4.4 → ~5 min soak sufficient. Q4.5 → Task 12 folds "POI implemented / distinct-fresh-POI acceptance" into the sibling spec body (both docs' stub framing is obsolete).
- **Follow-up for Task 12:** the sibling design spec `docs/superpowers/specs/2026-07-07-stage3-wallpaper-service-design.md` still describes POI as a stub in its body/status line/steps 5 & 7 and its Q3.3 override note says the scanner "lands before Stage 3 executes" (future tense) — both are now factually stale; rewrite to present-tense "implemented".
