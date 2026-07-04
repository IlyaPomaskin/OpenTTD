# Stage 0: Build Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Plan-confidence status:** Ready for execution (iteration 3, confidence 92%)

**Goal:** OpenTTD builds as Android APK from branch `lwp2` (fresh off upstream/master). APK build success = Stage 0 done (per user directive); on-device boot verification deferred to Stage 1 entry.

**Architecture:** Port the battle-tested build layer from reference branch `gles` (see `docs/wallpaper/build-system.md`): root CMake subproject refactor + Android Gradle/CMake wrapper + vendored SDL Java glue + `android_main.cpp`. New files copied verbatim via `git checkout gles -- <path>`; modified upstream files re-applied via 3-way patch since upstream moved ~114 days past the reference's merge-base `e24f92ce8`.

**Tech Stack:** CMake, Gradle 9/AGP 8.11, NDK 27, prebuilt OpenRCT2 Android deps (SDL2 etc.), adb.

## Global Constraints

- Working dir: `~/work/OpenTTD`, branch `lwp2`. Reference branch: `gles` (do NOT modify it). Merge-base: `e24f92ce8`.
- Desktop build dir: `/tmp/openttd`; always `-j4` (never dynamic core detection).
- Python: `/usr/bin/python3` only (homebrew python blocked by sandbox).
- Gradle: `JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ./android/gradlew -p android <task>`.
- Git: use `git -C ~/work/OpenTTD ...`, never `cd`.
- Target: physical arm64-v8a device (emulator NOT a target). No device steps in Stage 0 — APK build is the final gate.
- Spec: `docs/wallpaper/build-system.md` (build layer), `docs/wallpaper/android-app.md` (app structure). Port as-is; do NOT redesign in Stage 0.
- Commit messages: extremely concise. End with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

### Task 1: Commit project scaffolding

**Files:**
- Already staged: `CLAUDE.md`, `.gitignore`, `claude-cli.sb` (carried from gles; sandbox profile is session-critical)
- Add: `docs/wallpaper/*.md` (8 spec files, untracked), `docs/superpowers/plans/2026-07-04-stage0-build-skeleton.md`

**Interfaces:**
- Produces: clean baseline commit on `lwp2`; all later tasks branch from it.

- [ ] **Step 1: Pull fresh upstream, confirm lwp2 sits on the tip**

```bash
git -C ~/work/OpenTTD fetch upstream
git -C ~/work/OpenTTD rev-parse --short upstream/master
git -C ~/work/OpenTTD rev-parse --short lwp2
```

Expected: both hashes identical. If upstream moved: `git -C ~/work/OpenTTD stash --include-untracked && git -C ~/work/OpenTTD merge --ff-only upstream/master && git -C ~/work/OpenTTD stash pop` (lwp2 has no own commits yet, ff always succeeds).

- [ ] **Step 2: Verify staged state**

Run: `git -C ~/work/OpenTTD status --short`
Expected: `M .gitignore`, `A CLAUDE.md`, `A claude-cli.sb`, `?? docs/wallpaper/`, `?? docs/superpowers/`, `?? android/` (android/ = local build artifacts, ignore for now — Task 3 handles it).

- [ ] **Step 3: Stage docs and commit**

