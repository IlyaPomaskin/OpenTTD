package org.openttd.android;

import android.os.Bundle;
import android.view.View;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {

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
    protected String[] getArguments() {
        if (getIntent().hasExtra("commandLineArgs")) {
            return getIntent().getStringArrayExtra("commandLineArgs");
        }
        return new String[0];
    }
}
