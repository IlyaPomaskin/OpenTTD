# Build System Changes

Two build modes: desktop (unchanged flow, new sources) and Android (Gradle → CMake wrapper → OpenTTD as `add_subdirectory` shared lib).

## Root CMakeLists.txt — subproject support

Core refactor enabling `add_subdirectory(openttd-root)` from the Android wrapper:

- `set(OPENTTD_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})` — new var; `CMAKE_SOURCE_DIR` now points at the *wrapper* when embedded. Every `CMAKE_SOURCE_DIR` reference in root + sub-CMakeLists replaced by `CMAKE_CURRENT_SOURCE_DIR` (root) or `OPENTTD_SOURCE_DIR` (bin/ai, bin/game, media, media/baseset, script/api, table/settings, CreateGrfCommand, emscripten preload paths).
- Target type switch:
  ```cmake
  if(ANDROID) add_library(openttd SHARED)   # loaded by SDL via dlopen, entry = SDL_main
  else()      add_executable(openttd WIN32)
  ```
- ANDROID gates:
  - skip `find_package` for ZLIB/LibLZMA/LZO/PNG/CURL/breakpad/OpenGL/fontconfig-etc — wrapper pre-provides imported targets + cache vars;
  - `OPENGLES_FOUND=TRUE` + `-DWITH_OPENGLES` (GLESv3/EGL linked by wrapper);
  - `openttd_test`, `enable_testing`, `regression`, Pandoc, `InstallAndPackage` all skipped;
  - `target_link_libraries(openttd_lib android log)`.
- `cmake/SourceList.cmake`: test-file helper early-returns when `openttd_test` target absent.
- `cmake/Options.cmake`: ANDROID personal dir = `openttd`, shared/global dirs unset (real paths come from `OPENTTD_DATA_PATH` env at runtime, see fileio.cpp).

## New sources wired in

- `src/CMakeLists.txt`: `wallpaper.cpp/h`.
- `src/blitter/CMakeLists.txt`: `snapshot.cpp/hpp` `CONDITION NOT OPTION_DEDICATED AND OPENGLES_FOUND`.
- `src/video/CMakeLists.txt`: `gles_poi.*` (unconditional); `sdl2_gles_v.* gles_backend.* gles_sprite.*` `CONDITION SDL2_FOUND AND OPENGLES_FOUND`.
- `src/os/unix/CMakeLists.txt`: ANDROID → adds `../android/android_main.cpp` to `openttd` target instead of unix main.
- `src/os/macosx/osx_stdafx.h`: OTTD Rect/Point/etc rename-macros wrapped in `#if !defined(STRGEN) && !defined(SETTINGSGEN)` — host-tools build on macOS broke on the name-conflict hacks.
- `src/table/settings/CMakeLists.txt`: settingsgen preamble/postamble paths via `OPENTTD_SOURCE_DIR`.

## Android wrapper (android/app/src/main/CMakeLists.txt)

Chain: `gradle assembleDebug` → externalNativeBuild cmake → wrapper project → `add_subdirectory(OPENTTD_ROOT)`.

1. **Prebuilt deps**: ExternalProject downloads OpenRCT2 android deps bundle (`${ANDROID_ABI}-android-dynamic.tar.zst` v13): SDL2, png16, zlib, freetype, ICU (+bz2/brotli transitive). No dep compilation.
2. **find_package spoofing**: imported SHARED targets `SDL2::SDL2 / PNG::PNG / ZLIB::ZLIB / Freetype::Freetype` + `*_FOUND=TRUE` cache force; ICU via `ICU_*_LIBRARY/LIBRARIES/INCLUDE_DIRS` cache vars. Include dirs pre-created with `file(MAKE_DIRECTORY)` so INTERFACE validation passes before download.
3. **Host tools cross-compile bootstrap**: nested `execute_process(cmake)` at *configure time* builds strgen/settingsgen natively (`OPTION_TOOLS_ONLY=ON`, toolchain cleared, `/usr/bin/cc|c++`), result passed as `HOST_BINARY_DIR` — OpenTTD's existing cross-compile mechanism picks generated tools up.
4. `add_subdirectory` → `openttd` shared lib; `target_link_libraries(openttd_lib GLESv3 EGL)`; `add_dependencies(openttd_lib libs)`.
5. **Packaging**: `CopySharedLibs.cmake` globs deps' `*.so` into jniLibs output (resolves symlinks, renames back — versioned .so symlinks break APK packaging otherwise). `copy_assets` target: generated `baseset/` + `lang/` from build dir → `assets/` (packed into APK, extracted to filesDir at runtime by Java).

## Gradle

- AGP 8.11.1, Gradle 9.0, compileSdk 36, minSdk 24, NDK 27.3, CMake 4.1.2.
- `abiFilters 'arm64-v8a'` default; `-PabiFilter=x86_64,...` override (emulator); `-PskipNativeBuild` for Java-only builds.
- `-DANDROID_STL=c++_shared`, `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (16KB page devices).
- Release build signed with debug config (dev only).

## Misc

- `.gitignore`: `.claude/ .idea/ sprites-original/ .cache`.
- New tools (not build system proper): see [tools.md](tools.md).
- Build commands: desktop `cmake -B /tmp/openttd -S . && cmake --build /tmp/openttd -j4`; Android `cd android && JAVA_HOME=<temurin-25> ./gradlew assembleDebug` (or `/usr/bin/python3 tools/run_android.py build`).

## Reimpl notes

- Host-tools bootstrap at configure time is fragile (hardcoded `/usr/bin/cc`, retry hack for empty `-j`); consider CMake toolchain-file-based superbuild or prebuilt tools artifact.
- find_package spoofing works but silently breaks if OpenTTD adds new deps; a proper CMake package dir (`CMAKE_PREFIX_PATH` with config files) would be cleaner.
- `OPENTTD_SOURCE_DIR` refactor is upstream-mergeable and worth keeping regardless.