```bash
git -C ~/work/OpenTTD add docs/wallpaper docs/superpowers
git -C ~/work/OpenTTD commit -m "Project config + wallpaper reimpl spec docs

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

Expected: commit created; `git -C ~/work/OpenTTD status --short` shows only `?? android/`.

---

### Task 2: Port build-system layer (CMake refactor)

**Files (all modified upstream files — apply as 3-way patch from gles):**
- Modify: `CMakeLists.txt`, `cmake/Options.cmake`, `cmake/SourceList.cmake`, `cmake/CreateGrfCommand.cmake`, `bin/ai/CMakeLists.txt`, `bin/game/CMakeLists.txt`, `media/CMakeLists.txt`, `media/baseset/CMakeLists.txt`, `src/script/api/CMakeLists.txt`, `src/table/settings/CMakeLists.txt`, `src/os/macosx/osx_stdafx.h`

**Interfaces:**
- Produces: `OPENTTD_SOURCE_DIR` cmake var; `add_library(openttd SHARED)` under `if(ANDROID)`; `OPENGLES_FOUND`/`WITH_OPENGLES` on Android; ANDROID gates skipping find_package/tests/packaging. Task 3's wrapper depends on all of these names exactly.

- [ ] **Step 1: Apply the reference patch 3-way**

```bash
git -C ~/work/OpenTTD diff e24f92ce8..gles -- CMakeLists.txt cmake/Options.cmake cmake/SourceList.cmake cmake/CreateGrfCommand.cmake bin/ai/CMakeLists.txt bin/game/CMakeLists.txt media/CMakeLists.txt media/baseset/CMakeLists.txt src/script/api/CMakeLists.txt src/table/settings/CMakeLists.txt src/os/macosx/osx_stdafx.h > /tmp/openttd-stage0-build.patch
git -C ~/work/OpenTTD apply -3 /tmp/openttd-stage0-build.patch
```

Expected: clean apply, OR conflict markers in `CMakeLists.txt` (upstream churn). On conflict: resolve keeping BOTH upstream's new content AND the refactor pattern — every `CMAKE_SOURCE_DIR` in root becomes `CMAKE_CURRENT_SOURCE_DIR`, sub-dirs use `OPENTTD_SOURCE_DIR`; ANDROID gates per `docs/wallpaper/build-system.md` "Root CMakeLists.txt" section. If upstream added NEW `CMAKE_SOURCE_DIR` references since the patch, convert those too: check with `grep -n "CMAKE_SOURCE_DIR" ~/work/OpenTTD/CMakeLists.txt` — zero hits expected after resolve (only `CMAKE_CURRENT_SOURCE_DIR`/`OPENTTD_SOURCE_DIR`).

(Desktop-build gates skipped per Q1.6 — Android-only target; the APK build in Task 5 is the sole build gate. Any refactor breakage surfaces there.)

- [ ] **Step 2: Commit**

```bash
git -C ~/work/OpenTTD add -u
git -C ~/work/OpenTTD commit -m "Build: subproject refactor + Android gates (port from gles)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Port Android project + entry point + tools

**Files (all NEW on lwp2 — verbatim from gles):**
- Create: `android/` (entire dir: gradle project, wrapper CMakeLists, SDL Java glue, org.openttd.android Java, manifest, res, assets — incl. `assets/title/*.sav`, confirmed Q1.2)
- Create: `src/os/android/android_main.cpp`
- Modify: `src/os/unix/CMakeLists.txt` (ANDROID → add android_main.cpp to openttd target)
- Create: `tools/run_android.py`, `tools/test_wallpaper.sh`

**Interfaces:**
- Consumes: Task 2's `OPENTTD_SOURCE_DIR`, `add_library(openttd SHARED)`, imported-target names (`SDL2::SDL2` etc. — wrapper defines, root consumes).
- Produces: `android/app/build/outputs/apk/debug/app-debug.apk` path used by Tasks 5–6; JNI class names `org.openttd.android.*` fixed.

- [ ] **Step 1: Clear stale build artifacts, restore reference files**

```bash
rm -rf ~/work/OpenTTD/android
git -C ~/work/OpenTTD checkout gles -- android/ src/os/android/ src/os/unix/CMakeLists.txt tools/run_android.py tools/test_wallpaper.sh
```

Expected: `git -C ~/work/OpenTTD status --short` shows all as `A`/`M`, no `??` leftovers under android/.

- [ ] **Step 2: Fix ICU spoofing for new upstream (KNOWN drift — upstream deleted cmake/FindICU.cmake, now uses built-in FindICU with imported targets)**

In `android/app/src/main/CMakeLists.txt`, replace the ICU cache-var block (`set(ICU_i18n_FOUND ...)` ... `set(ICU_uc_INCLUDE_DIRS ...)`) with imported targets matching upstream's `link_package(ICU_I18N TARGET ICU::i18n)` / `link_package(ICU_UC TARGET ICU::uc)` and the `ICU_I18N_FOUND` (uppercase) check at root CMakeLists line ~192:

```cmake
# ICU — upstream uses CMake built-in FindICU: needs ICU::<comp> targets + ICU_<COMP>_FOUND.
foreach(comp i18n uc data)
    add_library(ICU::${comp} SHARED IMPORTED GLOBAL)
    set_target_properties(ICU::${comp} PROPERTIES
        IMPORTED_LOCATION "${LIBS_DIR}/lib/libicu${comp}.so"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBS_DIR}/include"
    )
endforeach()
set(ICU_I18N_FOUND TRUE CACHE BOOL "" FORCE)
set(ICU_UC_FOUND TRUE CACHE BOOL "" FORCE)
```

