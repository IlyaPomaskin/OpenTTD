package org.openttd.android;

import android.app.AlertDialog;
import android.app.WallpaperManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

public class WallpaperSettingsActivity extends AppCompatActivity {

    private TextView txtMapIntervalValue;
    private TextView txtMapZoomValue;
    private TextView txtBrightnessValue;
    private SeekBar seekbarBrightness;

    private final String[] intervalLabels = new String[5];
    private final String[] zoomLabels = {"1x", "2x", "4x"};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_wallpaper_settings);

        intervalLabels[0] = getString(R.string.interval_every_switch);
        intervalLabels[1] = getString(R.string.interval_10m);
        intervalLabels[2] = getString(R.string.interval_30m);
        intervalLabels[3] = getString(R.string.interval_2h);
        intervalLabels[4] = getString(R.string.interval_24h);

        txtMapIntervalValue = findViewById(R.id.txt_map_interval_value);
        txtMapZoomValue = findViewById(R.id.txt_map_zoom_value);
        txtBrightnessValue = findViewById(R.id.txt_brightness_value);
        seekbarBrightness = findViewById(R.id.seekbar_brightness);

        findViewById(R.id.btn_set_wallpaper).setOnClickListener(v -> {
            Intent intent = new Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER);
            intent.putExtra(WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
                new ComponentName(this, OpenTTDWallpaperService.class));
            startActivity(intent);
        });

        findViewById(R.id.row_map_interval).setOnClickListener(v -> {
            int current = SettingsHelper.getMapUpdateInterval(this);
            new AlertDialog.Builder(this)
                .setTitle(R.string.map_interval_title)
                .setSingleChoiceItems(intervalLabels, current, (dialog, which) -> {
                    SettingsHelper.setMapUpdateInterval(this, which);
                    updateIntervalDisplay();
                    dialog.dismiss();
                })
                .show();
        });

        findViewById(R.id.row_map_zoom).setOnClickListener(v -> {
            int currentZoom = SettingsHelper.getMapZoom(this);
            int currentIndex = 0;
            for (int i = 0; i < SettingsHelper.ZOOM_VALUES.length; i++) {
                if (SettingsHelper.ZOOM_VALUES[i] == currentZoom) { currentIndex = i; break; }
            }
            new AlertDialog.Builder(this)
                .setTitle(R.string.map_zoom_title)
                .setSingleChoiceItems(zoomLabels, currentIndex, (dialog, which) -> {
                    SettingsHelper.setMapZoom(this, SettingsHelper.ZOOM_VALUES[which]);
                    updateZoomDisplay();
                    dialog.dismiss();
                })
                .show();
        });

        seekbarBrightness.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                txtBrightnessValue.setText(progress + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                SettingsHelper.setBrightness(WallpaperSettingsActivity.this, seekBar.getProgress());
            }
        });

        findViewById(R.id.row_title_maps).setOnClickListener(v ->
            startActivity(new Intent(this, MapsActivity.class)));
        findViewById(R.id.row_game_assets).setOnClickListener(v ->
            startActivity(new Intent(this, AssetSetupActivity.class)));

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

    private void refreshAll() {
        updateIntervalDisplay();
        updateZoomDisplay();
        int brightness = SettingsHelper.getBrightness(this);
        seekbarBrightness.setProgress(brightness);
        txtBrightnessValue.setText(brightness + "%");
    }

    private void updateIntervalDisplay() {
        int idx = SettingsHelper.getMapUpdateInterval(this);
        if (idx >= 0 && idx < intervalLabels.length) {
            txtMapIntervalValue.setText(intervalLabels[idx]);
        }
    }

    private void updateZoomDisplay() {
        int zoom = SettingsHelper.getMapZoom(this);
        for (int i = 0; i < SettingsHelper.ZOOM_VALUES.length; i++) {
            if (SettingsHelper.ZOOM_VALUES[i] == zoom) {
                txtMapZoomValue.setText(zoomLabels[i]);
                return;
            }
        }
        txtMapZoomValue.setText(zoomLabels[0]);
    }
}
