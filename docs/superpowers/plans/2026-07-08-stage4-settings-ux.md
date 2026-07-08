# Stage 4 — Settings & Content UX Implementation Plan

**Plan-confidence status:** Ready for execution (iteration 9, confidence 90%). All spec decisions (a)–(k) folded; the 6 plan-layer survey items (iter 8) resolved interactively and folded (iter 9). The two concrete defects are now FIXED: Task 5 Step 6 `onEngineVisible()` lands as the first statement inside the `if (visible) {` block, not after `:311` (Q8.1); Task 2 Step 4 replace range corrected to `:136-145` (the `:146` `onReceive` brace excluded). Edge cases documented: manual broadcasts are an intentional out-of-band rotation override (Q8.2), import name-collision overwrite-and-refresh is acceptable (Q8.3); brightness device-verify gains a greppable Java `SETTINGS_CHANGED…brightness=` log assertion (Q8.4). Q8.5/Q8.6 dismissed by decision (rescan is cheap; the code snippet already specifies reset-on-resume cadence). Anchors re-verified against the tree (HEAD `cbb87ebf5a`; only an unrelated `viewport.cpp` commit past `bd4a687386`, no plan file affected). Held at 90% by the inherent ceiling shared with the accepted Stage 3 plan — no Java/JNI unit-test harness (pre-impl grep/build substitutes for a failing test) + normal un-executed-plan risk, neither closable by document edits.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish wiring the half-built user-facing configuration in the ported Android app — brightness end-to-end hardening, a map-rotation *interval* selector + cadence consumer, dropping the dead *zoom* preference, and making `MapsActivity` import/delete propagate into a running wallpaper — via Java completion plus a small additive native surface.

**Architecture:** Mostly Java completion in the default-process settings UIs (`WallpaperSettingsActivity`, `MapsActivity`, `MainActivity`, `SettingsHelper`) and the `:wallpaper` service (`OpenTTDWallpaperService`). Two additive native JNI exports (`nativeSetIntervalActive`, `nativeRefreshTitleMaps`) in `src/video/sdl2_gles_v.cpp`, each backed by an atomic; the refresh atomic is drained on the GL thread in `ProcessOverlayActions()` under `game_state_mutex` (the device-validated pattern), the interval-active atomic is a level gate read by `RequestNextTitleMap()` in `src/wallpaper.cpp`. Cross-process wiring stays `SharedPreferences` (source of truth) + broadcasts (live signal); native is reachable only from `:wallpaper`/`:game`.

**Tech Stack:** Java (Android app + `:wallpaper` WallpaperService), C++ (OpenTTD engine + `src/video/sdl2_gles_v.cpp` GLES driver, `src/wallpaper.{cpp,h}`), Gradle/AGP 8.11.1, Android XML resources.

## Global Constraints

- **Native build gate:** `JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug` → `BUILD SUCCESSFUL`.
- **Java-only build gate:** same command with `-PskipNativeBuild` appended (skips the C++ build for pure-Java iterations).
- **Python:** always `/usr/bin/python3` (homebrew python blocked by sandbox).
- **Reimpl principles (binding):** no dead scaffolding; additive edits in owned files; `#ifdef WALLPAPER_BUILD` / `#ifdef __ANDROID__` guards on any upstream-file touch; do NOT delete upstream code; one commit per logical unit.
- **Threading rule (load-bearing):** new native title-map work MUST route through the GL-thread atomic-drain in `ProcessOverlayActions()` (`src/video/sdl2_gles_v.cpp:454-489`) under `game_state_mutex` (`:467`) — exactly like `nativeRotateMap` (`:120-124` → drain `:475-476`). Do NOT call `BuildTitleFileList()`/`RotateTitleMap()` directly on the JNI binder thread the way `nativeSwitchMap` does (`:108-112`, a known latent race).
- **1:1 JNI invariant:** every `private static native` in a Java class has exactly one matching `Java_org_openttd_android_<Class>_native…` export in `sdl2_gles_v.cpp`. Baseline is 13 (service 8/8, `GameActivity` 5/5); this stage adds 2 service natives → **15/15** (service 10/10, `GameActivity` 5/5). Any mismatch is fixed in `sdl2_gles_v.cpp`, never by stubbing Java.
- **Process boundaries (`AndroidManifest.xml`):** `GameActivity` → `:game`; `OpenTTDWallpaperService` → `:wallpaper`; `MainActivity`/`WallpaperSettingsActivity`/`MapsActivity` → default process. Native loads only in `:wallpaper`/`:game`; settings/maps UIs reach native only via prefs + broadcasts. `filesDir/title/` is shared across all processes.
- **Stage 3 is DONE + device-verified** (`lwp2` tip `bd4a687386`; all 9 gates PASS on Pixel 10 Pro): the live `OpenTTDWallpaperService` runtime is now exercisable, so every task in this plan is fully device-verifiable now — the "Stage-3-blocked" deferral in decision (k) is resolved. Each task keeps a cheap **static/build interim check** (fast pre-device gate) plus a **live device-verify** step that is runnable today against the set live wallpaper.
- **Logcat tags:** native `Debug(...)` → tag `OpenTTD`; Java `Log.i(TAG,...)` → tag `OpenTTDWallpaper`. Device steps that need Java lifecycle logs use `adb logcat -s OpenTTD:V OpenTTDWallpaper:V`. Brightness has no native log line (the `SETTINGS_CHANGED` path is Java-only) — verify it visually.
- **Device caveat (from the Stage 3 gate):** reinstalling the APK de-selects the live wallpaper (Android invalidates the component). After every `deploy`/install, re-set it via `WallpaperSettingsActivity` → "Set Wallpaper" before running live checks.
- **Interval index semantics (fixed by existing strings, `strings.xml:12-16`):** `0 = every home-screen switch`, `1 = 10 min`, `2 = 30 min` (current default), `3 = 2 h`, `4 = 24 h`.

---

## File Structure

| File | Responsibility this stage |
|---|---|
| `android/app/src/main/res/layout/activity_wallpaper_settings.xml` | `seekbar_brightness` `max` 99→100; add an interval selector row. |
| `android/app/src/main/java/org/openttd/android/WallpaperSettingsActivity.java` | Wire the interval selector to `SettingsHelper.get/setMapUpdateInterval`; refresh it in `refreshAll()`. |
| `android/app/src/main/java/org/openttd/android/GameActivity.java` | Replace single-shot 500 ms brightness push with a bounded idempotent retry. |
| `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` | Bounded brightness retry (`onSurfaceCreated`); drop zoom read; interval Handler + cold-start prefs seed; `TITLE_MAPS_CHANGED` receiver → `nativeRefreshTitleMaps`; 2 new native decls. |
| `android/app/src/main/java/org/openttd/android/SettingsHelper.java` | Drop the zoom pref (keys/defaults/getters/setters/`ZOOM_VALUES`/broadcast extra); add `ACTION_TITLE_MAPS_CHANGED`. |
| `android/app/src/main/res/values/strings.xml` | Remove `zoom_*` + `map_zoom_title` strings. |
| `android/app/src/main/java/org/openttd/android/MapsActivity.java` | `.sav` enforcement; send `TITLE_MAPS_CHANGED` after import/delete. |
| `android/app/src/main/java/org/openttd/android/MainActivity.java` | Copy-once for bundled title maps (skip existing dest). |
| `src/video/sdl2_gles_v.cpp` | Add `_gles_interval_active` + `_gles_refresh_title_maps` atomics, `nativeSetIntervalActive`/`nativeRefreshTitleMaps` JNI exports; drain the refresh atomic in `ProcessOverlayActions`. |
| `src/wallpaper.{cpp,h}` | Gate `RequestNextTitleMap()` on `_gles_interval_active`; add `RefreshTitleMaps()` (rescan + clamp index) called from the drain. |
| `src/video/gles_poi.cpp` | **NO CHANGE** — the POI-wrap `RequestNextTitleMap()` call (`:549-553`) no-ops via the gated callee. |

---

## Task 1: Brightness — seekbar range fix + bounded retry (§1)

**Tag:** Device-verifiable now (Stage 3 landed). Interim: `max=100` (static/`GameActivity`) + the `GameActivity` bounded retry via the `:game` path. Live: the `:wallpaper` `onSurfaceCreated` retry against the running wallpaper.