(Reference pattern `add_dependencies(<imported> libs)` is not valid for these; the existing `add_dependencies(openttd_lib libs)` already orders the download. Note: upstream also added optional HarfBuzz — skipped on Android via the existing `elseif(ANDROID)` gate, warning is expected and fine.)

- [ ] **Step 3: Sanity check android_main against new upstream API**

Run: `grep -n "openttd_main" ~/work/OpenTTD/src/openttd.h`
Expected: `int openttd_main(std::span<std::string_view> arguments);` — matches the call in `src/os/android/android_main.cpp`. If signature changed upstream, adapt `android_main.cpp` (only that file).

- [ ] **Step 4: Commit**

```bash
git -C ~/work/OpenTTD add android src/os/android src/os/unix/CMakeLists.txt tools/run_android.py tools/test_wallpaper.sh
git -C ~/work/OpenTTD commit -m "Android: gradle project, SDL glue, android_main (port from gles)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Engine Android-compat patch (runtime essentials)

**Files (modified upstream files — 3-way patch from gles):**
- Modify: `src/fileio.cpp` (OPENTTD_DATA_PATH env → search paths), `src/debug.cpp` (Debug() → logcat), `src/os/unix/unix.cpp` (ShowOSErrorBox → logcat), `src/ini.cpp` (`__ANDROID__` include guard)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: engine finds baseset via `OPENTTD_DATA_PATH` (Java sets it, see `android-app.md`); all `Debug()` output visible in logcat — Task 6's verification depends on this.

- [ ] **Step 1: Apply patch**

```bash
git -C ~/work/OpenTTD diff e24f92ce8..gles -- src/fileio.cpp src/debug.cpp src/os/unix/unix.cpp src/ini.cpp > /tmp/openttd-stage0-compat.patch
git -C ~/work/OpenTTD apply -3 /tmp/openttd-stage0-compat.patch
```

Expected: clean apply (these are small `#ifdef __ANDROID__` blocks). On conflict in `debug.cpp`/`unix.cpp`/`ini.cpp`: re-apply manually per `docs/wallpaper/engine-changes.md` "Misc" section — each is a self-contained ifdef block. **On conflict in `fileio.cpp` (61 upstream lines changed there): STOP and ask the user before resolving (per Q1.4).**

(Desktop-build gate skipped per Q1.6 — APK build in Task 5 catches breakage.)

- [ ] **Step 2: Commit**

```bash
git -C ~/work/OpenTTD add -u
git -C ~/work/OpenTTD commit -m "Engine: Android data path + logcat output (port from gles)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Build APK, fix upstream drift

**Files:**
- Possibly modify: any file failing to compile for Android (114 days of upstream drift; likely candidates: cmake ANDROID gates vs new find_package calls, new upstream source using APIs gated off on Android)

**Interfaces:**
- Consumes: everything from Tasks 2–4.
- Produces: `android/app/build/outputs/apk/debug/app-debug.apk`.

- [ ] **Step 1: Build**

```bash
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home ~/work/OpenTTD/android/gradlew -p ~/work/OpenTTD/android assembleDebug
```

Expected first run: deps bundle download + host-tools configure + full engine cross-compile (~10–20 min). Likely failures & fixes:
- host-tools configure fails → check hardcoded `/usr/bin/cc|c++` in `android/app/src/main/CMakeLists.txt` still valid;
- new upstream dependency not spoofed → add imported target/cache var in wrapper CMakeLists per existing pattern;
- engine source fails Android compile → smallest possible `#ifdef __ANDROID__` gate;
- cmake refactor errors (desktop gates were skipped per Q1.6) → fix per docs/wallpaper/build-system.md.

Iterate until: `BUILD SUCCESSFUL`. **Do NOT commit fixes as you go — batch them (per Q1.3).**

- [ ] **Step 2: Verify APK exists**

Run: `ls -la ~/work/OpenTTD/android/app/build/outputs/apk/debug/app-debug.apk`
Expected: file present, >40MB (contains .so + baseset assets).

