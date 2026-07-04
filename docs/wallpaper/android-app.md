# Android App (android/)

All-new Gradle project wrapping OpenTTD as APK: live wallpaper service + settings UI + debug game activity.

## Build system

- `android/app/build.gradle`: compileSdk 36, minSdk 24, NDK 27, arm64-v8a default (`-PabiFilter=` to override), `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (16KB pages), c++_shared. CMake targets: `openttd`, `copy_assets`. `-PskipNativeBuild` for Java-only iterations.
- `android/app/src/main/CMakeLists.txt` (wrapper project):
  - Deps: **prebuilt OpenRCT2 android deps bundle** (SDL2, png, zlib, freetype, ICU, bz2, brotli) via ExternalProject download — SDL2 native is stock, unpatched.
  - Fakes `find_package` results: imported targets `SDL2::SDL2`, `PNG::PNG`, `ZLIB::ZLIB`, `Freetype::Freetype`, ICU cache vars.
  - Builds host tools (strgen/settingsgen) natively first via nested cmake (`OPTION_TOOLS_ONLY=ON`, `HOST_BINARY_DIR`).
  - `add_subdirectory(OPENTTD_ROOT)` → root CMakeLists builds `add_library(openttd SHARED)` when ANDROID (instead of executable); links `GLESv3 EGL android log`.
  - `copy_assets`: baseset+lang from build dir → APK assets.
- Root CMake changes for subproject use: `OPENTTD_SOURCE_DIR` var replaces `CMAKE_SOURCE_DIR` everywhere; ANDROID skips find_package(zlib/lzma/lzo/png/curl/breakpad/OpenGL); `WITH_OPENGLES` define.
- Entry point: `src/os/android/android_main.cpp` — `SDL_main` → StrMakeValid args → crashlog init → `openttd_main`. `OPENTTD_DATA_PATH` env consumed by fileio.cpp.
- Assets in APK: `baseset/`, `lang/`, `title/*.sav` (bundled title maps) — copied to `filesDir` on first run (`MainActivity.copyAssetsStatic`).

## Manifest / processes

- `WallpaperSettingsActivity` = LAUNCHER.
- `OpenTTDWallpaperService` in **`:wallpaper` process**; `GameActivity` in **`:game` process** — game debug and wallpaper never share a process (each has own SDL/native state; brightness etc. passed via intent extras, not statics).
- GameActivity: `configChanges` covers everything (no recreate on rotation), `screenOrientation=sensor`.

## Components

| Class | Role |
|---|---|
| `OpenTTDWallpaperService` | WallpaperService hosting SDL headless (see below) |
| `WallpaperSettingsActivity` | launcher UI: set-wallpaper button, brightness slider (0–100 → nativeSetBrightness), title-maps manager, Debug Game button |
| `MapsActivity` | manage `filesDir/title/*.sav`: file picker import, delete |
| `MainActivity` | legacy asset-copy + launch GameActivity (not launcher) |
| `GameActivity` | SDLActivity subclass for debugging: overlay buttons (map ±, POI ±, d-pad scroll 600px, pause/play, back), immersive fullscreen, `killProcess` on finish (OpenTTD ignores SDL_QUIT, join would hang) |
| `SettingsHelper` | SharedPreferences (brightness, map interval, zoom) + `SETTINGS_CHANGED` broadcast |

## Wallpaper service flow

`OpenTTDEngine` (WallpaperService.Engine):
- `onCreate`: load libs once per process (`c++_shared,z,bz2,brotli*,png16,freetype,SDL2,icu*,openttd`).
- `onSurfaceCreated` (first time): copy assets, setenv `OPENTTD_DATA_PATH`, `SDLActivity.initForService()`, set `sOverrideLibrary/Function/Arguments` (`libopenttd.so`, `SDL_main`, `-s null -m null -d driver=3`). Then inject surface: `sOverrideSurface = holder.getSurface()` → `onNativeSurfaceCreated()` → `nativeSurfaceChanged()` (tells GL thread to rebind EGL). Destroys previous surface first if exists.
- `onSurfaceChanged`: update sOverrideSurface + `nativeSetScreenResolution` + `onNativeResize` + `onNativeSurfaceChanged` + `nativeSurfaceChanged()`.
- `onSurfaceDestroyed`: only if THIS engine owns the active surface — null it, `onNativeSurfaceDestroyed`, force PAUSED state. Stale engines (preview↔home transition creates overlapping engines) skip pause to not block new surface.
- `onVisibilityChanged(true)`: re-inject surface if lost, `nativeSetGamePaused(false)`, push brightness, RESUMED.
- `onVisibilityChanged(false)`: `nativePrepareBackground()` (jump to next POI **while still visible** — renders + warms atlas at new position), then delayed 150ms `nativeSetGamePaused(true)`.

## Control surface (broadcast receivers, service onCreate)

`adb shell am broadcast -a org.openttd.android.X`: `JUMP_POI`, `SWITCH_MAP`, `NEXT_POI`/`PREV_POI`, `NEXT_MAP`/`PREV_MAP`, `SCROLL_CAMERA` (--ei dx/dy), `SETTINGS_CHANGED` (brightness push). All → native atomics (see wallpaper-mode.md).

## Notes for reimpl

- Brightness needs delayed retry push (GLESBackend not ready until SDL thread renders) — 500ms Handler hack in both service and GameActivity.
- Static `sLibrariesLoaded`/`sSDLInitialized` guards: SDL_main can only start once per process; wallpaper service process restart = fresh native state.
- Map interval / zoom prefs exist in SettingsHelper but consumers are hidden/disabled in the settings UI (only brightness wired end-to-end).
