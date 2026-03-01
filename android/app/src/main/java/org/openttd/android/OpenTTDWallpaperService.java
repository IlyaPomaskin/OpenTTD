package org.openttd.android;

import android.os.Handler;
import android.os.Looper;
import android.service.wallpaper.WallpaperService;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

public class OpenTTDWallpaperService extends WallpaperService {
    private static final String TAG = "OpenTTDWallpaper";

    // Same library list as GameActivity.getLibraries()
    private static final String[] LIBRARIES = {
        "c++_shared", "z", "bz2", "brotlicommon", "brotlidec",
        "png16", "freetype", "SDL2", "icudata", "icuuc", "icui18n", "openttd"
    };

    private static boolean sLibrariesLoaded = false;
    private static boolean sSDLInitialized = false;

    @Override
    public Engine onCreateEngine() {
        return new OpenTTDEngine();
    }

    private class OpenTTDEngine extends Engine {
        private static final long PAUSE_DELAY_MS = 5000;
        private Surface mEngineSurface;
        private int mSurfaceWidth = 1;
        private int mSurfaceHeight = 1;
        private final Handler mHandler = new Handler(Looper.getMainLooper());
        private final Runnable mDeferredPause = () -> {
            SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
            SDLActivity.handleNativeState();
        };

        @Override
        public void onCreate(SurfaceHolder surfaceHolder) {
            super.onCreate(surfaceHolder);
            setTouchEventsEnabled(true);
            // Load native libraries once per process
            if (!sLibrariesLoaded) {
                for (String lib : LIBRARIES) {
                    SDL.loadLibrary(lib, getApplicationContext());
                }
                sLibrariesLoaded = true;
            }
        }

        @Override
        public void onTouchEvent(MotionEvent event) {
            if (!sSDLInitialized) return;
            int action = event.getActionMasked();
            float x = event.getX() / mSurfaceWidth;
            float y = event.getY() / mSurfaceHeight;
            Log.i(TAG, "onTouchEvent action=" + action + " x=" + event.getX() + " y=" + event.getY());
            int sdlAction;
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    sdlAction = 0; // SDL_FINGERDOWN
                    break;
                case MotionEvent.ACTION_UP:
                    sdlAction = 1; // SDL_FINGERUP
                    break;
                case MotionEvent.ACTION_MOVE:
                    sdlAction = 2; // SDL_FINGERMOTION
                    break;
                default:
                    return;
            }
            SDLActivity.onNativeTouch(0, 0, sdlAction, x, y, event.getPressure());
        }

        @Override
        public void onSurfaceCreated(SurfaceHolder holder) {
            super.onSurfaceCreated(holder);
            if (!sLibrariesLoaded) return;

            if (!sSDLInitialized) {
                // Copy assets (baseset, lang) to internal storage
                MainActivity.copyAssetsStatic(getApplicationContext());
                // Set data path env var
                try {
                    android.system.Os.setenv("OPENTTD_DATA_PATH",
                        getApplicationContext().getFilesDir().getAbsolutePath(), true);
                } catch (android.system.ErrnoException e) {
                    Log.e(TAG, "Failed to set OPENTTD_DATA_PATH", e);
                }

                SDLActivity.initForService(getApplicationContext());
                String nativeLibDir = getApplicationContext().getApplicationInfo().nativeLibraryDir;
                SDLActivity.sOverrideLibrary = nativeLibDir + "/libopenttd.so";
                SDLActivity.sOverrideFunction = "SDL_main";
                SDLActivity.sOverrideArguments = new String[]{"-s", "null", "-m", "null"};
                sSDLInitialized = true;
            }

            if (SDLActivity.sOverrideSurface != null) {
                SDLActivity.onNativeSurfaceDestroyed();
            }
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            SDLActivity.onNativeSurfaceCreated();
        }

        @Override
        public void onSurfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            super.onSurfaceChanged(holder, format, width, height);
            if (!sSDLInitialized) return;
            mSurfaceWidth = Math.max(width, 1);
            mSurfaceHeight = Math.max(height, 1);
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            SDLActivity.nativeSetScreenResolution(width, height, width, height, 60.0f);
            SDLActivity.onNativeResize();
            SDLActivity.onNativeSurfaceChanged();
        }

        @Override
        public void onSurfaceDestroyed(SurfaceHolder holder) {
            mHandler.removeCallbacks(mDeferredPause);
            if (SDLActivity.sOverrideSurface == mEngineSurface) {
                SDLActivity.sOverrideSurface = null;
                SDLActivity.onNativeSurfaceDestroyed();
            }
            mEngineSurface = null;
            SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
            SDLActivity.handleNativeState();
            super.onSurfaceDestroyed(holder);
        }

        @Override
        public void onVisibilityChanged(boolean visible) {
            super.onVisibilityChanged(visible);
            if (!sSDLInitialized) return;
            if (visible) {
                mHandler.removeCallbacks(mDeferredPause);
                // Re-inject surface if lost during engine transition
                Surface currentSurface = getSurfaceHolder().getSurface();
                if (SDLActivity.sOverrideSurface == null
                        && currentSurface != null && currentSurface.isValid()) {
                    mEngineSurface = currentSurface;
                    SDLActivity.sOverrideSurface = mEngineSurface;
                    SDLActivity.onNativeSurfaceCreated();
                    android.graphics.Rect frame = getSurfaceHolder().getSurfaceFrame();
                    SDLActivity.nativeSetScreenResolution(
                        frame.width(), frame.height(),
                        frame.width(), frame.height(), 60.0f);
                    SDLActivity.onNativeResize();
                    SDLActivity.onNativeSurfaceChanged();
                }
                SDLActivity.mNextNativeState = SDLActivity.NativeState.RESUMED;
                SDLActivity.handleNativeState();
            } else {
                // Delay pause to let the rendering loop finish current frame
                mHandler.removeCallbacks(mDeferredPause);
                mHandler.postDelayed(mDeferredPause, PAUSE_DELAY_MS);
            }
        }

        @Override
        public void onDestroy() {
            mHandler.removeCallbacks(mDeferredPause);
            super.onDestroy();
        }
    }
}
