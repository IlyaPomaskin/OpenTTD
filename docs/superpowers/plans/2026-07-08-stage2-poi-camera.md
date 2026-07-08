# Stage 2: POI Camera Scanner Implementation Plan

**Plan-confidence status:** Ready for execution (iteration 2, confidence 90% — Risk-capped: real-scanner behaviour is verifiable only on the Stage 2 device gate). Independently re-verified file-based against lwp2 HEAD `634dd21703` + read-only `gles` reference — every file:line, drift symbol, and the transitive-`VehicleID` reasoning confirmed; see Reconciliation Log iteration 2.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In wallpaper / GameActivity mode the camera visits scored map locations (stations, rail-junction clusters, towns, lighthouses) and cycles through them, replacing the Stage 1 stub's fixed geometric map-center. Verified on a physical arm64 device through GameActivity (the live WallpaperService path is Stage 3).

**Architecture:** A file-body swap. Replace the Stage 1 stub body of `src/video/gles_poi.cpp` (`gles_poi.cpp:20-31`) with the 648-line real POI scanner from read-only reference branch `gles`, dropping all vehicle-follow code; prune the now-dead `follow_vehicle` field + `../vehicle_type.h` include from `src/video/gles_poi.h`. This is a **selective re-port** (verbatim minus vehicle code, plus scoped-enum drift fixes). Zero renderer / driver / CMake changes — all wiring shipped and was device-verified in Stage 1 (`634dd21703`).

**Tech Stack:** C++20; the Stage 1 GLES snapshot pipeline; Android gradle APK; `adb` broadcasts + `tools/run_android.py` for the device gate.

## Global Constraints

Copied verbatim from the design spec (`docs/superpowers/specs/2026-07-07-stage2-poi-camera-design.md`) and `CLAUDE.md`. Every task's requirements implicitly include this section.