- [ ] **Step 3: Present combined drift-fix review, then commit (per Q1.3)**

Show the user the full combined diff of all drift fixes (`git -C ~/work/OpenTTD diff` + `status --short`) with a one-line rationale per file. WAIT for approval. Then:

```bash
git -C ~/work/OpenTTD add -u
git -C ~/work/OpenTTD commit -m "Fix Android build vs current upstream

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

Also log each engine-file fix in `docs/wallpaper/engine-changes.md` "Misc" (include in the same commit). Skip entirely if no fixes were needed.

---

### Task 6: Wrap-up — record result, push to fork

**Files:**
- Modify: `docs/superpowers/plans/2026-07-04-stage0-build-skeleton.md` (add `## Stage 0 Result`)

**Interfaces:**
- Consumes: `app-debug.apk` from Task 5.
- Produces: Stage 0 DONE marker; `fork/lwp2` branch on GitHub. On-device boot verification (install, GameActivity launch, logcat check) moved to Stage 1 entry criteria — first step of the Stage 1 plan.

- [ ] **Step 1: Record the outcome**

Append to this file under a new `## Stage 0 Result` heading (2-3 lines): APK path + size, build duration, list of drift fixes applied in Task 5.

```bash
git -C ~/work/OpenTTD add docs/superpowers/plans/2026-07-04-stage0-build-skeleton.md
git -C ~/work/OpenTTD commit -m "Stage 0 done: APK builds

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 2: Push lwp2 to fork (per Q1.5)**

```bash
git -C ~/work/OpenTTD push -u fork lwp2
```

Expected: branch created on `github.com/IlyaPomaskin/OpenTTD`; tracking switches from upstream/master to fork/lwp2.

## Confidence Survey

*(no open questions — all iteration 1 questions folded into the plan body)*

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-04 (time of writing)
- **Confidence:** 82% (cap from readiness: no adb device attached; Task 5 inherently exploratory)
- **Resolved:** — (first iteration; context verification folded directly: openttd_main signature unchanged ✓, temurin-25 + /usr/bin/cc ✓, ICU drift found → new Task 3 Step 2 with imported-targets fix)
- **Still uncertain:** device availability (readiness), fileio.cpp patch conflict handling (unknowns), drift-fix autonomy (readiness)
- **New questions:** Q1.1–Q1.6

### Iteration 2 — 2026-07-04 19:20
- **Confidence:** 90% (residual: Task 5 drift is inherently discover-at-build-time; protocol for it now fixed)
- **Resolved:** Q1.1 → device connected by Task 6 → Task 6 Step 1; Q1.2 → port saves verbatim → Task 3 Files; Q1.3 → batch fixes + combined review before commit → Task 5 Steps 1/3; Q1.4 → STOP and ask on fileio.cpp conflict → Task 4 Step 1; Q1.5 → push to fork after Stage 0 → Task 6 Step 5; Q1.6 → skip desktop gates, APK build is sole gate → Tasks 2/4 trimmed
- **Still uncertain:** exact set of drift fixes Task 5 will need (unknowable pre-build; bounded by batch-review protocol)
- **New questions:** none

### Iteration 3 — 2026-07-04 19:30
- **Confidence:** 92% (device readiness gap removed)
- **Resolved:** user directive (no survey) → drop on-device verification from Stage 0; APK build = done → Goal, Global Constraints, Task 6 rewritten as wrap-up (record + push); boot verification moved to Stage 1 entry criteria
- **Still uncertain:** Task 5 drift-fix set (unknowable pre-build, bounded by batch review)
- **New questions:** none

## Stage 0 Result

**DONE — 2026-07-05.** APK builds from `lwp2` off upstream/master.
- APK: `android/app/build/outputs/apk/debug/app-debug.apk`, 97,367,236 bytes (~93 MB).
- Build: `BUILD SUCCESSFUL in 32m 17s` (first full build; attempt 1 failed only on a gitignored, uncommitted `android/local.properties` `sdk.dir` — environmental).
- Task 5 drift fixes: ZERO. Only upstream-drift adaptation was the ICU imported-targets fix, applied pre-emptively in Task 3 (a7021e0a54).
- Commits: 41f9f7bc8e (docs/config), 02d8ef6a37 (cmake refactor), a7021e0a54 (android port + ICU), 6623f779ce (compat patch).