**Files:**
- Modify: `android/app/src/main/res/layout/activity_wallpaper_settings.xml:78`
- Modify: `android/app/src/main/java/org/openttd/android/GameActivity.java:58-66`
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java:63` (add constants), `:266-267` (replace push)

**Interfaces:**
- Consumes: `SettingsHelper.getBrightness(Context)` (returns 0–100 int, default 100); JNI `nativeSetBrightness(float)` (idempotent; no-ops while `GLESBackend::Get()==nullptr`, `sdl2_gles_v.cpp:158,183`).
- Produces: nothing later tasks consume.

**Context:** `seekbar_brightness android:max="99"` (`:78`) caps the slider at 0.99 and clamps the default 100 down to 99. Both push sites fire once at 500 ms — a comment-only "retry". Replace with an idempotent re-post loop (≈250 ms × 8 ≈ 2 s); the JNI null-check drops early pushes and accepts the value once the backend exists.

- [ ] **Step 1: Confirm the current warts (pre-impl check)**

```bash
cd /Users/ilyapomaskin/work/OpenTTD
grep -n 'android:max' android/app/src/main/res/layout/activity_wallpaper_settings.xml
grep -n 'postDelayed' android/app/src/main/java/org/openttd/android/GameActivity.java
grep -n 'postDelayed(() -> pushBrightness' android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected (bug present): `android:max="99"`; a single `postDelayed(…, 500)` in `GameActivity`; the single-shot `postDelayed(() -> pushBrightness(), 500)` in the service.

- [ ] **Step 2: Fix the SeekBar range**

In `activity_wallpaper_settings.xml`, change line 78:
```xml
                    android:max="100" />
```

- [ ] **Step 3: Replace `GameActivity.pushBrightnessDelayed` with a bounded retry**

Replace `GameActivity.java:58-66` with:
```java
    private static final int BRIGHTNESS_RETRY_MS = 250;
    private static final int BRIGHTNESS_RETRY_MAX = 8; // ~2s window

    private void pushBrightnessDelayed() {
        pushBrightnessRetry(0);
    }

    // Idempotent re-push: the JNI null-check drops pushes until GLESBackend
    // exists, so re-posting for ~2s lets the value stick once the renderer is up.
    private void pushBrightnessRetry(int attempt) {
        int value = SettingsHelper.getBrightness(getApplicationContext());
        nativeSetBrightness(value / 100.0f);
        if (attempt + 1 >= BRIGHTNESS_RETRY_MAX) return;
        new android.os.Handler(android.os.Looper.getMainLooper())
            .postDelayed(() -> pushBrightnessRetry(attempt + 1), BRIGHTNESS_RETRY_MS);
    }
```

- [ ] **Step 4: Replace the service `onSurfaceCreated` single-shot push with a bounded retry**

Add the two constants to `OpenTTDWallpaperService`. Immediately after the field `private int mLastBrightness = SettingsHelper.DEFAULT_BRIGHTNESS;` (`:63`), insert:
```java
    private static final int BRIGHTNESS_RETRY_MS = 250;
    private static final int BRIGHTNESS_RETRY_MAX = 8; // ~2s window
```
Add the retry helper next to the existing `pushBrightness` overloads (after `:161`):
```java
    private void pushBrightnessRetry(int attempt) {
        pushBrightness();
        if (attempt + 1 >= BRIGHTNESS_RETRY_MAX) return;
        new android.os.Handler(android.os.Looper.getMainLooper())
            .postDelayed(() -> pushBrightnessRetry(attempt + 1), BRIGHTNESS_RETRY_MS);
    }
```
Replace the `onSurfaceCreated` push (`:266-267`):
```java
            pushBrightnessRetry(0);
```

- [ ] **Step 5: Build (native gate — unchanged JNI surface)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 6: Interim check — `max=100` + GameActivity brightness on `:game`**

```bash
grep -n 'android:max="100"' android/app/src/main/res/layout/activity_wallpaper_settings.xml
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.GameActivity
sleep 4
adb logcat -d -s OpenTTD:V | grep -iE 'brightness|SetBrightness'
```
Expected: `max="100"` present; the `:game` path renders and brightness applies (no crash). Slider default renders 100% (visual on `WallpaperSettingsActivity`).

- [ ] **Step 7: Live device-verify (running wallpaper)**

```bash
/usr/bin/python3 tools/run_android.py deploy
# re-set the live wallpaper (install de-selects it): WallpaperSettingsActivity -> Set Wallpaper -> confirm -> home
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
# drag the brightness slider 100 -> 0 -> 100
```
Acceptance (visual + one greppable Java signal — decided Q8.4): the wallpaper dims live; 0 ≈ black, 100 = full (not capped at 0.99 — the `max=100` fix). Brightness has no native log line, so assert the Java receiver's signal instead: `adb logcat -d -s OpenTTDWallpaper:V | grep 'SETTINGS_CHANGED.*brightness='` shows the new value on each slider change. Then force-stop and re-set the wallpaper:
```bash
adb shell am force-stop org.openttd.android
# re-set the live wallpaper, then observe WITHOUT touching the slider
```
Acceptance: brightness is restored from prefs on the fresh surface (the `onSurfaceCreated` bounded retry lands once the backend is ready).

- [ ] **Step 8: Commit**

```bash
git add android/app/src/main/res/layout/activity_wallpaper_settings.xml \
  android/app/src/main/java/org/openttd/android/GameActivity.java \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "android: fix brightness seekbar range (max=100) + bounded idempotent brightness retry"
```

---

## Task 2: Drop the zoom preference (§3, decision a)

**Tag:** Checkable now (Java-only; build + grep).

**Files:**
- Modify: `android/app/src/main/java/org/openttd/android/SettingsHelper.java` (remove `:11`, `:15`, `:20`, `:35-42`, `:56`)
- Modify: `android/app/src/main/res/values/strings.xml` (remove `:7`, `:17-19`)
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java:136-145` (remove zoom read — NOT `:146`, which is the `onReceive` closing brace)

**Interfaces:**
- Consumes: nothing new.
- Produces: `SettingsHelper.notifySettingsChanged` now broadcasts only `KEY_MAP_INTERVAL` + `KEY_BRIGHTNESS`; `KEY_MAP_ZOOM`/`DEFAULT_MAP_ZOOM`/`ZOOM_VALUES`/`getMapZoom`/`setMapZoom` no longer exist (later tasks must not reference them).

**Context:** The zoom pref has no UI, no consumer, and its only ex-consumer `nativeCycleZoom` is absent (empty `onTouchEvent`, `OpenTTDWallpaperService.java:225-227`). POI zoom is a per-POI native choice materialized by the Stage-2 scanner. Removing the pref satisfies "no dead scaffolding". No native change.

- [ ] **Step 1: Confirm zoom symbols exist today (pre-impl check)**

```bash
cd /Users/ilyapomaskin/work/OpenTTD
grep -rn 'MAP_ZOOM\|ZOOM_VALUES\|getMapZoom\|setMapZoom\|zoom_1x\|map_zoom_title\|KEY_MAP_ZOOM' \
  android/app/src/main/java/org/openttd/android/ android/app/src/main/res/values/strings.xml
```
Expected: hits in `SettingsHelper.java`, `strings.xml`, and the service receiver (`OpenTTDWallpaperService.java:138-143`).

- [ ] **Step 2: Remove zoom from `SettingsHelper.java`**

Delete the constant `public static final String KEY_MAP_ZOOM = "map_zoom";` (`:11`), `public static final int DEFAULT_MAP_ZOOM = 1;` (`:15`), and `public static final int[] ZOOM_VALUES = {1, 2, 4};` (`:20`). Delete the two methods (`:35-42`):
```java
    public static int getMapZoom(Context context) {
        return getPrefs(context).getInt(KEY_MAP_ZOOM, DEFAULT_MAP_ZOOM);
    }

    public static void setMapZoom(Context context, int value) {
        getPrefs(context).edit().putInt(KEY_MAP_ZOOM, value).apply();
        notifySettingsChanged(context);
    }
