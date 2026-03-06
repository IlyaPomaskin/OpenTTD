package org.openttd.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import android.service.wallpaper.WallpaperService;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.widget.Toast;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

public class OpenTTDWallpaperService extends WallpaperService {
    private static final String TAG = "OpenTTDWallpaper";
    private static final String ACTION_JUMP_POI = "org.openttd.android.JUMP_POI";
    private static final String ACTION_SWITCH_MAP = "org.openttd.android.SWITCH_MAP";
    private static final String ACTION_TOAST = "org.openttd.android.TOAST";

    /** Jump camera to a random map waypoint and mark the area dirty for asset pre-loading. */
    private static native void nativePrepareBackground();
    /** Cycle zoom level In2x → Normal → Out2x → In2x. */
    private static native void nativeCycleZoom();
    /** Trigger map regeneration. */
    private static native void nativeSwitchMap();

    private BroadcastReceiver mJumpReceiver;
    private BroadcastReceiver mSwitchMapReceiver;
    private BroadcastReceiver mToastReceiver;

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
        mToastReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String msg = intent.getStringExtra("msg");
                if (msg == null) msg = "";
                Log.i(TAG, "TOAST broadcast: " + msg);
                new Handler(Looper.getMainLooper()).post(() ->
                    Toast.makeText(context, msg, Toast.LENGTH_SHORT).show());
            }
        };
        registerReceiver(mToastReceiver, new IntentFilter(ACTION_TOAST),
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
        if (mToastReceiver != null) {
            unregisterReceiver(mToastReceiver);
            mToastReceiver = null;
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
                SDLActivity.sOverrideArguments = new String[]{"-s", "null", "-m", "null"};
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
        }

        @Override
        public void onSurfaceDestroyed(SurfaceHolder holder) {
            Log.i(TAG, "onSurfaceDestroyed: mEngineSurface=" + mEngineSurface
                + " sOverrideSurface=" + SDLActivity.sOverrideSurface);
            if (SDLActivity.sOverrideSurface == mEngineSurface) {
                Log.i(TAG, "onSurfaceDestroyed: calling onNativeSurfaceDestroyed");
                SDLActivity.sOverrideSurface = null;
                SDLActivity.onNativeSurfaceDestroyed();
            }
            mEngineSurface = null;
            Log.i(TAG, "onSurfaceDestroyed: setting state=PAUSED");
            SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
            SDLActivity.handleNativeState();
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
                Log.i(TAG, "onVisibilityChanged: setting state=RESUMED");
                SDLActivity.mNextNativeState = SDLActivity.NativeState.RESUMED;
                SDLActivity.handleNativeState();
            } else {
                Log.i(TAG, "onVisibilityChanged: preparing background then pausing");
                nativePrepareBackground();
                SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
                SDLActivity.handleNativeState();
            }
        }

    }
}
