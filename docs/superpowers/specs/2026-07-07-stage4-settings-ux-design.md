# Stage 4 — Settings & Content UX (Design Spec)

**Plan-confidence status:** Draft (iteration 2, confidence 82%) — cap from Readiness (Stages 1–3 not green; native surface unverifiable on `lwp2`).

**Goal:** Finish wiring the half-built user-facing configuration in the (already-ported) Android app: brightness end-to-end, the map-rotation **interval** preference, the **zoom** preference (decided: dropped — see §3), and `MapsActivity` title-map import/delete — including making import/delete actually take effect in a running wallpaper. Verified on-device through the live **WallpaperService** (Stage 3 must be green) with `GameActivity` as a secondary check.

**Nature:** Mostly Java completion + small additive native surface. The Java classes, layouts, strings, and `SettingsHelper` prefs ALREADY EXIST (Stage 0 ported them verbatim from `gles`) — brightness is the only setting wired end-to-end. Interval and zoom prefs exist but have **no UI element and no consumer** (confirmed identical in `gles` — they were never wired even in the reference). `MapsActivity` import/delete already work at the file level but do not propagate to the native title-file cache. Stage 4 adds: interval UI + a cadence consumer, a zoom decision, a native title-map rescan trigger, and hardening of the existing brightness path.

## Context / dependencies

