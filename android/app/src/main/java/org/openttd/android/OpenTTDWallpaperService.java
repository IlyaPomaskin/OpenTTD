package org.openttd.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;

import android.service.wallpaper.WallpaperService;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

public class OpenTTDWallpaperService extends WallpaperService {
    private static final String TAG = "OpenTTDWallpaper";
    private static final String ACTION_JUMP_POI = "org.openttd.android.JUMP_POI";
    private static final String ACTION_SWITCH_MAP = "org.openttd.android.SWITCH_MAP";
    private static final String ACTION_NEXT_POI = "org.openttd.android.NEXT_POI";
    private static final String ACTION_PREV_POI = "org.openttd.android.PREV_POI";
    private static final String ACTION_NEXT_MAP = "org.openttd.android.NEXT_MAP";
    private static final String ACTION_PREV_MAP = "org.openttd.android.PREV_MAP";
    private static final String ACTION_SCROLL_CAMERA = "org.openttd.android.SCROLL_CAMERA";


    /** Jump camera to next POI and start rendering the new area. */
    private static native void nativePrepareBackground();
    /** Cycle zoom level In2x → Normal → Out2x → In2x. */
    private static native void nativeCycleZoom();
    /** Trigger map regeneration. */
    private static native void nativeSwitchMap();
    /** Navigate POI by delta (+1/-1) without auto map rotation. */
    private static native void nativeNavigatePOI(int delta);
    /** Rotate title map by delta (+1/-1). */
    private static native void nativeRotateMap(int delta);
    /** Scroll camera by pixel offset. */
    private static native void nativeScrollCamera(int dx, int dy);
    /** Pause/resume game thread when wallpaper not visible. */
    private static native void nativeSetGamePaused(boolean paused);
    /** Signal GL thread that wallpaper surface changed and EGL needs rebind. */
    private static native void nativeSurfaceChanged();

    private BroadcastReceiver mJumpReceiver;
    private BroadcastReceiver mSwitchMapReceiver;
    private BroadcastReceiver mNextPoiReceiver;
    private BroadcastReceiver mPrevPoiReceiver;
    private BroadcastReceiver mNextMapReceiver;
    private BroadcastReceiver mPrevMapReceiver;
    private BroadcastReceiver mScrollCameraReceiver;
    private BroadcastReceiver mSettingsChangedReceiver;


    // Same library list as GameActivity.getLibraries()
    private static final String[] LIBRARIES = {
        "c++_shared", "z", "bz2", "brotlicommon", "brotlidec",
        "png16", "freetype", "SDL2", "icudata", "icuuc", "icui18n", "openttd"
    };

    private static boolean sLibrariesLoaded = false;
    private static boolean sSDLInitialized = false;