```
In `notifySettingsChanged`, delete the zoom extra line (`:56`):
```java
        intent.putExtra(KEY_MAP_ZOOM, getMapZoom(context));
```

- [ ] **Step 3: Remove zoom strings from `strings.xml`**

Delete `<string name="map_zoom_title">Map zoom</string>` (`:7`) and the three zoom value strings (`:17-19`):
```xml
  <string name="zoom_1x">1x</string>
  <string name="zoom_2x">2x</string>
  <string name="zoom_4x">4x</string>
```

- [ ] **Step 4: Remove the zoom read from the service `SETTINGS_CHANGED` receiver**

Replace `OpenTTDWallpaperService.java:136-145` (the statement body only — do NOT include the `onReceive` closing brace at `:146`) with:
```java
                int interval = intent.getIntExtra(SettingsHelper.KEY_MAP_INTERVAL,
                    SettingsHelper.DEFAULT_MAP_INTERVAL);
                int brightness = intent.getIntExtra(SettingsHelper.KEY_BRIGHTNESS,
                    SettingsHelper.DEFAULT_BRIGHTNESS);
                Log.i(TAG, "SETTINGS_CHANGED: interval=" + interval
                    + " brightness=" + brightness);
                mLastBrightness = brightness;
                pushBrightness(brightness);
```
(`interval` stays read+logged here; Task 5 adds its consumer.)

- [ ] **Step 5: Verify zoom is fully gone (post-impl check)**

```bash
grep -rn 'MAP_ZOOM\|ZOOM_VALUES\|getMapZoom\|setMapZoom\|zoom_1x\|zoom_2x\|zoom_4x\|map_zoom_title' \
  android/app/src/main/java/org/openttd/android/ android/app/src/main/res/values/strings.xml
```
Expected: no output.

- [ ] **Step 6: Build (Java-only)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug -PskipNativeBuild
```
Expected: `BUILD SUCCESSFUL` (no unresolved `R.string.zoom_*` / `map_zoom_title` references).

- [ ] **Step 7: Commit**

```bash
git add android/app/src/main/java/org/openttd/android/SettingsHelper.java \
  android/app/src/main/res/values/strings.xml \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "android: drop unused map-zoom preference (no UI, no native consumer)"
```

---

## Task 3: Interval selector UI (§2 UI)

**Tag:** Checkable now (Java-only; UI renders, selection persists, broadcast fires).

**Files:**
- Modify: `android/app/src/main/res/layout/activity_wallpaper_settings.xml` (insert interval row after the brightness block, `:79`)
- Modify: `android/app/src/main/java/org/openttd/android/WallpaperSettingsActivity.java`

**Interfaces:**
- Consumes: `SettingsHelper.getMapUpdateInterval(Context)` / `setMapUpdateInterval(Context,int)` (already exist, `:26-33`); the five `interval_*` strings (`strings.xml:12-16`).
- Produces: writing an interval index fires `notifySettingsChanged` (existing `setMapUpdateInterval` behavior) — the signal Task 5's service consumer reacts to.

**Context:** A click-to-dialog row styled like the existing `row_title_maps` (a clickable `TextView`, `:81-90`). The dialog is a single-choice list of the five interval labels; the chosen list index IS the interval index (0–4). A value `TextView` shows the current selection, refreshed in `refreshAll()`.

- [ ] **Step 1: Confirm no interval UI exists today (pre-impl check)**

```bash
grep -n 'map_interval\|row_map_interval\|txt_interval_value\|setMapUpdateInterval' \
  android/app/src/main/java/org/openttd/android/WallpaperSettingsActivity.java \
  android/app/src/main/res/layout/activity_wallpaper_settings.xml
```
Expected: no output (no interval row or wiring yet).

- [ ] **Step 2: Add the interval row to the layout**

In `activity_wallpaper_settings.xml`, insert between the brightness `</LinearLayout>` (`:79`) and the `row_title_maps` `TextView` (`:81`):
```xml
            <LinearLayout
                android:id="@+id/row_map_interval"
                android:layout_width="match_parent"
                android:layout_height="wrap_content"
                android:orientation="vertical"
                android:background="?attr/selectableItemBackground"
                android:clickable="true"
                android:focusable="true"
                android:padding="16dp">
                <TextView
                    android:layout_width="wrap_content"
                    android:layout_height="wrap_content"
                    android:text="@string/map_interval_title"
                    android:textSize="16sp" />
                <TextView
                    android:id="@+id/txt_interval_value"
                    android:layout_width="wrap_content"
                    android:layout_height="wrap_content"
                    android:textSize="14sp"
                    android:textColor="?attr/colorOnSurfaceVariant" />
            </LinearLayout>
```

- [ ] **Step 3: Wire the selector in `WallpaperSettingsActivity`**

Add the import (with the existing imports, after `android.widget.SeekBar`):
```java
import android.app.AlertDialog;
import android.widget.TextView;
```
(`TextView` is already imported at `:8` — do not duplicate; add only `android.app.AlertDialog`.)

Add a field next to `txtBrightnessValue` (`:19`):
```java
    private TextView txtIntervalValue;
```
In `onCreate`, after `seekbarBrightness = findViewById(R.id.seekbar_brightness);` (`:44`):
```java
        txtIntervalValue = findViewById(R.id.txt_interval_value);
        findViewById(R.id.row_map_interval).setOnClickListener(v -> showIntervalDialog());
```
Add the helper methods (before `refreshAll()`):
```java
    private String[] intervalLabels() {
        return new String[]{
            getString(R.string.interval_every_switch),
            getString(R.string.interval_10m),
            getString(R.string.interval_30m),
            getString(R.string.interval_2h),
            getString(R.string.interval_24h),
        };
    }

    private void showIntervalDialog() {
        int current = SettingsHelper.getMapUpdateInterval(this);
        new AlertDialog.Builder(this)
            .setTitle(R.string.map_interval_title)
            .setSingleChoiceItems(intervalLabels(), current, (dialog, which) -> {
                SettingsHelper.setMapUpdateInterval(this, which);
                dialog.dismiss();
                refreshAll();
            })
            .setNegativeButton(R.string.cancel, null)
            .show();
    }
```
Extend `refreshAll()` (`:88-92`) to update the interval label:
```java
    private void refreshAll() {
        int brightness = SettingsHelper.getBrightness(this);
        seekbarBrightness.setProgress(brightness);
        txtBrightnessValue.setText(brightness + "%");

        String[] labels = intervalLabels();
        int interval = SettingsHelper.getMapUpdateInterval(this);
        int idx = (interval >= 0 && interval < labels.length)
            ? interval : SettingsHelper.DEFAULT_MAP_INTERVAL;
        txtIntervalValue.setText(labels[idx]);
    }
```

- [ ] **Step 4: Build (Java-only)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug -PskipNativeBuild
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 5: Interim check — selector persists + broadcasts**

```bash
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.WallpaperSettingsActivity
# manually: tap "Map change interval" → pick "30 minutes" → reopen the screen
adb logcat -d | grep -iE 'SETTINGS_CHANGED'
adb shell run-as org.openttd.android cat /data/data/org.openttd.android/shared_prefs/wallpaper_prefs.xml | grep map_update_interval
```
Expected: the row shows the chosen label after reopen (persists); `SETTINGS_CHANGED` broadcast logged; `map_update_interval` value matches the picked index in the prefs XML.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/res/layout/activity_wallpaper_settings.xml \
  android/app/src/main/java/org/openttd/android/WallpaperSettingsActivity.java
git commit -m "android: add map-rotation interval selector (indices 0-4) to wallpaper settings"
```

---

## Task 4: Native interval-active gate + refresh-title-maps drain + JNI (§2 gate, §4 native)

**Tag:** Build-checkable now (`BUILD SUCCESSFUL` + 15/15 grep). This task's deliverable is the native surface (no caller yet); its runtime behavior is device-verified through Tasks 5 & 7 on the now-live service.

**Files:**
- Modify: `src/video/sdl2_gles_v.cpp` (atomics after `:63`; JNI exports after `:193`; drain in `ProcessOverlayActions` `:460-489`)
- Modify: `src/wallpaper.h:17` (declare `RefreshTitleMaps`)
- Modify: `src/wallpaper.cpp` (add `<atomic>` + extern; gate `RequestNextTitleMap` `:84-87`; add `RefreshTitleMaps`)
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java:43` (2 native decls)