- **Hard dependency:** Stages 1–3 complete and green. Stage 4 needs the Stage 1 JNI surface in `src/video/sdl2_gles_v.cpp` (`nativeSetBrightness`, `nativeRotateMap`, `nativeSwitchMap`, `nativeNavigatePOI`, `nativePrepareBackground`, `nativeSetGamePaused`, `nativeScrollCamera`, `nativeSurfaceChanged`) and the Stage 1/3 `src/wallpaper.{cpp,h}` rotation API (`BuildTitleFileList`, `RotateTitleMap`, `LoadNextTitleMap`, `CanRotateTitleMap`, `RequestNextTitleMap`), plus the Stage 3 `OpenTTDWallpaperService` running as a real live wallpaper. None of these files exist on `lwp2` yet — they arrive in Stages 1/3.
- **Stage 1 removed `nativeCycleZoom`** (plan Q1.5 / Step 2b): the `private static native void nativeCycleZoom();` declaration at `OpenTTDWallpaperService.java:31` is deleted and a 1:1 Java-native↔JNI-export assertion is enforced. There is therefore **no zoom native control** to reuse — the zoom pref is DROPPED (decision a, §3).
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
  - `index 0` ("every home-screen switch"): rotate the title map on each `onVisibilityChanged(true)` (home-screen resume) rather than only after the POI list wraps. Implement by calling `nativeRotateMap(1)` (or a dedicated advance) on resume when interval==0, and SUPPRESS the native POI-wrap auto-rotation in this mode (decided b′: the interval index owns cadence). **Caveat (Q2.1):** `onVisibilityChanged(false)` already calls `nativePrepareBackground()` (POI jump on hide) + a 150 ms delayed pause — the interval-0 resume rotation must be reconciled with this existing on-hide behavior (rotate-on-resume vs the current jump-on-hide); see Confidence Survey Q2.1.
  - `index 1–4`: a periodic timer at 10 m / 30 m / 2 h / 24 h that triggers a map rotation. **Timer location (decided b): a Java `Handler` in the service**, armed on `onVisibilityChanged(true)` / `SETTINGS_CHANGED` and cancelled on `onVisibilityChanged(false)` (don't rotate while hidden; re-arm on next resume). This keeps rotation off-screen-cheap and needs no new native. Process death re-arms on next resume.
- The interval index owns cadence (decided b′): the native POI-wrap `RequestNextTitleMap()` stays only for the "cinematic" default when no interval override is active; when an interval index 0–4 is set it becomes the single authority and the native wrap-rotation is suppressed (no double-fire).

### 3. Map zoom — DROP (decided, was open decision a)

- The zoom pref (`KEY_MAP_ZOOM`, `ZOOM_VALUES={1,2,4}`, `zoom_*` strings) has no UI and no consumer, and its only ex-consumer (`nativeCycleZoom`) was removed in Stage 1. POI zoom is chosen per-POI natively (`In4x`/`In2x`, cluster-driven).
- **Decided (a): drop the pref** — delete `KEY_MAP_ZOOM`/`DEFAULT_MAP_ZOOM`/`ZOOM_VALUES`/getters/setters/`zoom_*` strings/`map_zoom_title`, and the `zoom` extra in `notifySettingsChanged` + the `zoom` read in the service receiver. A global zoom override fights the POI scanner's own zoom-out decisions and there is no native control for it, so wiring it is net-new scope for little value. This also satisfies "no dead scaffolding." No `nativeSetZoom`, no CMake/native change.
- **Rejected alternative (keep):** would add a new `nativeSetZoom(int)` JNI + a global zoom bias in the POI camera (`ShowCurrentPOI`/`RecenterOnCurrentPOI`) + a UI selector — genuine new native feature work, not a rewire. Reopen only if a user-facing zoom knob is explicitly required.

### 4. MapsActivity — propagate import/delete to a running wallpaper + hardening

- **Import/delete file ops already work** (SAF `GetContent` picker → copy stream to `filesDir/title/<DISPLAY_NAME>`; `AlertDialog` confirm delete; `canDelete = mapFiles.size() > 1` guard). Keep them.
- **Live propagation (new, required by the verification gate):** add a `TITLE_MAPS_CHANGED` broadcast that `MapsActivity` sends after a successful import or delete. The `:wallpaper` service registers a receiver that calls a **new native `nativeRefreshTitleMaps()`** → rebuilds `_title_files` via `BuildTitleFileList()` (clamp/reset `_title_file_idx`). This is the only way a change made in the default-process `MapsActivity` reaches the native static cache in `:wallpaper` without a process restart. Add the matching JNI export in `sdl2_gles_v.cpp` (drain via the existing atomics/`ProcessOverlayActions` pattern to stay on the GL/game thread, consistent with the other controls) and keep the 1:1 Java-native↔JNI-export invariant.
- **Bundled-vs-imported delete wart:** `copyAssetsStatic` → `copyAssetDir` re-copies bundled maps every launch (verified: the file branch writes with no dest-existence check; the only `.exists()` checks are dir mkdirs), so deleting a bundled map does not persist. **Decided (d): copy-once** — make the title-file copy skip when the dest file already exists (or gate on an `assets_copied` marker pref) so deletes stick for everything. Rejected alternative: treat bundled maps as permanent defaults + hide delete on bundled names (needs bundled-vs-imported detection and confuses "delete" semantics). The "can't delete the last map" guard (`mapFiles.size() > 1`) stays; `opntitle.dat` (baseset, not in the `MapsActivity` `title/*.sav` list) is the rotation floor regardless.
- **Import validation (light hardening):** enforce/append `.sav` (the list filter already hides non-`.sav`, so a mis-picked file silently vanishes today); handle name collisions (overwrite is acceptable but should refresh + not duplicate). Deeper management (rename/reorder/preview) is out of scope — decision (c).

### 5. SETTINGS_CHANGED propagation & persistence (cross-process)

- `SettingsHelper.notifySettingsChanged` stays the single write+broadcast entry point (persist via `SharedPreferences.apply()`, then `sendBroadcast` with the extras). After the zoom decision it carries brightness + interval (+ zoom only if kept).
- The `:wallpaper` `SETTINGS_CHANGED` receiver applies **all** live-relevant settings (brightness push + interval re-arm), not just brightness. On process (re)start the service reads persisted values fresh (`SettingsHelper.getX`) — do this in `onCreate`/first `onSurfaceCreated` so a wallpaper started cold honors saved prefs without needing a broadcast.
- Do **not** rely on cross-process in-memory `SharedPreferences` consistency (`MODE_MULTI_PROCESS` is deprecated/unreliable): the broadcast is the live signal; the persisted file is the source of truth read on start.

## Known risk

- **`nativeRefreshTitleMaps` touches game/loader state:** rebuilding `_title_files` is a filesystem scan (cheap, no map mutation) but it must run on the correct thread and not race an in-flight `LoadNextTitleMap`. **Mitigation:** route it through the existing GL-thread atomic/`ProcessOverlayActions` drain like the other controls (do not call `BuildTitleFileList()` directly from the JNI thread). Deleting the currently-loaded map is safe (Android/Linux keeps the open inode; next rotation skips the now-absent path).
- **Interval timer vs POI-wrap rotation double-fire:** if both the native POI-wrap auto-rotation and the new interval timer are active, maps could rotate twice. **Resolved (b′): the interval index owns cadence** — index 0 rotates on `onVisibilityChanged(true)`, indices 1–4 rotate on the Handler tick, and BOTH suppress the native POI-wrap `RequestNextTitleMap()`. The native wrap-rotation runs only when no interval override is in effect. Requires a native switch (or Java-side gate) to disable POI-wrap auto-rotation — confirm the mechanism exists / add a small native flag (Q2.1-adjacent).
- **Delete not persisting for bundled maps** (copyAssetsStatic overwrite) — addressed in scope §4; call out in verification so the tester deletes an *imported* map (or a bundled one after the copy-once fix).

## Files

| File | Status | Change |
|---|---|---|
| `android/app/src/main/java/.../WallpaperSettingsActivity.java` | EDIT | add interval selector wiring (no zoom selector — dropped); refresh in `refreshAll()` |
| `android/app/src/main/res/layout/activity_wallpaper_settings.xml` | EDIT | add interval row/spinner (no zoom row — dropped); `seekbar_brightness` `max=100` |
| `android/app/src/main/java/.../OpenTTDWallpaperService.java` | EDIT | apply interval in `SETTINGS_CHANGED`; interval timer (Handler) arm/cancel on visibility; `TITLE_MAPS_CHANGED` receiver → `nativeRefreshTitleMaps`; bounded brightness retry; cold-start prefs read |
| `android/app/src/main/java/.../GameActivity.java` | EDIT | bounded brightness retry (replace single-shot) |
| `android/app/src/main/java/.../MapsActivity.java` | EDIT | send `TITLE_MAPS_CHANGED` after import/delete; `.sav` enforcement; (bundled-delete handling per §4) |
| `android/app/src/main/java/.../MainActivity.java` | EDIT (copy-once, decided d) | make `copyAssetsStatic`/`copyAssetDir` skip existing title files |
| `android/app/src/main/java/.../SettingsHelper.java` | EDIT | drop zoom pref (decided a): remove `KEY_MAP_ZOOM`/`DEFAULT_MAP_ZOOM`/`ZOOM_VALUES`/getters/setters + `zoom` extra; interval unchanged |
| `android/app/src/main/res/values/strings.xml` | EDIT | remove `zoom_*`/`map_zoom_title` (zoom dropped); add any new labels |
| `src/video/sdl2_gles_v.cpp` | EDIT (native) | add `nativeRefreshTitleMaps` JNI (no `nativeSetZoom` — zoom dropped); keep 1:1 Java-native↔JNI-export invariant; add matching Java decl in `OpenTTDWallpaperService` |
| `src/wallpaper.{cpp,h}` | EDIT (native) | expose a rescan entry (`BuildTitleFileList` re-run) drained via `ProcessOverlayActions`; (optional) a flag to suppress POI-wrap auto-rotation when an interval override is active |

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

## Decisions (folded from plan-confidence iteration 1)

- **(a) Zoom preference → DROPPED.** POI zoom is native (In4x/In2x); `nativeCycleZoom` removed in Stage 1; a global override has no native hook and fights the scanner. No `nativeSetZoom`. See §3.
- **(b) Interval timer → Java `Handler` in `:wallpaper`.** Armed on `onVisibilityChanged(true)`/`SETTINGS_CHANGED`, cancelled on hide; process death re-arms on next resume. No new native. See §2.
- **(b′) Cadence authority → the interval index owns cadence.** Index 0 rotates on resume; indices 1–4 on the Handler tick; both suppress the native POI-wrap `RequestNextTitleMap()`. See §2 + Known risk.
- **(c) Import UX → minimal** (SAF picker + delete + `.sav` enforcement + live refresh). Rename/reorder/preview/enable-toggle deferred (Out of scope).
- **(d) Bundled-map delete → copy-once** (skip the title-file copy when the dest already exists). See §4.
- **(e) Ported-wiring reuse — verified against the tree, not a decision.** `SettingsHelper` (prefs+broadcast), `MapsActivity` file ops, and the brightness path are reusable with additive changes only. Confirmed: `WallpaperSettingsActivity` has no interval/zoom UI (brightness seekbar only); the service `SETTINGS_CHANGED` receiver (`OpenTTDWallpaperService.java:135-149`) reads interval/zoom into locals and only `Log.i`s them while consuming brightness; `gles` `MapsActivity` has NO broadcast/native propagation → `TITLE_MAPS_CHANGED` + `nativeRefreshTitleMaps` are genuinely net-new, not a re-port.

Residual open items are tracked in the Confidence Survey below (iteration 2): interval-0 vs the existing on-hide `nativePrepareBackground` (Q2.1), the `TITLE_MAPS_CHANGED` receiver export mode + cross-process reach (Q2.2), the cold-start prefs read site (Q2.3), and the Stage-1/3-green readiness gate before this spec can become an implementation plan (Q2.4).

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled `*(Recommended)*` is the skill's best guess given current plan + repo context — override freely. (Run non-interactively: recommendations are self-assessed; residual items below drive the confidence gap.)

### Iteration 2 — 2026-07-07

#### Q2.1. `onVisibilityChanged(false)` today calls `nativePrepareBackground()` (POI jump) + a 150 ms delayed `nativeSetGamePaused(true)`; there is no rotation on `onVisibilityChanged(true)`. Interval-0 ("every home-screen switch") wants a *map rotation* tied to the resume/hide cycle. Where should the interval-0 rotation fire relative to the existing on-hide POI jump?
- [ ] Rotate on `onVisibilityChanged(true)` (resume): keep the existing on-hide POI-jump warm-up untouched; add `nativeRotateMap(1)` on resume when interval==0  *(Recommended)*
- [ ] Rotate on `onVisibilityChanged(false)` (hide) instead, replacing/augmenting the existing `nativePrepareBackground()` warm-up so the new map is warm before pause
- [ ] Rotate on resume AND drop the on-hide POI jump entirely when interval==0 (they conflict)
- [ ] Defer — leave interval-0 as a no-op alias for the native POI-wrap default until on-device behavior is observed

#### Q2.2. `MapsActivity` runs in the default process and sends `TITLE_MAPS_CHANGED`; the `:wallpaper` service consumes it. Existing receivers register with `RECEIVER_EXPORTED`. How should the new receiver be registered, given it only needs same-app delivery?
- [ ] `RECEIVER_NOT_EXPORTED` (same-app only) for the new receiver — more secure; verify delivery still works cross-process within the app  *(Recommended)*
- [ ] `RECEIVER_EXPORTED` to match every existing receiver (consistency over least-privilege)
- [ ] Use a `LocalBroadcastManager`-style / explicit-component intent instead of a system broadcast
- [ ] Skip the broadcast; rely on the service re-reading the title dir on next `onVisibilityChanged(true)` (no live refresh while visible)

#### Q2.3. §5 says the cold-started service reads persisted prefs in "`onCreate`/first `onSurfaceCreated`". Which is the single canonical read site so a wallpaper started cold honors saved interval/brightness without a broadcast?
- [ ] First `onSurfaceCreated` (after `sSDLInitialized`), where native is guaranteed loaded — read interval (arm Handler) + brightness (push)  *(Recommended)*
- [ ] `onCreate` (service create) — earliest, but native/GL not ready so brightness push would no-op and interval Handler would arm before a surface exists
- [ ] Both: read interval in `onCreate` (arm on first visibility), push brightness in `onSurfaceCreated`
- [ ] `onVisibilityChanged(true)` only — rely on the first resume to seed everything

#### Q2.4. This spec hard-depends on Stages 1–3 (none of `src/wallpaper.{cpp,h}`, `src/video/sdl2_gles_v.cpp`, or the live `OpenTTDWallpaperService` runtime exist on `lwp2` yet). How should that gate the transition to an implementation plan?
- [ ] Treat Stage-1/3-green as an explicit precondition: keep this as a design spec, write the implementation plan only after Stages 1–3 land (native API names verified against real code, not projected from `gles`)  *(Recommended)*
- [ ] Write the Java-only portions (brightness fixes, interval UI, `TITLE_MAPS_CHANGED` send, copy-once) into an impl plan now (buildable with `-PskipNativeBuild`); defer the native `nativeRefreshTitleMaps` task behind the Stage-1/3 gate
- [ ] Write the full implementation plan now against the projected `gles` native surface; fix drift during execution
- [ ] Fold this spec into the Stage 3 plan so the service lifecycle and its settings/maps consumers land together

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-07
- **Confidence:** 72% (cap from Unknowns/open-questions — five explicit open decisions (a)–(e), and §3 literally read "resolve and wire OR drop" = a "decide between X and Y" universal ≤70% trigger).
- **Resolved:** none (first pass).
- **Verified against the current `lwp2` tree + `gles` reference (all spec "current state" claims true):**
  - SeekBar `android:max="99"` while `DEFAULT_BRIGHTNESS=100` and `pushBrightness` ÷100 → default clamps to 99, cap at 0.99 brightness (`activity_wallpaper_settings.xml:78`). ✓
  - `SETTINGS_CHANGED` receiver reads interval+zoom into locals, `Log.i`s them, consumes only brightness (`OpenTTDWallpaperService.java:135-149`); interval/zoom have no UI and no consumer. ✓
  - 500 ms delayed brightness push in BOTH consumers (service `onSurfaceCreated:267-268`; `GameActivity.pushBrightnessDelayed:58-66` — single-shot, comment says "retry" but fires once, no loop). ✓ `pushBrightness` guarded by `sLibrariesLoaded && sSDLInitialized` (service:160); GameActivity's path unguarded (relies on the JNI null-check). Brightness re-pushed on `onVisibilityChanged(true)` (service:332). ✓
  - `nativeCycleZoom` present (`OpenTTDWallpaperService.java:31`) with no caller (`onTouchEvent` empty, "Zoom cycling on tap disabled"); Stage 1 removes the decl. ✓
  - `gles src/wallpaper.cpp`: `static std::vector<...> _title_files` (:32), `static size_t _title_file_idx` (:33); `BuildTitleFileList` clears+rescans (:41); `LoadNextTitleMap` rebuilds only if empty (:103); `RotateTitleMap` just modulo-advances, no rebuild (:88-92); `RequestNextTitleMap = RotateTitleMap(1)` (:85). Static-cache claim ✓ — import/delete does not reach a running wallpaper without a rebuild.
  - `MainActivity.copyAssetDir` file branch writes with NO dest-existence check (only dir mkdirs at :114/:129) → bundled maps re-copied every launch (:92). Bundled `.sav`: `Titlegame14.sav`, `g.sav`, `title13.sav`; `opntitle.dat` in `assets/baseset/`. ✓
  - `gles MapsActivity` has only `refreshList()` — no `sendBroadcast`/native/`TITLE_MAPS` → live propagation is genuinely net-new. ✓
- **Gap I found (not in the spec):** `onVisibilityChanged(false)` already calls `nativePrepareBackground()` (POI jump) + 150 ms delayed pause (`:341-347`); the interval-0 "rotate on resume" design does not reconcile with this existing on-hide behavior → Q2.1.
- **New questions:** Q1.1 (zoom drop/keep) … Q1.8.

### Iteration 2 — 2026-07-07
- **Confidence:** 82% (cap from Readiness). Universal "decide X or Y" ≤70% trigger cleared by folding (a)–(d) into the body; residual cap is Readiness, which no survey answer can lift.
- **Resolved (Q1.1–Q1.8 folded into the body, dissolved from the survey):**
  - Q1.1 → **drop zoom** → §3 rewritten to "DROP (decided)"; Files table (`SettingsHelper`/`strings.xml`/`sdl2_gles_v.cpp`/`wallpaper.{cpp,h}`) drop the "iff kept" clauses.
  - Q1.2 → **Java `Handler` in `:wallpaper`** → §2 interval timer marked decided (b).
  - Q1.3 → **interval index owns cadence** → §2 index-0/1–4 + Known-risk double-fire marked resolved (b′): interval overrides suppress native POI-wrap rotation.
  - Q1.4 → **copy-once** → §4 bundled-delete marked decided (d); Files table `MainActivity` row EDIT active.
  - Q1.5 → **minimal import UX** → §Decisions (c); Out-of-scope unchanged.
  - Q1.6 → **route `nativeRefreshTitleMaps` through the GL-thread `ProcessOverlayActions` atomic drain** (not a direct JNI-thread `BuildTitleFileList` call) → §4 + Known risk already state this; confirmed as the decided mechanism.
  - Q1.7 → **bounded re-post retry + idempotent re-push** for brightness (not a native is-ready query) → §1 already specifies bounded retry; confirmed.
  - Q1.8 → **`seekbar_brightness max="100"`** (not scale-by-max) → §1 already specifies; confirmed against `:78`.
- **Honesty note on the Readiness cap:** this is a tech-spec whose next step is an implementation plan. Two engineers could now write *similar* Java-side plans, but the native surface (`nativeRefreshTitleMaps`, `ProcessOverlayActions`, `BuildTitleFileList` rescan, POI-wrap suppression flag) is projected from `gles` and cannot be verified against real `lwp2` code because Stages 1–3 have not landed (`src/wallpaper.{cpp,h}` and `src/video/sdl2_gles_v.cpp` do not exist on `lwp2`; the live `OpenTTDWallpaperService` runtime is Stage 3). The verification gate (live WallpaperService) is likewise unrunnable until then. This is a genuine external ceiling — held at 82%, not raised to 90%.
- **Still uncertain (drives the gap):** Readiness (Stage-1/3-green dependency, Q2.4) primarily; plus one load-bearing behavior reconciliation (Q2.1) and two low-stakes wiring confirmations (Q2.2 receiver export mode, Q2.3 cold-start read site).
- **New questions:** Q2.1 … Q2.4.
- **No downstream skill invoked (HARD-GATE): confidence < 90% and this run is non-interactive; survey recorded for review.**
