package org.openttd.android;

import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {

    private static final String TAG = "GameActivity";
    private static final int SCROLL_PX = 600;

    private static native void nativeRotateMap(int delta);
    private static native void nativeNavigatePOI(int delta);
    private static native void nativeScrollCamera(int dx, int dy);
    private static native void nativeSetGamePaused(boolean paused);

    @Override
    protected String[] getLibraries() {
        return new String[]{
                "c++_shared",
                "z",
                "bz2",
                "brotlicommon",
                "brotlidec",
                "png16",
                "freetype",
                "SDL2",
                "icudata",
                "icuuc",
                "icui18n",
                "openttd"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Ensure assets are extracted and data path is set (needed when launched directly via adb)
        MainActivity.copyAssetsStatic(getApplicationContext());
        try {
            android.system.Os.setenv("OPENTTD_DATA_PATH",
                getFilesDir().getAbsolutePath(), true);
        } catch (android.system.ErrnoException e) {
            Log.e(TAG, "Failed to set OPENTTD_DATA_PATH", e);
        }
        super.onCreate(savedInstanceState);
        addOverlayButtons();
    }

    private Button btn(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(12);
        b.setMinimumWidth(0);
        b.setMinimumHeight(0);
        b.setPadding(16, 8, 16, 8);
        b.setClickable(true);
        b.setFocusable(true);
        b.setOnClickListener(listener);
        return b;
    }

    private void addOverlayButtons() {
        // Bottom bar: map + POI navigation
        LinearLayout navBar = new LinearLayout(this);
        navBar.setOrientation(LinearLayout.HORIZONTAL);
        navBar.setGravity(Gravity.CENTER);
        navBar.setBackgroundColor(0x80000000);
        navBar.setPadding(8, 4, 8, 4);
        navBar.setId(View.generateViewId());

        navBar.setElevation(10);
        navBar.addView(btn("< Map", v -> { Log.d(TAG, "BTN: < Map"); nativeRotateMap(-1); }));
        navBar.addView(btn("Map >", v -> { Log.d(TAG, "BTN: Map >"); nativeRotateMap(1); }));
        navBar.addView(btn("< POI", v -> { Log.d(TAG, "BTN: < POI"); nativeNavigatePOI(-1); }));
        navBar.addView(btn("POI >", v -> { Log.d(TAG, "BTN: POI >"); nativeNavigatePOI(1); }));

        RelativeLayout.LayoutParams navParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        navParams.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        navParams.addRule(RelativeLayout.CENTER_HORIZONTAL);
        navParams.bottomMargin = 16;
        mLayout.addView(navBar, navParams);

        // D-pad above nav bar: arrow keys layout
        //     [▲]
        //  [◀][▼][▶]
        RelativeLayout dpad = new RelativeLayout(this);
        dpad.setElevation(10);
        dpad.setBackgroundColor(0x80000000);
        dpad.setPadding(4, 4, 4, 4);
        dpad.setId(View.generateViewId());

        Button up = btn("\u25B2", v -> nativeScrollCamera(0, -SCROLL_PX));
        up.setId(View.generateViewId());
        RelativeLayout.LayoutParams upP = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        upP.addRule(RelativeLayout.CENTER_HORIZONTAL);
        dpad.addView(up, upP);

        Button left = btn("\u25C0", v -> nativeScrollCamera(-SCROLL_PX, 0));
        left.setId(View.generateViewId());
        RelativeLayout.LayoutParams leftP = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        leftP.addRule(RelativeLayout.BELOW, up.getId());
        leftP.addRule(RelativeLayout.ALIGN_PARENT_START);
        dpad.addView(left, leftP);

        Button down = btn("\u25BC", v -> nativeScrollCamera(0, SCROLL_PX));
        down.setId(View.generateViewId());
        RelativeLayout.LayoutParams downP = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        downP.addRule(RelativeLayout.BELOW, up.getId());
        downP.addRule(RelativeLayout.END_OF, left.getId());
        dpad.addView(down, downP);

        Button right = btn("\u25B6", v -> nativeScrollCamera(SCROLL_PX, 0));
        RelativeLayout.LayoutParams rightP = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        rightP.addRule(RelativeLayout.BELOW, up.getId());
        rightP.addRule(RelativeLayout.END_OF, down.getId());
        dpad.addView(right, rightP);

        RelativeLayout.LayoutParams dpadParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        dpadParams.addRule(RelativeLayout.ABOVE, navBar.getId());
        dpadParams.addRule(RelativeLayout.CENTER_HORIZONTAL);
        dpadParams.bottomMargin = 4;
        mLayout.addView(dpad, dpadParams);

        // Pause/Play buttons: right side, above nav bar
        LinearLayout pauseBar = new LinearLayout(this);
        pauseBar.setElevation(10);
        pauseBar.setOrientation(LinearLayout.VERTICAL);
        pauseBar.setGravity(Gravity.CENTER);
        pauseBar.setBackgroundColor(0x80000000);
        pauseBar.setPadding(4, 4, 4, 4);

        pauseBar.addView(btn("\u25B6", v -> nativeSetGamePaused(false)));
        pauseBar.addView(btn("\u23F8", v -> nativeSetGamePaused(true)));

        RelativeLayout.LayoutParams pauseParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        pauseParams.addRule(RelativeLayout.ALIGN_PARENT_END);
        pauseParams.addRule(RelativeLayout.ABOVE, navBar.getId());
        pauseParams.rightMargin = 16;
        pauseParams.bottomMargin = 4;
        mLayout.addView(pauseBar, pauseParams);

        // Back button: top-left corner
        Button back = btn("\u2190 Back", v -> finish());
        back.setBackgroundColor(0x80000000);
        RelativeLayout.LayoutParams backParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        backParams.addRule(RelativeLayout.ALIGN_PARENT_START);
        backParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        backParams.leftMargin = 16;
        backParams.topMargin = 16;
        back.setElevation(10);
        mLayout.addView(back, backParams);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        if (hasFocus) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            );
        }
    }

    @Override
    protected void onDestroy() {
        Log.w(TAG, "onDestroy: isChangingConfigurations=" + isChangingConfigurations()
            + " isFinishing=" + isFinishing());
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        Log.w(TAG, "onConfigurationChanged: " + newConfig.diff(getResources().getConfiguration()));
        super.onConfigurationChanged(newConfig);
    }

    @Override
    protected String[] getArguments() {
        if (getIntent().hasExtra("commandLineArgs")) {
            return getIntent().getStringArrayExtra("commandLineArgs");
        }
        return new String[0];
    }
}