    @Override
    public void onCreate() {
        super.onCreate();
        mJumpReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "JUMP_POI broadcast received");
                nativePrepareBackground();
            }
        };
        registerReceiver(mJumpReceiver, new IntentFilter(ACTION_JUMP_POI),
            Context.RECEIVER_EXPORTED);
        mSwitchMapReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "SWITCH_MAP broadcast received");
                nativeSwitchMap();
            }
        };
        registerReceiver(mSwitchMapReceiver, new IntentFilter(ACTION_SWITCH_MAP),
            Context.RECEIVER_EXPORTED);
        mNextPoiReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "NEXT_POI broadcast received");
                nativeNavigatePOI(1);
            }
        };
        registerReceiver(mNextPoiReceiver, new IntentFilter(ACTION_NEXT_POI),
            Context.RECEIVER_EXPORTED);
        mPrevPoiReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "PREV_POI broadcast received");
                nativeNavigatePOI(-1);
            }
        };
        registerReceiver(mPrevPoiReceiver, new IntentFilter(ACTION_PREV_POI),
            Context.RECEIVER_EXPORTED);
        mNextMapReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "NEXT_MAP broadcast received");
                nativeRotateMap(1);
            }
        };
        registerReceiver(mNextMapReceiver, new IntentFilter(ACTION_NEXT_MAP),
            Context.RECEIVER_EXPORTED);
        mPrevMapReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "PREV_MAP broadcast received");
                nativeRotateMap(-1);
            }
        };
        registerReceiver(mPrevMapReceiver, new IntentFilter(ACTION_PREV_MAP),
            Context.RECEIVER_EXPORTED);
        mScrollCameraReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int dx = intent.getIntExtra("dx", 0);
                int dy = intent.getIntExtra("dy", 0);
                Log.i(TAG, "SCROLL_CAMERA broadcast received dx=" + dx + " dy=" + dy);
                nativeScrollCamera(dx, dy);
            }
        };
        registerReceiver(mScrollCameraReceiver, new IntentFilter(ACTION_SCROLL_CAMERA),
            Context.RECEIVER_EXPORTED);
        mSettingsChangedReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int interval = SettingsHelper.getMapUpdateInterval(context);
                int zoom = SettingsHelper.getMapZoom(context);
                int brightness = SettingsHelper.getBrightness(context);
                Log.i(TAG, "SETTINGS_CHANGED: interval=" + interval
                    + " zoom=" + zoom + " brightness=" + brightness);
            }
        };
        registerReceiver(mSettingsChangedReceiver,
            new IntentFilter(SettingsHelper.ACTION_SETTINGS_CHANGED),
            Context.RECEIVER_EXPORTED);
    }

    @Override
    public void onDestroy() {
        if (mJumpReceiver != null) {
            unregisterReceiver(mJumpReceiver);
            mJumpReceiver = null;
        }
        if (mSwitchMapReceiver != null) {
            unregisterReceiver(mSwitchMapReceiver);
            mSwitchMapReceiver = null;
        }
        if (mNextPoiReceiver != null) {
            unregisterReceiver(mNextPoiReceiver);
            mNextPoiReceiver = null;
        }
        if (mPrevPoiReceiver != null) {
            unregisterReceiver(mPrevPoiReceiver);
            mPrevPoiReceiver = null;
        }
        if (mNextMapReceiver != null) {
            unregisterReceiver(mNextMapReceiver);
            mNextMapReceiver = null;
        }
        if (mPrevMapReceiver != null) {
            unregisterReceiver(mPrevMapReceiver);
            mPrevMapReceiver = null;
        }
        if (mScrollCameraReceiver != null) {
            unregisterReceiver(mScrollCameraReceiver);
            mScrollCameraReceiver = null;
        }
        if (mSettingsChangedReceiver != null) {
            unregisterReceiver(mSettingsChangedReceiver);
            mSettingsChangedReceiver = null;
        }
        super.onDestroy();
    }

    @Override
    public Engine onCreateEngine() {
        return new OpenTTDEngine();
    }

    private class OpenTTDEngine extends Engine {
        private Surface mEngineSurface;
        private int mSurfaceWidth = 1;
        private int mSurfaceHeight = 1;
        private boolean mVisible = false;

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
            /* Zoom cycling on tap disabled. */
        }

        @Override
        public void onSurfaceCreated(SurfaceHolder holder) {
            Log.i(TAG, "onSurfaceCreated: holder=" + holder + " surface=" + holder.getSurface());
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
                SDLActivity.sOverrideArguments = new String[]{"-s", "null", "-m", "null", "-d", "driver=3"};
                sSDLInitialized = true;
            }

            Log.i(TAG, "onSurfaceCreated: sSDLInitialized=" + sSDLInitialized
                + " sOverrideSurface=" + SDLActivity.sOverrideSurface);
            if (SDLActivity.sOverrideSurface != null) {
                Log.i(TAG, "onSurfaceCreated: destroying old surface before creating new one");
                SDLActivity.onNativeSurfaceDestroyed();
            }
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            Log.i(TAG, "onSurfaceCreated: calling onNativeSurfaceCreated surface=" + mEngineSurface);
            SDLActivity.onNativeSurfaceCreated();
            nativeSurfaceChanged();
        }

        @Override
        public void onSurfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Log.i(TAG, "onSurfaceChanged: " + width + "x" + height + " format=" + format);
            super.onSurfaceChanged(holder, format, width, height);
            if (!sSDLInitialized) return;
            mSurfaceWidth = Math.max(width, 1);
            mSurfaceHeight = Math.max(height, 1);
            mEngineSurface = holder.getSurface();
            SDLActivity.sOverrideSurface = mEngineSurface;
            SDLActivity.nativeSetScreenResolution(width, height, width, height, 60.0f);
            SDLActivity.onNativeResize();
            SDLActivity.onNativeSurfaceChanged();
            nativeSurfaceChanged();
        }

        @Override
        public void onSurfaceDestroyed(SurfaceHolder holder) {
            Log.i(TAG, "onSurfaceDestroyed: mEngineSurface=" + mEngineSurface
                + " sOverrideSurface=" + SDLActivity.sOverrideSurface);
            if (SDLActivity.sOverrideSurface == mEngineSurface) {
                Log.i(TAG, "onSurfaceDestroyed: active engine — calling onNativeSurfaceDestroyed + PAUSED");
                SDLActivity.sOverrideSurface = null;
                SDLActivity.onNativeSurfaceDestroyed();
                SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
                SDLActivity.handleNativeState();
            } else {
                Log.i(TAG, "onSurfaceDestroyed: stale engine — skipping pause to avoid blocking new surface");
            }
            mEngineSurface = null;
            super.onSurfaceDestroyed(holder);
        }

        @Override
        public void onVisibilityChanged(boolean visible) {
            Log.i(TAG, "onVisibilityChanged: visible=" + visible
                + " sOverrideSurface=" + SDLActivity.sOverrideSurface
                + " mCurrentNativeState=" + SDLActivity.mCurrentNativeState
                + " mNextNativeState=" + SDLActivity.mNextNativeState);
            super.onVisibilityChanged(visible);
            if (!sSDLInitialized) return;
            mVisible = visible;
            if (visible) {
                // Re-inject surface if lost during engine transition
                Surface currentSurface = getSurfaceHolder().getSurface();
                Log.i(TAG, "onVisibilityChanged visible: currentSurface=" + currentSurface
                    + " isValid=" + (currentSurface != null && currentSurface.isValid()));
                if (SDLActivity.sOverrideSurface == null
                        && currentSurface != null && currentSurface.isValid()) {
                    Log.i(TAG, "onVisibilityChanged: re-injecting lost surface");
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
                Log.i(TAG, "onVisibilityChanged: resuming game thread");
                nativeSetGamePaused(false);
                SDLActivity.mNextNativeState = SDLActivity.NativeState.RESUMED;
                SDLActivity.handleNativeState();
            } else {
                /* Jump POI now while still rendering — the game+GL threads
                 * keep running (SDL not paused) so the new camera position
                 * gets rendered and the sprite cache warms up.  Delay the
                 * game thread pause to allow a few frames at the new POI. */
                Log.i(TAG, "onVisibilityChanged: jumping POI, delaying pause for warm-up");
                nativePrepareBackground();
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                    if (!mVisible) {
                        Log.i(TAG, "onVisibilityChanged: warm-up done, pausing game thread");
                        nativeSetGamePaused(true);
                    }
                }, 150);
            }
        }

    }
}
