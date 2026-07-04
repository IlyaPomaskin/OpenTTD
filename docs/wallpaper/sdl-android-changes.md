# SDL Android Changes

SDL2 **native code is stock** (prebuilt .so from OpenRCT2 deps bundle, v2.x). All modifications are in the **vendored Java glue** `android/app/src/main/java/org/libsdl/app/` — needed because stock SDLActivity assumes an Activity owns the surface/lifecycle, but a WallpaperService has neither Activity nor SDLSurface view.

## The problem being solved

Run SDL app headless inside `WallpaperService`: no Activity, no `SDLSurface` view, surface comes from `WallpaperService.Engine`'s SurfaceHolder and can be swapped/destroyed at any time (preview ↔ home screen creates overlapping engines).

## SDLActivity.java modifications

New static "service mode" state:

```java
public static Surface sOverrideSurface;   // wallpaper injects surface here
public static boolean sServiceMode;       // true when running as wallpaper
public static Context mServiceContext;    // app context replaces Activity
public static String sOverrideLibrary;    // lib path (nativeLibraryDir + /libopenttd.so)
public static String sOverrideFunction;   // "SDL_main"
public static String[] sOverrideArguments;
```

- `initForService(Context)`: new — `SDL.setupJNI() + SDL.initialize() + sServiceMode=true + SDL.setContext(appContext)`. Replaces the whole `onCreate` path (no window, no SDLSurface, no layout, no HIDDeviceManager).
- `initialize()`: resets the new service-mode statics too (process-reuse safety).
- `getNativeSurface()` (called from SDL C via JNI): **returns `sOverrideSurface` first**, falls back to `mSurface.getNativeSurface()`. This is the core hook — SDL's native EGL window creation transparently uses the wallpaper surface.
- `handleNativeState()`:
  - RESUMED condition in service mode = `sOverrideSurface != null` (instead of `mSurface.mIsSurfaceReady && mHasFocus && mIsResumedCalled`).
  - starts `mSDLThread` w/o enabling accelerometer in service mode.
  - PAUSED transition guards `mSurface != null` before `mSurface.handlePause()`.
  - extra logging.
- `SDLMain.run()`: uses `sOverride{Library,Function,Arguments}` in service mode instead of `mSingleton.getMainSharedObject()/getArguments()`; on exit just nulls `mSDLThread` (no `Activity.finish()`).
- Many static callbacks get `mSingleton == null` guards (no Activity exists in service mode).
- Wallpaper service drives lifecycle manually: sets `mNextNativeState` + calls `handleNativeState()`, calls `onNativeSurfaceCreated/Changed/Destroyed` + `nativeSetScreenResolution/onNativeResize` directly (stock SDL these come from SDLSurface callbacks).

## SDLSurface.java

- `surfaceChanged()`: early-return when `sServiceMode` — the (nonexistent/unused) SDLSurface view must not fight the wallpaper engine's surface dimensions.

## SDL.java / SDLAudioManager / SDLControllerManager / HID*

Unmodified stock (SDL.setContext works with any Context). Audio unused (null drivers).

## Remaining SDL-related workarounds live in C++ (see rendering.md)

SDL's C side is unpatched, so its blind spots are handled in `sdl2_gles_v.cpp`:
- SDL never recreates its EGL surface when the wallpaper engine hands a new ANativeWindow → `RecoverContextIfLost()` manually `eglCreateWindowSurface` over `SDLActivity.getNativeSurface()` (via SDL_GetWindowWMInfo, JNI fallback to `getNativeSurface()`).
- `SDL_GL_CreateContext` can "succeed" while internal eglMakeCurrent failed → verified via `eglGetCurrentContext/Surface/eglGetError` after creation.
- Swap failure detection via `eglGetError()` after `SDL_GL_SwapWindow` (returns void).
- `SDL_APP_DIDENTERBACKGROUND/WILLENTERFOREGROUND` events → game thread pause/resume; `SDL_RENDER_DEVICE_RESET` → `_gles_context_lost`.

## Reimplementation note

The vendored SDL Java is a full copy (~5700 lines) with maybe ~60 changed lines. Consider: diff against upstream SDL2 release to extract a clean patch, or subclass/reflection instead of forking. Changes are all greppable by `sOverride|sServiceMode|initForService|mServiceContext`.
