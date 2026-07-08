package org.openttd.android;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

public class SettingsHelper {
    private static final String PREFS_NAME = "wallpaper_prefs";

    public static final String KEY_MAP_INTERVAL = "map_update_interval";
    public static final String KEY_BRIGHTNESS = "brightness";
    public static final String KEY_TITLE_ASSETS_PROVISIONED = "title_assets_provisioned";

    public static final int DEFAULT_MAP_INTERVAL = 2;
    public static final int DEFAULT_BRIGHTNESS = 100;

    public static final String ACTION_SETTINGS_CHANGED = "org.openttd.android.SETTINGS_CHANGED";
    public static final String ACTION_TITLE_MAPS_CHANGED = "org.openttd.android.TITLE_MAPS_CHANGED";

    public static SharedPreferences getPrefs(Context context) {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    public static int getMapUpdateInterval(Context context) {
        return getPrefs(context).getInt(KEY_MAP_INTERVAL, DEFAULT_MAP_INTERVAL);
    }

    public static void setMapUpdateInterval(Context context, int value) {
        getPrefs(context).edit().putInt(KEY_MAP_INTERVAL, value).apply();
        notifySettingsChanged(context);
    }

    public static int getBrightness(Context context) {
        return getPrefs(context).getInt(KEY_BRIGHTNESS, DEFAULT_BRIGHTNESS);
    }

    public static void setBrightness(Context context, int value) {
        getPrefs(context).edit().putInt(KEY_BRIGHTNESS, value).apply();
        notifySettingsChanged(context);
    }

    public static boolean isTitleAssetsProvisioned(Context context) {
        return getPrefs(context).getBoolean(KEY_TITLE_ASSETS_PROVISIONED, false);
    }

    public static void setTitleAssetsProvisioned(Context context) {
        getPrefs(context).edit().putBoolean(KEY_TITLE_ASSETS_PROVISIONED, true).apply();
    }

    public static void notifySettingsChanged(Context context) {
        Intent intent = new Intent(ACTION_SETTINGS_CHANGED);
        intent.putExtra(KEY_MAP_INTERVAL, getMapUpdateInterval(context));
        intent.putExtra(KEY_BRIGHTNESS, getBrightness(context));
        context.sendBroadcast(intent);
    }
}