- **Scope is exactly two files:** `src/video/gles_poi.cpp` (REPLACE stub body → real scanner, no vehicle code) and `src/video/gles_poi.h` (drop `follow_vehicle` field + `../vehicle_type.h` include). **No CMake change** — both files were registered in Stage 1.
- **Scoped enums (drift):** use `GameMode::Wallpaper`, `SwitchMode::Wallpaper`, `SwitchMode::None` — the exact forms in-tree (`openttd.h:23/43/28`). The reference's `GM_WALLPAPER` / `SM_WALLPAPER` / `SM_NONE` tokens are all **stale**; none may appear in the ported file.
- **Locking (Q1.3/Q2.3 — ratified): port as-is, no mutex reliance.** The scanner ports verbatim with no locking logic of its own. Stage 2 adds no guard and does NOT treat the incidental `game_state_mutex` in `ProcessOverlayActions` (`sdl2_gles_v.cpp:467`) as its correctness guarantee. The residual scan/mutation race is ACCEPTED for this stage. Only revisit (explicitly guard `ScanMapPOIs`) if the race manifests on the device gate or a new bypassing scan path appears.
- **Camera (Q1.4 — ratified): keep the `vp.follow_vehicle = VehicleID::Invalid();` reset** in `ShowCurrentPOI` → camera always static on the POI. Drop only the commented-out follow *branch*, never this reset line. (`ViewportData::follow_vehicle` is an upstream field in `window_gui.h`, unrelated to the dropped `GlesPOI` field.)
- **RNG (Q1.5 — ratified): keep the reference `std::rand()` Fisher-Yates as-is** — a non-deterministic top-pool subset is acceptable; no reseeding.
- **Verification signal (Q1.6 — ratified): reuse `Debug(driver, …)` logcat** (candidate-count breakdown + per-POI score/reason) as the primary signal — no new instrumentation this stage.
- **Recording coords are pointer-math on lwp2** (explicit `bp.sprite_x/sprite_y` were reverted in `634dd21703`; `BlitterParams` keeps only `sprite_id`/`pal`). POI code does not consume these, but the port must introduce **no** `sprite_x`/`sprite_y` / explicit-coords assumption.
- **Reimpl principles (`docs/wallpaper/overview.md`):** no dead scaffolding; this file is new/owned code (zero upstream-merge conflict); commit per logical unit.
- **Reference branch `gles` is read-only** — extract via `git -C ~/work/OpenTTD show gles:<path>`, never modify it.
- **Build gate:** gradle APK reaches `BUILD SUCCESSFUL`. Build takes 5–30 min (incremental); on timeout, re-run — timeout ≠ failure.
- **Python:** `/usr/bin/python3` only (homebrew python is sandbox-blocked).
- **Target:** physical arm64-v8a device via **GameActivity** (`org.openttd.android/.GameActivity`). The WallpaperService path is Stage 3.
- **Commit messages:** extremely concise; end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/video/gles_poi.cpp` | REPLACE | stub body → real POI scanner (scanners, selection, cycling, camera, debug overlay); no vehicle code |
| `src/video/gles_poi.h` | EDIT | drop `follow_vehicle` field + `../vehicle_type.h` include; keep the 5-function API + `GlesPOI` struct otherwise |

Task order = dependency order: Task 1 produces a green APK; Task 2 is the on-device gate.

---

### Task 1: Port the real POI scanner + prune the header

**Files:**
- Modify: `src/video/gles_poi.h` (drop include at `:17` + field at `:31`)
- Replace: `src/video/gles_poi.cpp` (stub `:20-31` → ported scanner)
- Reference (read-only): `gles:src/video/gles_poi.cpp` (648 lines), `gles:src/video/gles_poi.h`

**Interfaces:**
- Consumes (Stage 1, already in-tree): `ProcessOverlayActions` → `PrepareBackground`/`NavigatePOI`/`RotateTitleMap` (`sdl2_gles_v.cpp:454`); `DrawPOIMarkers(vp)` call site (`viewport.cpp:1884`); `CanRotateTitleMap()`/`RequestNextTitleMap()` (`wallpaper.h`, `wallpaper.cpp:79/84`); `PrepareBackground()` also invoked at `wallpaper.cpp:155` + `openttd.cpp:351`; scoped `GameMode`/`SwitchMode` (`openttd.h`).
- Produces: `struct GlesPOI` **without** `follow_vehicle`; the unchanged 5-function API `PrepareBackground()`, `NavigatePOI(int)`, `RecenterOnCurrentPOI()`, `InvalidatePOIs()`, `DrawPOIMarkers(const Viewport&)`.

- [ ] **Step 1: Pre-flight — verify upstream-drift symbols against the current tree**

Confirm the scoped-enum spellings and helper decls the port depends on, and prove the stale tokens are absent:

```bash
cd ~/work/OpenTTD
grep -n "enum class GameMode" -A 15 src/openttd.h        # expect a `Wallpaper,` member (openttd.h:23)
grep -n "enum class SwitchMode" -A 25 src/openttd.h      # expect `None,` (:28) and `Wallpaper,` (:43)
grep -rn "GM_WALLPAPER\|SM_WALLPAPER\|SM_NONE" src/       # expect ZERO hits — these tokens are stale
grep -n "CanRotateTitleMap\|RequestNextTitleMap" src/wallpaper.h   # both declared
grep -n "follow_vehicle" src/window_gui.h                # ViewportData::follow_vehicle present (upstream field)
```

Expected: `GameMode::Wallpaper`, `SwitchMode::None`, `SwitchMode::Wallpaper` all present; **zero** `GM_WALLPAPER`/`SM_WALLPAPER`/`SM_NONE` in `src/`; `CanRotateTitleMap`/`RequestNextTitleMap` declared; `ViewportData::follow_vehicle` present. Also spot-check a scanner symbol that drifted per the spec's Upstream-drift checklist:

```bash
grep -n "facilities\|train_station" src/base_station_base.h   # both live in BaseStation (:69/:84), reached via Station inheritance
```

- [ ] **Step 2: Prune the header (`src/video/gles_poi.h`)**

Two removals only:
1. Delete the include at `gles_poi.h:17`: `#include "../vehicle_type.h"`.
2. Delete the field at `gles_poi.h:31` (with its trailing comment): `VehicleID follow_vehicle = VehicleID::Invalid(); ///< ...`.

Leave everything else (the `GlesPOI` members `map_fx`/`map_fy`/`score`/`zoom`/`delay_ms`/`reason`/`influences`, the doc comment, and the 5 function declarations) unchanged. After this edit the header no longer references `VehicleID`, so dropping the include is correct.

- [ ] **Step 3: Swap in the reference `.cpp` body**

