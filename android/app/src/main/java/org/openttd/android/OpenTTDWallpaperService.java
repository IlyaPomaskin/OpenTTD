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

    private BroadcastReceiver mJumpReceiver;
    private BroadcastReceiver mSwitchMapReceiver;
    private BroadcastReceiver mNextPoiReceiver;
    private BroadcastReceiver mPrevPoiReceiver;
    private BroadcastReceiver mNextMapReceiver;
    private BroadcastReceiver mPrevMapReceiver;
    private BroadcastReceiver mScrollCameraReceiver;
    private BroadcastReceiver mSettingsChangedReceiver;
    private BroadcastReceiver mTitleMapsChangedReceiver;

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
        mJumpReceiver = command(WallpaperNative.CMD_PREPARE_BG, 0, 0);
        registerReceiver(mJumpReceiver, new IntentFilter(ACTION_JUMP_POI), Context.RECEIVER_EXPORTED);
        mSwitchMapReceiver = command(WallpaperNative.CMD_ROTATE_MAP, 1, 0);
        registerReceiver(mSwitchMapReceiver, new IntentFilter(ACTION_SWITCH_MAP), Context.RECEIVER_EXPORTED);
        mNextPoiReceiver = command(WallpaperNative.CMD_NAVIGATE_POI, 1, 0);
        registerReceiver(mNextPoiReceiver, new IntentFilter(ACTION_NEXT_POI), Context.RECEIVER_EXPORTED);
        mPrevPoiReceiver = command(WallpaperNative.CMD_NAVIGATE_POI, -1, 0);
        registerReceiver(mPrevPoiReceiver, new IntentFilter(ACTION_PREV_POI), Context.RECEIVER_EXPORTED);
        mNextMapReceiver = command(WallpaperNative.CMD_ROTATE_MAP, 1, 0);
        registerReceiver(mNextMapReceiver, new IntentFilter(ACTION_NEXT_MAP), Context.RECEIVER_EXPORTED);
        mPrevMapReceiver = command(WallpaperNative.CMD_ROTATE_MAP, -1, 0);
        registerReceiver(mPrevMapReceiver, new IntentFilter(ACTION_PREV_MAP), Context.RECEIVER_EXPORTED);
        mScrollCameraReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int dx = intent.getIntExtra("dx", 0);
                int dy = intent.getIntExtra("dy", 0);
                Log.i(TAG, "SCROLL_CAMERA dx=" + dx + " dy=" + dy);
                if (sLibrariesLoaded && sSDLInitialized) {
                    WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_SCROLL_CAMERA, dx, dy);
                }
            }
        };
        registerReceiver(mScrollCameraReceiver, new IntentFilter(ACTION_SCROLL_CAMERA), Context.RECEIVER_EXPORTED);
        mSettingsChangedReceiver = command(WallpaperNative.CMD_RELOAD_SETTINGS, 0, 0);
        registerReceiver(mSettingsChangedReceiver,
            new IntentFilter(WallpaperConfig.ACTION_SETTINGS_CHANGED), Context.RECEIVER_EXPORTED);
        mTitleMapsChangedReceiver = command(WallpaperNative.CMD_REFRESH_TITLE_MAPS, 0, 0);
        registerReceiver(mTitleMapsChangedReceiver,
            new IntentFilter(SettingsHelper.ACTION_TITLE_MAPS_CHANGED), Context.RECEIVER_EXPORTED);
    }

    /** A receiver that forwards a fixed command to the native engine. */
    private BroadcastReceiver command(final String cmd, final int arg1, final int arg2) {
        return new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.i(TAG, "broadcast -> " + cmd + " " + arg1 + "," + arg2);
                if (sLibrariesLoaded && sSDLInitialized) {
                    WallpaperNative.nativeWallpaperCommand(cmd, arg1, arg2);
                }
            }
        };
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
        if (mTitleMapsChangedReceiver != null) {
            unregisterReceiver(mTitleMapsChangedReceiver);
            mTitleMapsChangedReceiver = null;
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
            Log.i(TAG, "onSurfaceCreated: took surface ownership sOverrideSurface=" + SDLActivity.sOverrideSurface);
            Log.i(TAG, "onSurfaceCreated: calling onNativeSurfaceCreated surface=" + mEngineSurface);
            SDLActivity.onNativeSurfaceCreated();
            WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_SURFACE_CHANGED, 0, 0);
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
            Log.i(TAG, "onSurfaceChanged: re-took surface ownership sOverrideSurface=" + SDLActivity.sOverrideSurface);
            SDLActivity.nativeSetScreenResolution(width, height, width, height, 60.0f);
            SDLActivity.onNativeResize();
            SDLActivity.onNativeSurfaceChanged();
            WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_SURFACE_CHANGED, 0, 0);
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
                WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_SET_PAUSED, 0, 0);
                SDLActivity.mNextNativeState = SDLActivity.NativeState.RESUMED;
                SDLActivity.handleNativeState();
            } else {
                Log.i(TAG, "onVisibilityChanged: prepare background, delaying pause for warm-up");
                WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_PREPARE_BG, 0, 0);
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                    if (!mVisible) {
                        Log.i(TAG, "onVisibilityChanged: warm-up done, pausing game thread");
                        WallpaperNative.nativeWallpaperCommand(WallpaperNative.CMD_SET_PAUSED, 1, 0);
                    }
                }, 150);
            }
        }

    }
}
