package org.openttd.android;

import android.content.Context;
import android.content.SharedPreferences;

/** App-state prefs that are NOT wallpaper settings. Wallpaper settings live in
 *  openttd.cfg via {@link WallpaperConfig}. */
public class SettingsHelper {
    private static final String PREFS_NAME = "wallpaper_prefs";

    public static final String KEY_TITLE_ASSETS_PROVISIONED = "title_assets_provisioned";

    public static final String ACTION_TITLE_MAPS_CHANGED = "org.openttd.android.TITLE_MAPS_CHANGED";

    public static SharedPreferences getPrefs(Context context) {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    public static boolean isTitleAssetsProvisioned(Context context) {
        return getPrefs(context).getBoolean(KEY_TITLE_ASSETS_PROVISIONED, false);
    }

    public static void setTitleAssetsProvisioned(Context context) {
        getPrefs(context).edit().putBoolean(KEY_TITLE_ASSETS_PROVISIONED, true).apply();
    }
}