Overwrite the stub with the reference implementation verbatim (subsequent steps prune it):

```bash
cd ~/work/OpenTTD
git show gles:src/video/gles_poi.cpp > src/video/gles_poi.cpp
```

Expected: `src/video/gles_poi.cpp` is now the 648-line reference (still contains vehicle code + `SM_NONE` — removed next).

- [ ] **Step 4: Drop all vehicle code (the "no dead scaffolding" prune)**

Apply these removals to `src/video/gles_poi.cpp`:

1. **Includes:** delete `#include "../vehicle_base.h"` and `#include "../aircraft.h"` (used only by the dropped `ScanVehiclePOIs`).
2. **`ScanVehiclePOIs`:** delete the entire function (docstring `/** Scan vehicles and emit a "follow vehicle" POI (20% chance). */` through its closing brace).
3. **`ScanMapPOIs` call + bookkeeping:** delete the `ScanVehiclePOIs(candidates);` call and the `int n_vehicles = ...;` line; remove `vehicles={}` from the `"GLES POI candidates:"` `Debug(driver, …)` format string and drop its trailing `n_vehicles` argument; in the `"GLES POI after edge filter:"` line drop `+ n_vehicles` from the removed-count arithmetic.
4. **Edge-filter lambda:** delete the vehicle-exempt early return `if (c.follow_vehicle != VehicleID::Invalid()) return false;` (the edge margin now applies to every candidate).
5. **Top-50 dedup:** remove the `if (c.follow_vehicle == VehicleID::Invalid()) {` wrapper **and its matching closing brace**, de-indenting the enclosed body, so the 10-tile `DistanceManhattan` dedup runs on every candidate. There is **no `else` branch** (vehicle POIs were simply `push_back`-ed without the check), so removing the wrapper is purely additive to the filter. Drop **only the second sentence** of the pool comment — `* Vehicle-follow POIs skip distance check (they move).` — and keep the first line (`Build top-50 pool, skipping any within 10 tiles of an already selected entry.`).
6. **`ShowCurrentPOI`:** delete the commented-out follow branch (the `// TODO: temporarily disabled vehicle following` block, the `// if (poi.follow_vehicle …)` lines, and the `// } else {` / trailing `// }` wrappers). **KEEP** the active line `vp.follow_vehicle = VehicleID::Invalid();` and the static-camera body below it (Q1.4).

Do NOT touch `RecenterOnCurrentPOI` (no vehicle code — ports verbatim) or any scanner other than removing the vehicle one.

- [ ] **Step 5: Fix scoped-enum drift + restore the `VehicleID` include**

1. **Drift:** in `PrepareBackground`, change the wrap-to-rotate guard `_switch_mode == SM_NONE` → `_switch_mode == SwitchMode::None`. (This is the only stale enum token in the reference `.cpp`.)
2. **Include:** add `#include "../vehicle_type.h"` to `src/video/gles_poi.cpp`'s include block. The surviving `vp.follow_vehicle = VehicleID::Invalid();` reset (Step 4.6) needs `VehicleID`, which the reference previously got transitively via `gles_poi.h → vehicle_type.h`; the header prune (Step 2) removed that path, so the `.cpp` must include it directly. Do **not** re-add `vehicle_base.h`/`aircraft.h`.

- [ ] **Step 6: Static guard — grep the ported files for leftover / forbidden tokens**

```bash
cd ~/work/OpenTTD
grep -n "follow_vehicle\|ScanVehiclePOIs\|vehicle_base\|aircraft\.h\|sprite_x\|sprite_y\|SM_NONE\|SM_WALLPAPER\|GM_WALLPAPER" \
  src/video/gles_poi.cpp src/video/gles_poi.h
```

Expected: **exactly one** hit — `vp.follow_vehicle = VehicleID::Invalid();` in `gles_poi.cpp` (`ShowCurrentPOI`). Zero hits for everything else. Any other hit means a Step 4/5 removal was missed (fix before building). This also proves the pointer-math guard (no `sprite_x`/`sprite_y`) and the scoped-enum guard.

