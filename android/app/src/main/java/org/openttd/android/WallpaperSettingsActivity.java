package org.openttd.android;

import android.app.WallpaperManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

import androidx.appcompat.app.AppCompatActivity;

public class WallpaperSettingsActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_wallpaper_settings);

        findViewById(R.id.btn_set_wallpaper).setOnClickListener(v -> {
            Intent intent = new Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER);
            intent.putExtra(WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
                new ComponentName(this, OpenTTDWallpaperService.class));
            startActivity(intent);
        });

        findViewById(R.id.btn_debug_game).setOnClickListener(v -> {
            MainActivity.copyAssetsStatic(getApplicationContext());
            try {
                android.system.Os.setenv("OPENTTD_DATA_PATH",
                    getFilesDir().getAbsolutePath(), true);
            } catch (android.system.ErrnoException e) { /* ignore */ }
            Intent intent = new Intent(this, GameActivity.class);
            intent.putExtra("commandLineArgs", new String[]{"-s", "null", "-m", "null"});
            startActivity(intent);
        });
    }
}