**Interfaces:**
- Consumes: `BuildTitleFileList()` (`wallpaper.cpp:40`), `_title_files`/`_title_file_idx` (`:33-34`), `game_state_mutex` + `ProcessOverlayActions` drain pattern (`sdl2_gles_v.cpp:454-489`).
- Produces:
  - Java: `private static native void nativeSetIntervalActive(boolean active);` and `private static native void nativeRefreshTitleMaps();` on `OpenTTDWallpaperService` (Task 5 calls `nativeSetIntervalActive`; Task 7 calls `nativeRefreshTitleMaps`).
  - C++: `void RefreshTitleMaps();` (rebuilds `_title_files`, clamps `_title_file_idx`); `_gles_interval_active` level gate suppressing `RequestNextTitleMap()`.

**Context:** `_gles_interval_active` is a **level gate** (persistent boolean), not a drained edge — its JNI setter writes it directly (mirroring `nativePrepareBackground` setting `_gles_jump_waypoint = true` at `:105`), and `RequestNextTitleMap()` reads it on the game thread (the POI-wrap-on-hide read already occurs under `game_state_mutex` via the `_gles_jump_waypoint` drain → `PrepareBackground` → `RequestNextTitleMap`). `_gles_refresh_title_maps` IS a drained edge: `nativeRefreshTitleMaps` sets it true, `ProcessOverlayActions` `exchange(false)` → `RefreshTitleMaps()` under the mutex. The gate lives in `RequestNextTitleMap` only (decision i), covering all its callers; `RotateTitleMap` (manual broadcasts + the interval Handler's `nativeRotateMap`) is intentionally NOT gated. `gles_poi.cpp` is unchanged.

- [ ] **Step 1: Confirm the natives/atomics are absent + 13-pair baseline (pre-impl check)**

```bash
cd /Users/ilyapomaskin/work/OpenTTD
grep -c 'private static native' android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java   # expect 8
grep -c 'Java_org_openttd_android_OpenTTDWallpaperService_native' src/video/sdl2_gles_v.cpp                    # expect 8
grep -n '_gles_interval_active\|_gles_refresh_title_maps\|RefreshTitleMaps\|nativeSetIntervalActive\|nativeRefreshTitleMaps' \
  src/video/sdl2_gles_v.cpp src/wallpaper.cpp src/wallpaper.h                                                  # expect no output
```
Expected: `8`, `8`, and no output for the symbol grep.

- [ ] **Step 2: Add the two atomics in `sdl2_gles_v.cpp`**

After `_gles_surface_changed` (`:63`), insert (same unguarded region as the other overlay atomics):
```cpp
/** Level gate: true while an interval index 1-4 owns title-map cadence, suppressing
 *  the POI-wrap RequestNextTitleMap(). Written directly by JNI, read on the game thread. */
std::atomic<bool> _gles_interval_active{false};
/** Set from Java on title-map import/delete; drained on the GL thread to rebuild the list. */
std::atomic<bool> _gles_refresh_title_maps{false};
```

- [ ] **Step 3: Add the two JNI exports in `sdl2_gles_v.cpp`**

Immediately after `Java_org_openttd_android_OpenTTDWallpaperService_nativeSurfaceChanged` (`:188-193`) and before `#endif /* __ANDROID__ */` (`:195`):
```cpp
extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeSetIntervalActive(JNIEnv *, jclass, jboolean active)
{
	_gles_interval_active = (active == JNI_TRUE);
	Debug(driver, 0, "[LOAD] interval_active={}", (active == JNI_TRUE) ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeRefreshTitleMaps(JNIEnv *, jclass)
{
	_gles_refresh_title_maps = true;
}
```

- [ ] **Step 4: Drain the refresh atomic in `ProcessOverlayActions`**

In the fast-path `any` check (`:460-461`), append the refresh flag:
```cpp
	bool any = _gles_jump_waypoint.load() || _gles_navigate_poi.load() != 0 ||
		_gles_rotate_map.load() != 0 || _gles_scroll_dx.load() != 0 || _gles_scroll_dy.load() != 0 ||
		_gles_refresh_title_maps.load();
```
Inside the `game_state_mutex` block, after the `_gles_rotate_map` drain (`:475-476`), add:
```cpp
	if (_gles_refresh_title_maps.exchange(false)) {
		Debug(driver, 1, "Tick: refresh_title_maps triggered");
		RefreshTitleMaps();
	}
```

- [ ] **Step 5: Declare `RefreshTitleMaps` in `wallpaper.h`**

After `void RotateTitleMap(int delta);` (`:16`):
```cpp
void RefreshTitleMaps();
```

- [ ] **Step 6: Gate `RequestNextTitleMap` + implement `RefreshTitleMaps` in `wallpaper.cpp`**

Add `<atomic>` to the includes (with the other `<...>` includes near `:25-27`):
```cpp
#include <atomic>
```
After the `_title_file_idx` declaration (`:34`), add the Android-only extern:
```cpp
#ifdef __ANDROID__
extern std::atomic<bool> _gles_interval_active;
#endif
```
Replace `RequestNextTitleMap` (`:84-87`) with:
```cpp
void RequestNextTitleMap()
{
#ifdef __ANDROID__
	/* An interval index 1-4 owns cadence; suppress the POI-wrap auto-rotate. */
	if (_gles_interval_active.load()) return;
#endif
	RotateTitleMap(1);
}
```
Add `RefreshTitleMaps` after `RotateTitleMap` (`:95`):
```cpp
/**
 * Rebuild the title file list after an import/delete and clamp the index.
 * Called from the GL-thread overlay-action drain under game_state_mutex.
 */
void RefreshTitleMaps()
{
	BuildTitleFileList();
	if (_title_file_idx >= _title_files.size()) {
		_title_file_idx = _title_files.empty() ? 0 : _title_files.size() - 1;
	}
	Debug(misc, 0, "RefreshTitleMaps: {} files, idx={}", _title_files.size(), _title_file_idx);
}
```

- [ ] **Step 7: Add the two native decls in `OpenTTDWallpaperService.java`**

After `nativeSetBrightness` (`:42-43`):
```java
    /** Enable/disable interval-driven rotation (suppresses POI-wrap auto-rotate). */
    private static native void nativeSetIntervalActive(boolean active);
    /** Rebuild the native title-file list after import/delete. */
    private static native void nativeRefreshTitleMaps();
```

- [ ] **Step 8: Native build (JNI surface changed → full build)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 9: Assert the 1:1 invariant is now 15/15 (post-impl check)**

```bash
cd /Users/ilyapomaskin/work/OpenTTD
test "$(grep -c 'private static native' android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java)" = 10 && echo OK-service-java
test "$(grep -c 'Java_org_openttd_android_OpenTTDWallpaperService_native' src/video/sdl2_gles_v.cpp)" = 10 && echo OK-service-cpp
test "$(grep -c 'private static native' android/app/src/main/java/org/openttd/android/GameActivity.java)" = 5 && echo OK-game-java
test "$(grep -c 'Java_org_openttd_android_GameActivity_native' src/video/sdl2_gles_v.cpp)" = 5 && echo OK-game-cpp
```
Expected: all four `OK-*` printed (10 + 10 service, 5 + 5 game = 15/15).

- [ ] **Step 10: Runtime behavior deferred to Tasks 5 & 7**

This task adds no caller, so it has no observable runtime behavior on its own. The gate and drain are device-verified in Task 5 Step 10 (interval-active suppresses the POI-wrap rotate) and Task 7 Step 7 (`TITLE_MAPS_CHANGED` → `RefreshTitleMaps` dump), both runnable now that Stage 3 has landed.

- [ ] **Step 11: Commit**

```bash
git add src/video/sdl2_gles_v.cpp src/wallpaper.cpp src/wallpaper.h \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "gles: add nativeSetIntervalActive gate + nativeRefreshTitleMaps drain (1:1 -> 15/15)"
```

---

## Task 5: Service interval consumer — Handler + cold-start prefs seed (§2 consumer, §5 persistence)

**Tag:** Device-verifiable now (Stage 3 landed; live-service lifecycle available). Interim: static inspection + build.

**Files:**
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` (fields; `onCreate`; `SETTINGS_CHANGED` receiver; `onDestroy`; `OpenTTDEngine.onVisibilityChanged`)

**Interfaces:**
- Consumes: `nativeSetIntervalActive(boolean)` + `nativeRotateMap(int)` (both service natives; the latter routes via `_gles_rotate_map` drain, `sdl2_gles_v.cpp:120-124`); `SettingsHelper.getMapUpdateInterval` / `getBrightness`.
- Produces: nothing later tasks consume.

**Context:** Index 0 uses no timer — the existing `onVisibilityChanged(false)` → `nativePrepareBackground()` POI-advance/wrap-rotate IS the cadence, and `nativeSetIntervalActive(false)` leaves the POI-wrap rotate live (decision f). Indices 1–4 arm a main-looper `Handler` (fires `nativeRotateMap(1)` on the period, re-posting) and set `nativeSetIntervalActive(true)` so the POI-wrap rotate no-ops (the Handler is the sole title-map authority; decisions b, b′, j). The Handler is cancelled on hide and re-armed on resume. Cold-start (decision h): on the FIRST `onVisibilityChanged(true)` — guaranteed past `sSDLInitialized` and with native ready — seed interval + brightness from prefs. The interval state lives on the OUTER service (the receiver is registered there); the inner `Engine` reports visibility via two outer helpers.

- [ ] **Step 1: Confirm interval is logged-only today (pre-impl check)**

```bash
grep -n 'applyInterval\|mIntervalRunnable\|INTERVAL_MS\|mColdStartApplied\|onEngineVisible' \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected: no output (interval read but never consumed).

- [ ] **Step 2: Add interval/cold-start fields + main Handler**

After the brightness retry constants added in Task 1 (below `:63`), add:
```java
    private static final long[] INTERVAL_MS = {
        0L,                    // index 0: no timer (POI-wrap cadence)
        10L * 60 * 1000,       // 1: 10 min
        30L * 60 * 1000,       // 2: 30 min
        2L * 60 * 60 * 1000,   // 3: 2 h
        24L * 60 * 60 * 1000,  // 4: 24 h
    };

    private android.os.Handler mMainHandler;
    private Runnable mIntervalRunnable;
    private int mCurrentInterval = SettingsHelper.DEFAULT_MAP_INTERVAL;
    private boolean mEngineVisible = false;
    private boolean mColdStartApplied = false;
```

- [ ] **Step 3: Initialize the Handler in `onCreate`**

At the top of `onCreate` (after `super.onCreate();`, `:67`):
```java
        mMainHandler = new android.os.Handler(android.os.Looper.getMainLooper());
```

- [ ] **Step 4: Add the interval control methods**

Add to the service class (near `pushBrightness`, after `:161`):
```java
    private void applyInterval(int index) {
        mCurrentInterval = index;
        boolean timed = index >= 1 && index < INTERVAL_MS.length;
        if (sLibrariesLoaded && sSDLInitialized) nativeSetIntervalActive(timed);
        cancelIntervalTimer();
        if (timed && mEngineVisible) armIntervalTimer(index);
    }

    private void armIntervalTimer(int index) {
        if (index < 1 || index >= INTERVAL_MS.length) return;
        final long period = INTERVAL_MS[index];
        cancelIntervalTimer();
        mIntervalRunnable = new Runnable() {
            @Override
            public void run() {
                Log.i(TAG, "interval tick: rotating title map");
                if (sLibrariesLoaded && sSDLInitialized) nativeRotateMap(1);
                mMainHandler.postDelayed(this, period);
            }
        };
        mMainHandler.postDelayed(mIntervalRunnable, period);
    }

    private void cancelIntervalTimer() {
        if (mIntervalRunnable != null) {
            mMainHandler.removeCallbacks(mIntervalRunnable);
            mIntervalRunnable = null;
        }
    }

    private void onEngineVisible() {
        mEngineVisible = true;
        if (!mColdStartApplied) {
            mColdStartApplied = true;
            mLastBrightness = SettingsHelper.getBrightness(getApplicationContext());
            applyInterval(SettingsHelper.getMapUpdateInterval(getApplicationContext()));
        } else if (mCurrentInterval >= 1 && mCurrentInterval < INTERVAL_MS.length) {
            armIntervalTimer(mCurrentInterval);
        }
    }

    private void onEngineHidden() {
        mEngineVisible = false;
        cancelIntervalTimer();
    }
```

- [ ] **Step 5: Consume interval in the `SETTINGS_CHANGED` receiver**

In the receiver body (after `pushBrightness(brightness);`, the last line of the Task-2 block), add:
```java
                applyInterval(interval);
```

- [ ] **Step 6: Wire visibility from the `Engine`**

In `OpenTTDEngine.onVisibilityChanged`, as the **first statement inside the `if (visible) {` block** (`~:313`, right after `if (visible) {` at `:312`), before the surface re-injection — add the cold-start/re-arm hook. **Do NOT place it after `mVisible = visible;` at `:311`:** that line precedes `if (visible) {`, so inserting there would fire `onEngineVisible()` on the hidden branch too (setting `mEngineVisible=true` + running cold-start on a hide). Native is already ready here — the `if (!sSDLInitialized) return;` guard at `:310` runs first (decided Q8.1):
```java
                OpenTTDWallpaperService.this.onEngineVisible();
```
In the `else` (hidden) block, before `nativePrepareBackground();` (`:341`), add:
```java
                OpenTTDWallpaperService.this.onEngineHidden();
```
Placing `onEngineVisible()` before the existing `pushBrightness()` (`:332`) means the cold-start read into `mLastBrightness` happens first, so the existing resume-time push uses the persisted value (reusing that hook per decision h).

- [ ] **Step 7: Cancel the timer in `onDestroy`**

In `onDestroy`, before `super.onDestroy();` (`:197`):
```java
        cancelIntervalTimer();
```

- [ ] **Step 8: Native build**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 9: Interim check — wiring present + no orphan references**

```bash
grep -n 'applyInterval\|armIntervalTimer\|cancelIntervalTimer\|onEngineVisible\|onEngineHidden\|nativeSetIntervalActive' \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected: `applyInterval` called from both the receiver and `onEngineVisible`; `onEngineVisible`/`onEngineHidden` called from `onVisibilityChanged`; `cancelIntervalTimer` in `onEngineHidden`, `applyInterval`, `armIntervalTimer`, and `onDestroy`; `nativeSetIntervalActive` called only from `applyInterval`.

- [ ] **Step 10: Live device-verify (running wallpaper)**

```bash
/usr/bin/python3 tools/run_android.py deploy
# re-set the live wallpaper, then:
adb logcat -c
# set interval to index 0 in WallpaperSettingsActivity, then cycle home->app->home a few times
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'jumping POI|interval_active|map_rotate|interval tick'
```
Acceptance (index 0): each home-screen switch advances the POI camera (`jumping POI`), the title map rotates only on POI-wrap (`map_rotate` ~every 20 switches), `interval_active=false`, and NO `interval tick` lines. Then set a short interval (index 1 = 10 min, or a temporary shortened `INTERVAL_MS[1]` for the test):
```bash
adb logcat -c
# set interval to index 1; leave the wallpaper visible
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'interval_active|interval tick|rotate_map|map_rotate'
```
Acceptance (indices 1–4): `interval_active=true`; the title map auto-rotates on the timer (`interval tick` → `Tick: rotate_map=1`) while visible, not while hidden (hide → no ticks, re-arm on resume); exactly one rotation per tick and none from the POI wrap (no `map_rotate` wrap-load). Cold start: `adb shell am force-stop org.openttd.android`, re-set the wallpaper, and confirm the saved interval is honored on first show without sending a `SETTINGS_CHANGED` broadcast.

- [ ] **Step 11: Commit**

```bash
git add android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "wallpaper: interval Handler consumer (0=POI-wrap, 1-4=timer) + cold-start prefs seed"
```

---

## Task 6: MapsActivity hardening + broadcast + copy-once (§4 Java)

**Tag:** Checkable now (Java-only). `.sav` enforcement, broadcast send, and copy-once are verifiable via static/log/file checks; the live-service consumption is Task 7.

**Files:**
- Modify: `android/app/src/main/java/org/openttd/android/SettingsHelper.java` (add `ACTION_TITLE_MAPS_CHANGED`)
- Modify: `android/app/src/main/java/org/openttd/android/MapsActivity.java` (`.sav` enforcement; send broadcast after import/delete)
- Modify: `android/app/src/main/java/org/openttd/android/MainActivity.java` (copy-once for title files)

**Interfaces:**
- Consumes: existing `MapsActivity` file ops (`onFilePicked` `:75-100`, `confirmDelete` `:102-111`); `copyAssetDir` (`MainActivity.java:108-135`).
- Produces: `SettingsHelper.ACTION_TITLE_MAPS_CHANGED` (String constant Task 7's service receiver filters on); a `sendBroadcast(TITLE_MAPS_CHANGED)` after every successful import/delete.

**Context:** `copyAssetDir` currently re-copies bundled `title/*.sav` every launch with no dest check, so deleting a bundled map does not persist. Copy-once is scoped to the `title` subtree only (via a `skipExisting` param) so `baseset`/`lang` still refresh on updates (decision d). `.sav` enforcement stops a mis-picked file from silently vanishing from the `.sav`-filtered list. **Name-collision behavior (decided Q8.3): overwrite-and-refresh is acceptable** — a SAF import whose display name matches an existing/bundled map copies to the same dest and overwrites it (per §4 / decision c); no rename, no dedupe. Intentional and testable: re-importing a same-named file replaces it and the list refreshes without duplicating.

- [ ] **Step 1: Confirm no propagation + unconditional copy today (pre-impl check)**

```bash
cd /Users/ilyapomaskin/work/OpenTTD
grep -n 'sendBroadcast\|TITLE_MAPS_CHANGED\|\.sav' android/app/src/main/java/org/openttd/android/MapsActivity.java
grep -n 'skipExisting\|destDir.exists' android/app/src/main/java/org/openttd/android/MainActivity.java
```
Expected: `MapsActivity` has no `sendBroadcast`/`TITLE_MAPS_CHANGED` and no `.sav` enforcement in `onFilePicked`; `MainActivity` has no `skipExisting` and no dest-existence check in the file branch.

- [ ] **Step 2: Add the action constant in `SettingsHelper`**

After `ACTION_SETTINGS_CHANGED` (`:18`):
```java
    public static final String ACTION_TITLE_MAPS_CHANGED = "org.openttd.android.TITLE_MAPS_CHANGED";
```

- [ ] **Step 3: `.sav` enforcement + broadcast on import**

In `MapsActivity`, add the import (with the existing imports):
```java
import android.content.Intent;
```
In `onFilePicked`, after the display-name cursor block resolves `fileName` (after `:85`), enforce the extension:
```java
            if (!fileName.toLowerCase().endsWith(".sav")) fileName = fileName + ".sav";
```
After the copy try-with-resources completes, replace the trailing `refreshList();` (`:96`) with:
```java
            refreshList();
            sendTitleMapsChanged();
```

- [ ] **Step 4: Broadcast on delete + add the helper**

In `confirmDelete` positive button (`:105-108`), after `refreshList();`:
```java
            .setPositiveButton(R.string.maps_delete, (dialog, which) -> {
                file.delete();
                refreshList();
                sendTitleMapsChanged();
            })
```
Add the helper (after `getTitleDir()`, `:132`):
```java
    private void sendTitleMapsChanged() {
        sendBroadcast(new Intent(SettingsHelper.ACTION_TITLE_MAPS_CHANGED));
    }
```

- [ ] **Step 5: Copy-once for title files in `MainActivity`**

Change the `copyAssetDir` signature (`:108`) to take `boolean skipExisting`:
```java
    private static void copyAssetDir(AssetManager assets, String srcPath, File destDir, boolean skipExisting) throws IOException {
        String[] list = assets.list(srcPath);
        if (list == null) return;

        if (list.length == 0) {
            // It's a file — copy it (skip when it already exists, for copy-once assets).
            if (skipExisting && destDir.exists()) return;
            if (!destDir.getParentFile().exists()) {
                destDir.getParentFile().mkdirs();
            }
            try (InputStream in = assets.open(srcPath);
                 OutputStream out = new FileOutputStream(destDir)) {
                byte[] buf = new byte[8192];
                int len;
                while ((len = in.read(buf)) > 0) {
                    out.write(buf, 0, len);
                }
            }
            return;
        }

        // It's a directory — recurse.
        if (!destDir.exists()) {
            destDir.mkdirs();
        }
        for (String fileName : list) {
            copyAssetDir(assets, srcPath + "/" + fileName, new File(destDir, fileName), skipExisting);
        }
    }
```
Update the three callers in `copyAssetsStatic`: `baseset` (`:80`) and `lang` (`:86`) pass `false`; `title` (`:92`) passes `true`:
```java
            copyAssetDir(assets, "baseset", new File(dataDir, "baseset"), false);
```
```java
            copyAssetDir(assets, "lang", new File(dataDir, "lang"), false);
```
```java
            copyAssetDir(assets, "title", new File(dataDir, "title"), true);
```

- [ ] **Step 6: Build (Java-only)**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug -PskipNativeBuild
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 7: Interim check — broadcast fires on import/delete + copy-once persists a delete**

```bash
/usr/bin/python3 tools/run_android.py deploy
adb logcat -c
adb shell am start -n org.openttd.android/.MapsActivity
# manually: delete a bundled map (leave >1), confirm it disappears
adb logcat -d | grep -iE 'TITLE_MAPS_CHANGED|SETTINGS_CHANGED'   # broadcast intent observable in logs
# copy-once: relaunch and confirm the deleted map does NOT reappear
adb shell am force-stop org.openttd.android
adb shell am start -n org.openttd.android/.MapsActivity
adb shell run-as org.openttd.android ls -1 files/title/
```
Expected: the deleted `.sav` is absent from `files/title/` even after a process restart (copy-once); the `TITLE_MAPS_CHANGED` broadcast is sent (visible via `am broadcast` history / no crash). The `>1` delete guard still hides the last map's delete button.

- [ ] **Step 8: Commit**

```bash
git add android/app/src/main/java/org/openttd/android/SettingsHelper.java \
  android/app/src/main/java/org/openttd/android/MapsActivity.java \
  android/app/src/main/java/org/openttd/android/MainActivity.java
git commit -m "android: propagate title-map import/delete (TITLE_MAPS_CHANGED) + .sav enforce + copy-once bundled maps"
```

---

## Task 7: Service TITLE_MAPS_CHANGED receiver → nativeRefreshTitleMaps (§4 wiring)

**Tag:** Device-verifiable now (Stage 3 landed). Interim: static inspection + build. Depends on Task 4 (`nativeRefreshTitleMaps`) and Task 6 (`ACTION_TITLE_MAPS_CHANGED` + the sender).

**Files:**
- Modify: `android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java` (field; register in `onCreate`; unregister in `onDestroy`)

**Interfaces:**
- Consumes: `SettingsHelper.ACTION_TITLE_MAPS_CHANGED` (Task 6); `nativeRefreshTitleMaps()` (Task 4) → `_gles_refresh_title_maps` drain → `RefreshTitleMaps()`.
- Produces: nothing later tasks consume.

**Context:** Registered `RECEIVER_EXPORTED` (decision g) to match every existing service receiver; the intent is app-internal so exposure is low-risk. This is the only path by which a change made in the default-process `MapsActivity` reaches the `:wallpaper` native static title cache without a process restart, via the device-validated drain (not the direct-JNI-thread path).

- [ ] **Step 1: Confirm the receiver is absent today (pre-impl check)**

```bash
grep -n 'mTitleMapsChangedReceiver\|TITLE_MAPS_CHANGED\|nativeRefreshTitleMaps' \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected: no output.

- [ ] **Step 2: Add the receiver field**

Next to the other receiver fields (after `mSettingsChangedReceiver`, `:52`):
```java
    private BroadcastReceiver mTitleMapsChangedReceiver;
```

- [ ] **Step 3: Register the receiver in `onCreate`**

After the `mSettingsChangedReceiver` registration (`:148-150`):
```java
        mTitleMapsChangedReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "TITLE_MAPS_CHANGED broadcast received");
                if (sLibrariesLoaded && sSDLInitialized) nativeRefreshTitleMaps();
            }
        };
        registerReceiver(mTitleMapsChangedReceiver,
            new IntentFilter(SettingsHelper.ACTION_TITLE_MAPS_CHANGED),
            Context.RECEIVER_EXPORTED);
```

- [ ] **Step 4: Unregister in `onDestroy`**

After the `mSettingsChangedReceiver` unregister block (`:193-196`):
```java
        if (mTitleMapsChangedReceiver != null) {
            unregisterReceiver(mTitleMapsChangedReceiver);
            mTitleMapsChangedReceiver = null;
        }
```

- [ ] **Step 5: Native build**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 6: Interim check — receiver registered + unregistered, native call gated**

```bash
grep -n 'mTitleMapsChangedReceiver\|nativeRefreshTitleMaps\|ACTION_TITLE_MAPS_CHANGED' \
  android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
```
Expected: field declared; registered with `RECEIVER_EXPORTED` on `ACTION_TITLE_MAPS_CHANGED`; `nativeRefreshTitleMaps()` guarded by `sLibrariesLoaded && sSDLInitialized`; unregistered in `onDestroy`.

- [ ] **Step 7: Live device-verify (running wallpaper)**

```bash
/usr/bin/python3 tools/run_android.py deploy
# re-set the live wallpaper, then import a .sav via MapsActivity (Add), OR fire the broadcast directly:
adb logcat -c
adb shell am broadcast -a org.openttd.android.TITLE_MAPS_CHANGED
adb logcat -d -s OpenTTD:V OpenTTDWallpaper:V | grep -nE 'TITLE_MAPS_CHANGED|RefreshTitleMaps'
```
Acceptance: `TITLE_MAPS_CHANGED broadcast received` → `RefreshTitleMaps: N files, idx=…` dump reflecting the new file count. Import a `.sav` in `MapsActivity` → it enters rotation (`SWITCH_MAP` reaches it). Delete an imported map → it leaves rotation after refresh; deleting the currently-loaded map is safe (open inode; next rotation skips the absent path).

- [ ] **Step 8: Commit**

```bash
git add android/app/src/main/java/org/openttd/android/OpenTTDWallpaperService.java
git commit -m "wallpaper: refresh native title-map cache on TITLE_MAPS_CHANGED (RECEIVER_EXPORTED)"
```

---

## Out of scope (do NOT add tasks for these)

- Any `nativeSetZoom` / global zoom bias — zoom is dropped (decision a).
- Rich map management: rename, reorder, thumbnail/preview, per-map enable toggles (decision c).
- POI scoring / zoom tuning, rotation-cadence perf instrumentation (Stage 5).
- `GameActivity` debug-overlay controls beyond the brightness retry (dev-only).
- WallpaperService on-device lifecycle itself (Stage 3).
- Any `gles_poi.cpp` edit — the suppression gate lives in `RequestNextTitleMap()` (decision i).

**Known behavior (decided Q8.2) — manual broadcasts are an intentional out-of-band override.** While an interval index 1–4 is active (`_gles_interval_active=true`), a manual `SWITCH_MAP`/`NEXT_MAP`/`PREV_MAP` broadcast still rotates: it calls the un-gated `nativeSwitchMap`/`nativeRotateMap`, and the gate deliberately covers only the automatic POI-wrap `RequestNextTitleMap()` (decision i), not user-initiated rotation. This is intended (a manual rotate should always work). The pre-existing `nativeSwitchMap` direct-JNI-thread race is left unchanged (Global Constraints threading rule) — not introduced by this stage, not in scope to fix here.

## Self-Review

- **Spec coverage:** §1 Brightness → Task 1 (`max=100`, GameActivity + service bounded retry). §2 Interval UI → Task 3; consumer → Task 5; double-fire gate/native → Task 4 (index 0 no flag, 1–4 Handler + `nativeSetIntervalActive`, gate in `RequestNextTitleMap`). §3 Zoom drop → Task 2. §4 MapsActivity propagation → Task 6 (broadcast/`.sav`/copy-once) + Task 7 (service receiver → `nativeRefreshTitleMaps` → `RefreshTitleMaps` drain). §5 Persistence → Task 5 cold-start on first `onVisibilityChanged(true)`. Native invariant 15/15 → Task 4 Step 9. Every scope item maps to a task. ✓
- **Decisions folded:** (a) zoom dropped — Task 2; (b/b′) Handler + cadence authority — Task 5; (f) index-0 reuses on-hide `nativePrepareBackground`, no timer/flag — Task 5 Step 4/6; (g) `RECEIVER_EXPORTED` — Task 7 Step 3; (h) cold-start first resume — Task 5 Step 6; (i) gate `RequestNextTitleMap` — Task 4 Step 6; (j) `nativeSetIntervalActive` atomic — Task 4; (k) full plan with layered interim+live verification — Tasks 1,4,5,7 (the Stage-3-blocking is now resolved: Stage 3 landed + device-verified at `bd4a687386`, so the live steps are runnable today); (c) minimal import UX — Task 6; (d) copy-once — Task 6 Step 5; (e) reuse ported wiring — additive edits throughout. ✓
- **Placeholder scan:** every code step carries verbatim snippets + exact line anchors; every command has expected output. No TBD/TODO. ✓
- **Type/name consistency:** Java `nativeSetIntervalActive(boolean)`/`nativeRefreshTitleMaps()` (Task 4) match the JNI exports `Java_org_openttd_android_OpenTTDWallpaperService_nativeSetIntervalActive`/`…nativeRefreshTitleMaps` (Task 4) and their callers in Tasks 5/7; `_gles_interval_active`/`_gles_refresh_title_maps` atomics (Task 4) match the `wallpaper.cpp` extern + gate (Task 4 Step 6); `RefreshTitleMaps` declared in `wallpaper.h` (Step 5), defined in `wallpaper.cpp` (Step 6), called in the drain (Step 4); `ACTION_TITLE_MAPS_CHANGED` defined in `SettingsHelper` (Task 6) matches the sender (Task 6) and the receiver filter (Task 7); `INTERVAL_MS`/`applyInterval`/`armIntervalTimer`/`cancelIntervalTimer`/`onEngineVisible`/`onEngineHidden` consistent within Task 5. ✓
- **Format note (TDD deviation):** OpenTTD's Android Java UI + JNI layer has no unit-test harness, so — like the accepted Stage 3 plan — each task substitutes a **pre-impl verification** (grep/build/GameActivity check showing the current wrong/absent state) for the failing unit test, followed by a **post-impl verification** and commit. Runtime behavior that needs the live `:wallpaper` service is split into a cheap **interim gate** (static/build) plus a **live device-verify** step; both are runnable now that Stage 3 has landed + device-verified (`bd4a687386`).

## Task tagging summary

Stage 3 is done + device-verified (`bd4a687386`), so every task is device-verifiable now; each keeps a cheap interim gate before the live check.

| Task | Scope | Interim gate (fast) | Live device-verify |
|---|---|---|---|
| 1 Brightness range + retry | §1 | `max=100` + `GameActivity` `:game` brightness | slider dims live wallpaper; brightness restored on fresh surface |
| 2 Drop zoom | §3 | build + grep (no zoom symbols) | n/a (removal; no runtime surface) |
| 3 Interval selector UI | §2 UI | selector persists + `SETTINGS_CHANGED` broadcast | n/a until consumer (Task 5) |
| 4 Native gate + refresh + JNI | §2 gate, §4 native | `BUILD SUCCESSFUL` + 15/15 grep | via Tasks 5 & 7 (no caller of its own) |
| 5 Service interval consumer + cold-start | §2 consumer, §5 | static wiring + build | index-0 POI cadence vs index-1–4 timer, double-fire suppressed, cold-start honored |
| 6 MapsActivity broadcast/.sav/copy-once | §4 Java | broadcast log + copy-once delete persists | import/delete propagates via Task 7 |
| 7 Service TITLE_MAPS_CHANGED receiver | §4 wiring | static wiring + build | `TITLE_MAPS_CHANGED` → `RefreshTitleMaps` dump; new map enters/leaves rotation |

## Confidence Survey

_No open questions._ Iteration 8's Q8.1–Q8.6 were answered interactively and folded (iteration 9); confidence 90%, Status **Ready for execution**. Re-open only if a new fork surfaces during execution.

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 8 — 2026-07-08
- **Lineage:** iterations 1–7 live in the sibling design spec (`docs/superpowers/specs/2026-07-07-stage4-settings-ux-design.md`, which reached 90% / "Ready for execution" at iter 7). This plan was authored by `writing-plans` from that spec with decisions (a)–(k) pre-folded (see Self-Review + Task tagging). This is the FIRST plan-confidence pass on the PLAN file — no prior `[x]` in this file to dissolve.
- **Confidence:** 87% (plateau) — cap from **Risk** (a few under-enumerated low-stakes edge cases: interval-tick vs. manual-broadcast rotation authority incl. the unchanged `nativeSwitchMap` JNI-thread race; import name-collision/overwrite; brightness device-verify visual-only with no native log). Secondary **Readiness** (no unit-test harness → pre-impl grep/build substitutes for a failing test — the documented, Stage-3-parity deviation — plus two mildly imprecise anchors). None of these is closable by a survey answer alone; the plateau is honest, not a hidden defect.
- **Verified against the tree (HEAD `cbb87ebf5a`, one unrelated `viewport.cpp` +17 commit past the plan's stated `bd4a687386`; `git merge-base --is-ancestor bd4a687386 HEAD` → yes; no plan-touched file changed since `bd4a687386`):**
  - JNI 1:1 baseline confirmed 8/8 service + 5/5 game (plan's 13→15/15 target math holds).
  - `sdl2_gles_v.cpp`: `_gles_surface_changed` decl `:63` ✓; fast-path `any` `:460-461` ✓; `game_state_mutex` lock `:467` ✓; `_gles_rotate_map` drain `:475-476` ✓; `nativeSurfaceChanged` block ends before `#endif /* __ANDROID__ */` `:195` ✓; `GLESBackend::Get() != nullptr` null-checks `:158`/`:183` ✓; `_gles_jump_waypoint = true` direct-set precedent `:105` ✓.
  - `wallpaper.cpp`: `_title_files` `:33`, `_title_file_idx` `:34`, `BuildTitleFileList` `:40`, `CanRotateTitleMap` `:79`, `RequestNextTitleMap` body `:84-87`, `RotateTitleMap` closes `:95`, `<...>` includes `:25-27` ✓. `wallpaper.h`: `RotateTitleMap(int)` decl `:16` ✓.
  - `gles_poi.cpp`: POI-wrap guard `:549-550` → `RequestNextTitleMap()` `:552`, `NavigatePOI` `:509`/`_poi_manual_browse=true` `:518` ✓ (NO CHANGE holds).
  - Java/XML/resources: `SettingsHelper` zoom symbols `:11/:15/:20/:35-42/:56` + `ACTION_SETTINGS_CHANGED :18` + interval getters `:26-33` ✓; `strings.xml` `map_zoom_title :7`, `interval_* :12-16`, `zoom_* :17-19` ✓; layout `max="99" :78` + brightness `</LinearLayout> :79` + `row_title_maps :81` ✓; `GameActivity.pushBrightnessDelayed :58-66` ✓; `WallpaperSettingsActivity` `TextView import :8` / `txtBrightnessValue :19` / `seekbar findViewById :44` / `refreshAll :88-92` ✓ (`AlertDialog` genuinely un-imported); `OpenTTDWallpaperService` `nativeSetBrightness :43`, `mSettingsChangedReceiver :52`, `mLastBrightness :63`, `super.onCreate :67`, SETTINGS receiver body `:136-145`, `pushBrightness` overloads close `:161`, register `:148-150`, unregister `:193-196`, `super.onDestroy :197`, `onTouchEvent :225`, `if (!sSDLInitialized) return; :310`, `mVisible = visible; :311`, `pushBrightness() :332`, `nativePrepareBackground() :341` ✓; `MapsActivity` `onFilePicked :75` / `refreshList :96` / `confirmDelete positive :105` / `getTitleDir :130` (`Intent` un-imported) ✓; `MainActivity` `copyAssetDir :108` + callers baseset `:80` / lang `:86` / title `:92` ✓.
- **Anchor imprecisions found (folded as survey items / notes, not blockers):**
  - Task 5 Step 6 — "immediately after `mVisible = visible;` (`:311`)" is one line outside the `if (visible)` scope; the co-stated "START of the `if (visible)` block" is the correct intent. A literal `:311` insert would run `onEngineVisible()` (cold-start) on the hidden branch. → Q8.1.
  - Task 2 Step 4 — replace range `:136-146` includes the `onReceive` closing brace at `:146` (the actual statements to replace are `:136-145`); an exact-range replace would drop the brace. Executor-navigable, but the range is one line too greedy. → noted here for the executor.
- **Still uncertain (why not ≥90%):** Risk — the low-stakes edge cases in Q8.2/Q8.3/Q8.5/Q8.6 (concurrency of rotation authorities, import collision, on-thread FS scan, timer reset-on-resume) are decisions the executor would otherwise make ad hoc; Readiness — no true failing-test step (documented deviation) + the two anchor imprecisions above. These do not close via document edits alone.
- **Reconciliation of the Stage-3-landed re-tag (per task):** confirmed intact — Tasks 1/5/7 carry concrete `adb`/`am broadcast`/`logcat -s OpenTTD:V OpenTTDWallpaper:V` live-verify steps + a cheap interim gate; Task 4 defers runtime to Tasks 5/7; no residual "Stage-3-blocked" language remains. Stage-3 blocking is NOT re-flagged as a risk (Stage 3 landed + 9/9 device-verified).
- **New questions:** Q8.1 … Q8.6 (all task-layer: hook placement/scope, interval-vs-manual authority, import collision, brightness observability, on-thread rescan cost, timer resume semantics).
- **No downstream skill invoked (HARD-GATE):** confidence < 90% and this run is non-interactive; survey recorded for review. Edits left uncommitted.

### Iteration 9 — 2026-07-08 (interactive fold)
- **Trigger:** Q8.1–Q8.6 answered via interactive form and folded; two concrete defects fixed, edge cases documented.
- **Confidence:** 90% (up from the 87% plateau). The subagent's iter-8 plateau assumed no survey answer could lift it, but two items WERE closable and are now closed: Q8.1 was a real correctness defect (fixed) and Q8.4 an observability gap (closed). Remaining ceiling is the inherent one shared with the accepted Stage 3 plan (no Java/JNI unit-test harness → pre-impl grep/build substitutes; normal un-executed-plan risk) — not inflated past that.
- **Resolved (Q8.1–Q8.6 folded, survey emptied):**
  - Q8.1 → **inside the `if (visible) {` block, first statement** (`~:313`) → Task 5 Step 6 rewritten with an explicit "do NOT place after `:311`" warning (removes the hidden-branch cold-start bug).
  - Q8.2 → **manual broadcasts = intentional out-of-band override** → new "Known behavior" note after Out-of-scope; no code change; the `nativeSwitchMap` JNI-thread race stays out of scope.
  - Q8.3 → **overwrite-and-refresh acceptable** → Task 6 Context gains an explicit collision note (no rename/dedupe, intentional + testable).
  - Q8.4 → **assert the existing Java `SETTINGS_CHANGED…brightness=` log** → Task 1 live device step now greps `OpenTTDWallpaper:V` for it (one greppable signal alongside the visual check).
  - Q8.5 → **drop the point** (rescan is cheap; no timing/worker change, no added note).
  - Q8.6 → **leave unspecified** (the `armIntervalTimer` snippet already implements reset-on-resume; executor is deterministic — no prose pinning added).
  - Bonus fix (same defect class as Q8.1): Task 2 Step 4 replace range corrected `:136-146` → `:136-145` at both instruction sites (the `:146` `onReceive` brace was one line too greedy).
- **Status → Ready for execution.** HARD-GATE: no downstream skill (`executing-plans`/`subagent-driven-development`) invoked — awaiting user direction. Plan + spec doc edits remain uncommitted per repo convention.
