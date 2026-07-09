package org.openttd.android;

import android.app.AlertDialog;
import android.app.WallpaperManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.MaterialToolbar;

public class WallpaperSettingsActivity extends AppCompatActivity {

    private TextView txtBrightnessValue;
    private TextView txtIntervalValue;
    private SeekBar seekbarBrightness;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        setContentView(R.layout.activity_wallpaper_settings);

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        // Apply insets to app bar (top) and scroll content (bottom)
        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.app_bar), (v, insets) -> {
            int top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top;
            v.setPadding(v.getPaddingLeft(), top, v.getPaddingRight(), v.getPaddingBottom());
            return insets;
        });
        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.scroll_content), (v, insets) -> {
            int bottom = insets.getInsets(WindowInsetsCompat.Type.systemBars()).bottom;
            v.setPadding(v.getPaddingLeft(), v.getPaddingTop(), v.getPaddingRight(), bottom);
            return insets;
        });

        txtBrightnessValue = findViewById(R.id.txt_brightness_value);
        seekbarBrightness = findViewById(R.id.seekbar_brightness);
        txtIntervalValue = findViewById(R.id.txt_interval_value);
        findViewById(R.id.row_map_interval).setOnClickListener(v -> showIntervalDialog());

        findViewById(R.id.btn_set_wallpaper).setOnClickListener(v -> {
            Intent intent = new Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER);
            intent.putExtra(WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
                new ComponentName(this, OpenTTDWallpaperService.class));
            startActivity(intent);
        });

        seekbarBrightness.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                txtBrightnessValue.setText(progress + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                WallpaperConfig.setBrightness(WallpaperSettingsActivity.this, seekBar.getProgress());
            }
        });

        findViewById(R.id.row_title_maps).setOnClickListener(v ->
            startActivity(new Intent(this, MapsActivity.class)));

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

        refreshAll();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshAll();
    }

    private String[] intervalLabels() {
        return new String[]{
            getString(R.string.interval_every_switch),
            getString(R.string.interval_10m),
            getString(R.string.interval_30m),
            getString(R.string.interval_2h),
            getString(R.string.interval_24h),
        };
    }

    private void showIntervalDialog() {
        int current = WallpaperConfig.getMapUpdateInterval(this);
        new AlertDialog.Builder(this)
            .setTitle(R.string.map_interval_title)
            .setSingleChoiceItems(intervalLabels(), current, (dialog, which) -> {
                WallpaperConfig.setMapUpdateInterval(this, which);
                dialog.dismiss();
                refreshAll();
            })
            .setNegativeButton(R.string.cancel, null)
            .show();
    }

    private void refreshAll() {
        int brightness = WallpaperConfig.getBrightness(this);
        seekbarBrightness.setProgress(brightness);
        txtBrightnessValue.setText(brightness + "%");

        String[] labels = intervalLabels();
        int interval = WallpaperConfig.getMapUpdateInterval(this);
        int idx = (interval >= 0 && interval < labels.length)
            ? interval : WallpaperConfig.DEFAULT_MAP_INTERVAL;
        txtIntervalValue.setText(labels[idx]);
    }
}