- [ ] **Step 7: Build gate**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```

(Equivalent documented alternative: `/usr/bin/python3 tools/run_android.py build`.)

Expected: `BUILD SUCCESSFUL`. If a symbol fails to resolve (114-day upstream drift), fix **mechanically and minimally** — add only the missing include (e.g. `../window_gui.h` if `ViewportData` is incomplete), never re-introduce vehicle scaffolding or change scanner logic. Re-run on timeout; the build is incremental.

- [ ] **Step 8: Commit**

```bash
git -C ~/work/OpenTTD add src/video/gles_poi.cpp src/video/gles_poi.h
git -C ~/work/OpenTTD commit -m "GLES POI: real map scanner, drop vehicle-follow

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: On-device verification gate (GameActivity, physical arm64)

**Files:**
- Modify: `docs/superpowers/plans/2026-07-08-stage2-poi-camera.md` (append `## Stage 2 Result`)

Reproduces the design spec's 6-check Verification gate. No source change unless a check fails and points back into Task 1. Uses the documented broadcasts (`JUMP_POI` advances the camera; `SWITCH_MAP` regenerates the map — see `CLAUDE.md` / `tools/run_android.py`).

- [ ] **Step 1: Install + launch (spec gate #1)**

```bash
adb install -r ~/work/OpenTTD/android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -c
adb shell am start -n org.openttd.android/.GameActivity
sleep 12 && adb logcat -d | grep -E "OpenTTD|SDL|GLES|AndroidRuntime|FATAL" | tail -80
```

Expected: no `UnsatisfiedLinkError`, no `FATAL`; the app renders. (If `Error extracting baseset assets` appears, rebuild+reinstall once — Stage 1 asset-packaging wart, out of Stage 2 scope.)

- [ ] **Step 2: Scanner logcat (spec gate #2)**

In the logs from Step 1, confirm the `Debug(driver, …)` signal:
- `GLES ScanMapPOIs: N POIs (map WxH)` with **N > 0**;
- the candidate-count breakdown line `GLES POI candidates: … (stations=… lighthouses=… junctions=… towns=…)` — note there is **no `vehicles=` field** (dropped);
- per-POI lines `POI[i] score=… fx=… fy=… zoom=… — <reason>`.

Expected: all three present; reasons name real features (`station:`, `rail junction:`, `town:`, `lighthouse`) — never a vehicle.

- [ ] **Step 3: Camera visits distinct locations; JUMP_POI advances (spec gate #3)**

```bash
adb exec-out screencap -p > /tmp/openttd/stage2_poi_a.png
adb shell am broadcast -a org.openttd.android.JUMP_POI     # or: /usr/bin/python3 tools/run_android.py jump
sleep 3
adb exec-out screencap -p > /tmp/openttd/stage2_poi_b.png
adb shell am broadcast -a org.openttd.android.JUMP_POI
sleep 3
adb exec-out screencap -p > /tmp/openttd/stage2_poi_c.png
```

Read all three PNGs. Expected: the camera sits at **distinct** map locations (not the fixed geometric center the stub produced), each broadcast visibly moves it. Cross-check each frame's position against the matching `POI[i]` logcat `fx/fy`.

> **Correction (final review, M1):** an earlier draft of this step also expected cluster POIs (logcat `zoom=1`) to "render zoomed out (`In2x`)". That is **not achievable with the ported code** — scanners compute `poi.zoom`, but `ShowCurrentPOI` never assigns it to `vp.zoom` (a verbatim omission inherited from the `gles` reference). Verify only distinct positions here; the zoom-application gap is tracked as a follow-up (see Out of scope).

- [ ] **Step 4: SWITCH_MAP → fresh scan (spec gate #4)**

```bash
adb logcat -c
adb shell am broadcast -a org.openttd.android.SWITCH_MAP    # or: /usr/bin/python3 tools/run_android.py switch
sleep 8 && adb logcat -d | grep -E "GLES ScanMapPOIs|GLES POI candidates" | tail -20
```

Expected: a new map loads and logcat shows a fresh `GLES ScanMapPOIs: …` with a **new** POI list (different N and/or reasons).

- [ ] **Step 5: Empty / tiny-map edge case (spec gate #5)**

Drive `SWITCH_MAP` until a tiny or feature-poor map appears (or observe the 64×64 empty fallback). Expected: `GLES ScanMapPOIs: 0 POIs …` is handled gracefully — the camera holds, **no crash**, no spin (every camera fn early-returns on an empty list).

- [ ] **Step 6: Accepted-race extended run (spec Known-risk + gate #3/#4 under stress)**

Loop repeated map regen + camera advance for several minutes and watch for the accepted scan/mutation race manifesting:

```bash
adb logcat -c
/usr/bin/python3 tools/run_android.py all 180 12   # ~3 min run, 12 SWITCH_MAP cycles + map switches
# then interleave manual advances during a separate long run:
for i in $(seq 1 40); do adb shell am broadcast -a org.openttd.android.JUMP_POI; sleep 2; \
  if [ $((i % 5)) -eq 0 ]; then adb shell am broadcast -a org.openttd.android.SWITCH_MAP; fi; done
adb logcat -d | grep -E "FATAL|AndroidRuntime|signal|abort" | tail -40
```

Expected: **no crash / no corruption** over the extended run with repeated `SWITCH_MAP` + `JUMP_POI` (accepted race did not manifest), and the ~648-line scan does **not** visibly stall the game thread (simulation keeps animating between/after scans). If a crash appears here, the race manifested → revisit and explicitly guard `ScanMapPOIs` (per Global Constraints / spec Known risk) before declaring Stage 2 done.

- [ ] **Step 7: POIs land on real features, not ocean (spec gate #6)**

Confirm placement quality via the `DrawPOIMarkers` debug overlay (red rect per POI, white for current, yellow influence lines) plus the logcat reasons:

```bash
adb exec-out screencap -p > /tmp/openttd/stage2_markers.png
```

Expected: markers sit on stations / junctions / towns / lighthouses (not open water), and influence lines connect each POI to its scoring contributors — confirming the motivating fix over the stub's geometric-center pick (which could land on ocean).

- [ ] **Step 8: Record result + commit**

Append a `## Stage 2 Result` section to this plan file: device model, pass/fail per gate check #1–#6, observed N/candidate breakdown on a representative map, whether the accepted race manifested (Step 6), and any known issues.

```bash
git -C ~/work/OpenTTD add docs/superpowers/plans/2026-07-08-stage2-poi-camera.md
git -C ~/work/OpenTTD commit -m "Stage 2 done: POI camera scanner on device

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Out of scope (later stages)

- Live vehicle-follow camera (dropped this stage; possible Stage 3+ enhancement).
- WallpaperService lifecycle / on-device service verification (Stage 3).
- POI scoring tuning against real title maps; perf instrumentation of the scan (Stage 5).
- Any explicit `ScanMapPOIs` mutex guard (only if the accepted race manifests — see Global Constraints / Task 2 Step 6).
- **FOLLOW-UP (M1, final review):** apply the computed `poi.zoom` to the viewport in `ShowCurrentPOI` so cluster POIs actually render zoomed out. Currently `poi.zoom` is scored but never written to `vp.zoom` (verbatim omission from the `gles` reference); the camera always renders at default zoom. Deferred — needs viewport-zoom handling (likely more than a one-line `vp.zoom` assignment) and diverges from the reference.

## Self-Review (performed at write time)

- **Spec coverage:** Scanners (`ScanStationPOIs`/`ScanLighthousePOIs`/`ScanJunctionPOIs`/`ScanTownPOIs`) + helpers port verbatim via the Step 3 swap; Selection (`ScanMapPOIs` — edge filter, sort, top-50 dedup, Fisher-Yates 20-pick, `Debug(driver)` breakdown) preserved (Step 4 only removes the vehicle count/exemptions); Cycling/camera (`PrepareBackground`/`NavigatePOI`/`RecenterOnCurrentPOI`/`ShowCurrentPOI`/`InvalidatePOIs`/`DrawPOIMarkers`) preserved; Drop list (`ScanVehiclePOIs`, `follow_vehicle` field + `.cpp` refs + commented branch, `vehicle_base.h`/`aircraft.h`, header `vehicle_type.h`) → Steps 2/4/5; Keep-Q1.4 reset → Step 4.6; scoped-enum drift → Steps 1/5/6; RNG/verification-signal → carried by the verbatim swap; Upstream-drift checklist → Step 1; Verification gate #1–#6 → Task 2 Steps 1–7 (accepted-race + real-feature checks in Steps 6–7). All spec sections mapped.
- **Placeholder scan:** none — every step is a concrete edit, grep, command, or expected-output assertion. Full C++ is not reproduced by design: the body is `git show`-extracted from the read-only reference and transformed by enumerated removals (house pattern for ports; see Stage 1 plan).
- **Type/name consistency:** the 5-function API and `GlesPOI` field names match between header (Step 2) and `.cpp` (Steps 3–5) and the Stage 1 call sites in Interfaces; `SwitchMode::None` / `CanRotateTitleMap` / `RequestNextTitleMap` / `vp.follow_vehicle` spellings match the verified tree (Step 1).

## Confidence Survey

No open questions. All detail decisions were ratified in the design spec's Reconciliation Log **Iteration 4** (interactive ratification): Q1.3/Q2.3 locking = port-as-is/no-mutex (override); Q1.4 camera reset kept; Q1.5 `std::rand()` kept; Q1.6 `Debug(driver)` logcat as verification signal. The only residual is Risk — real-scanner latency and POI-placement quality — which is inherently device-gate-verified (Task 2), not authorable in the plan.

**Iteration 2 (2026-07-08):** the plan was independently re-verified file-based against ground truth (lwp2 HEAD `634dd21703` + `gles` reference). Every drift symbol, every Drop-surface `file:line`, the transitive-`VehicleID` reasoning, the "vehicle includes used only by `ScanVehiclePOIs`" claim, and the dedup-wrapper / `RecenterOnCurrentPOI` structure were all confirmed — no drift, no stale symbol, no new question surfaced. See Reconciliation Log iteration 2. Confidence stays 90% (Risk-capped by the device gate).

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-08 (plan authored)
- **Source:** design spec `2026-07-07-stage2-poi-camera-design.md` (iteration 4, 90% Risk-capped, "Ready for implementation plan"). No plan-confidence survey iterations required — the spec's own iterations 1–4 resolved all Q1.x/Q2.x.
- **Verified against lwp2 HEAD `634dd21703`:** stub present (`gles_poi.cpp:20-31`); lwp2 header carries `follow_vehicle` (`:31`) + `../vehicle_type.h` (`:17`) — identical to the reference header, so the swap surface is exact.
- **Load-bearing find folded into the plan:** the reference `.cpp` sources `VehicleID` transitively through `gles_poi.h → vehicle_type.h`; pruning the header (Step 2) removes that path while `ShowCurrentPOI` keeps `VehicleID::Invalid()` (Q1.4). → Step 5.2 adds `#include "../vehicle_type.h"` to the `.cpp` directly (dropping `vehicle_base.h`/`aircraft.h` only). Step 6 grep + Step 7 build catch any residual include gap.
- **Drift:** the reference `.cpp`'s only stale enum token is `SM_NONE` (`PrepareBackground`); no `GM_WALLPAPER` in the body → single edit in Step 5.1, proven by the Step 1 + Step 6 greps.

### Iteration 2 — 2026-07-08 (independent file-based verification)
- **Confidence:** 90% (cap from **Risk** — unchanged; the honest ceiling for any pre-execution port). The residual is the device-gate behaviour (real-scanner latency; POIs-land-on-real-features), an external un-survey-able fact — a genuine plateau, not a plan gap.
- **Trigger:** re-ran plan-confidence file-based / non-interactively to pressure-test the authored plan against ground truth (lwp2 HEAD `634dd21703` + read-only `gles` reference). No interactive questions; every point settled from evidence.
- **Questions raised → resolved from evidence (no survey block needed):**
  - *Do Task 1 Step 1's drift symbols still resolve?* → YES. `GameMode::Wallpaper` (`openttd.h:23`), `SwitchMode::None` (`:28`), `SwitchMode::Wallpaper` (`:43`); **zero** `GM_WALLPAPER`/`SM_WALLPAPER`/`SM_NONE` anywhere in `src/`; `CanRotateTitleMap`/`RequestNextTitleMap` (`wallpaper.h:14/15`); `ViewportData::follow_vehicle` (`window_gui.h:251`); `facilities`/`train_station` in `BaseStation` (`base_station_base.h:69/84`). Survey is **not** capped on stale-symbol risk.
  - *Is the reference exactly the Drop surface the plan asserts?* → YES. `gles:gles_poi.cpp` = 648 lines; `vehicle_base.h`/`aircraft.h` (`:27/:28`); `ScanVehiclePOIs` (`:371`); `n_vehicles` + `vehicles={}` Debug (`:420-422`); edge-filter arithmetic `+ n_vehicles` (`:437`); vehicle-exempt early return (`:427`); top-50 dedup wrapper (`:447`); commented follow branch (`:511-518`,`:533`) + active reset (`:519`); lone stale `SM_NONE` (`:598`). Header `follow_vehicle` (`:31`) + `../vehicle_type.h` (`:17`) byte-identical between reference and lwp2 — the swap surface is exact.
  - *Does the load-bearing transitive-`VehicleID` reasoning hold?* → YES. Reference `.cpp` has **zero** direct `vehicle_type.h` include; `VehicleID` arrives only through the three vehicle includes the plan drops (header `vehicle_type.h` + `.cpp` `vehicle_base.h`/`aircraft.h`). Dropping all three ⇒ Step 5.2's direct `../vehicle_type.h` re-add is **required, not redundant**. Step 6 grep + Step 7 build catch any residual gap.
  - *Are `vehicle_base.h`/`aircraft.h` safe to drop?* → YES. All `Vehicle`/`Aircraft`/`veh->` usages are confined to `ScanVehiclePOIs` (`:375-397`) and the deleted commented block (`:515`); nothing else in the file references them.
  - *Does removing the dedup wrapper change behaviour correctly?* → YES. No `else` branch exists — vehicle POIs bypassed the check and were pushed directly; removing the `if`+brace makes the 10-tile dedup unconditional (the intended post-drop behaviour). Folded into Step 4.5.
  - *Is `RecenterOnCurrentPOI` clean, so Step 6's "exactly one `follow_vehicle` hit" holds?* → YES. `RecenterOnCurrentPOI` (`:537-556`) has **no** `follow_vehicle` reset; the sole surviving reference is `:519` in `ShowCurrentPOI`. Step 6's grep expectation is validated. (The design spec's parenthetical "(and `RecenterOnCurrentPOI`)" is loose — the reset lives only in `ShowCurrentPOI`; this **plan** is correct and consistent.)
  - *Build wiring?* → `gles_poi.cpp`/`.h` registered at `src/video/CMakeLists.txt:50-51`; **no CMake change** needed. Confirmed.
- **Folded into body:** Step 4.5 tightened (drop only the 2nd comment sentence; remove wrapper `if`+matching brace with de-indent; note no `else` branch). Top status line + Confidence Survey refreshed to iteration 2.
- **Still uncertain:** Risk only — Task 2 device-gate behaviour. Genuine plateau; not authorable in a pre-execution plan.
- **Status:** remains `Ready for execution`. Suggested downstream skill: `superpowers:executing-plans` (or `superpowers:subagent-driven-development`). **NOT invoked** (HARD-GATE: awaits explicit user review + approval).

### Iteration 3 — 2026-07-08 (interactive ratification + execution go)
- **Trigger:** user re-ran the iteration-2 verification findings as an interactive form (AskUserQuestion). Confidence unchanged (90%, Risk-capped).
- **Confirmed as-verified (no override):**
  - Include handling → move `../vehicle_type.h` into the `.cpp`; drop `vehicle_type.h`/`vehicle_base.h`/`aircraft.h` from the header (Step 5.2). Confirmed.
  - Dedup → 10-tile POI spacing is **unconditional** post-drop (Step 4.5). Confirmed.
  - Verification approach → static grep guard (Step 6) + gradle build gate + 6-check on-device gate; no unit-test harness at this layer. Confirmed proportionate.
- **Execution decision:** user chose **subagent-driven (this session)** → `superpowers:subagent-driven-development`. HARD-GATE lifted; execution authorized.

## Stage 2 Result

**Device:** Pixel 10 Pro, Android 17, arm64-v8a. Target `org.openttd.android/.GameActivity` (WallpaperService verification deferred to Stage 3, per plan scope). APK: Task 1 build, commit `cd8392506793`.

**Deviation from Task 2 brief:** `JUMP_POI`/`SWITCH_MAP` broadcasts do not reach GameActivity — only `OpenTTDWallpaperService` registers those receivers. Verified via source (`OpenTTDWallpaperService.java` vs `GameActivity.java`) and empirically (broadcasts "Enqueued" by the system, never handled by the app). Used GameActivity's on-screen buttons instead: `POI >`/`< POI` → `nativeNavigatePOI` (found via `adb shell uiautomator dump`), `MAP >`/`< MAP` → `nativeRotateMap` → `RotateTitleMap` (same function `nativeSwitchMap` calls, so functionally equivalent to `SWITCH_MAP`).

**Gate results:**
- **#1 Install + launch:** PASS. No `UnsatisfiedLinkError`/`FATAL`. Clean EGL/GLES boot, atlas + sprites loaded, process stable for the full ~10 min session.
- **#2 Scanner logcat:** PASS. Representative map (`opntitle.dat`, 256x256): `GLES POI candidates: 4 total (stations=4 lighthouses=0 junctions=0 towns=0)` (no `vehicles=` field) → `GLES ScanMapPOIs: 3 POIs` → per-POI reasons `station: train+5 town(...)+2 cluster+3`, `station: bus+1 town(...)+3`, `station: bus+1` — all real features, never a vehicle.
- **#3 Camera visits distinct locations:** PASS. 3 screenshots across 2 `POI >` taps showed 3 visually distinct scenes (downtown/rail hub → different town w/ river → small crossroad+bus stop near a lake), each matching the logged `fx/fy` for that POI index.
- **#4 SWITCH_MAP-equivalent → fresh scan:** PASS. `MAP >` tap loaded `Titlegame14.sav`; N 3→20, candidates 4→49 `(stations=45 towns=4)`, entirely new POI list.
- **#5 Empty/tiny map (0 POIs):** INCONCLUSIVE dynamically. All 4 bundled title maps are non-empty (N=3,20,20,20); the true empty-map fallback (`wallpaper.cpp:145-147`, `GenerateWorld(GWM_EMPTY, 64, 64)`) only fires if every title file fails to load, and `GameActivity.onCreate()` re-copies title assets from the APK on every launch, defeating an on-device attempt to hide them (`run-as` file rename). Verified instead via static code inspection: `RecenterOnCurrentPOI`, `NavigatePOI`, `PrepareBackground`, `DrawPOIMarkers` (`gles_poi.cpp:491,513/516,530,575`) all early-return on `_gles_poi_list.empty()`. Not a Task 1 defect — a test-reachability gap from the fixed bundled title-map set.
- **#6 Accepted-race extended run:** PASS — **race did not manifest.** Skipped `run_android.py all` (would activate the not-ready WallpaperService, out of scope); ran a manual ~3.5 min loop of 60 `POI >` taps + 12 interleaved `MAP >` taps directly against GameActivity. PID unchanged (14936) throughout, zero FATAL/AndroidRuntime/signal/abort lines, no new tombstones, 72 `poi_change` + 12 `map_load_end` events processed cleanly with no visible game-thread stall.
- **#7 POIs land on real features, not ocean:** PASS on the substantive claim — every reason across every map named real features (station/town/bus/train/cluster), never ocean, corroborated by camera screenshots. The `DrawPOIMarkers` debug overlay itself does not visibly render on this driver: pixel-scanned two overlay screenshots (converted to BMP, checked against exact `PC_RED`/`PC_YELLOW`/`PC_WHITE` palette RGB from `table/palettes.h`) and found zero marker pixels. Root cause: the GLES driver's active `Blitter_Snapshot` (`src/blitter/snapshot.hpp:38-42`) has no-op `SetPixel`/`DrawRect`/`DrawLine`/`DrawColourMappingRect` overrides (by design — see `docs/wallpaper/engine-changes.md`), so `GfxFillRect`/`GfxDrawLine` (which `DrawPOIMarkers` uses) silently draw nothing. Pre-existing Stage 1 blitter-architecture fact, not touched or introduced by Task 1's scanner swap.

**Known issues (non-blocking, no Task 1 source change indicated):**
1. Gate #5's genuine 0-POI path is untested dynamically on-device (reachability limited by the fixed title-map asset set + unconditional asset re-copy on launch); static-guard evidence only.
2. `DrawPOIMarkers`'s visual overlay (red/white marker rects, yellow influence lines) is inert under the GLES snapshot blitter — a Stage 1 architectural gap worth a future ticket if the overlay is ever needed for debugging on-device, but out of Stage 2 scope.

Full evidence (logcat excerpts, screenshot descriptions, pixel-scan methodology) in `.superpowers/sdd/task-2-report.md`.
