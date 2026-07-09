package org.openttd.android;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads/writes the ad-hoc [wallpaper] section of openttd.cfg. This is the
 * cross-process settings channel: the settings Activity (main process) writes;
 * the wallpaper service's native engine (:wallpaper process) reads the same
 * getFilesDir()/openttd.cfg. Only the two [wallpaper] keys are touched; every
 * other line is preserved verbatim. Native is read-only on this file.
 */
public final class WallpaperConfig {
    private static final String TAG = "WallpaperConfig";
    private static final String CFG_NAME = "openttd.cfg";
    private static final String GROUP = "[wallpaper]";
    private static final String KEY_BRIGHTNESS = "brightness";
    private static final String KEY_MAP_INTERVAL = "map_update_interval";

    public static final int DEFAULT_BRIGHTNESS = 100;
    public static final int DEFAULT_MAP_INTERVAL = 2;

    public static final String ACTION_SETTINGS_CHANGED = "org.openttd.android.SETTINGS_CHANGED";

    private WallpaperConfig() {}

    public static int getBrightness(Context context) {
        return readKey(context, KEY_BRIGHTNESS, DEFAULT_BRIGHTNESS);
    }

    public static int getMapUpdateInterval(Context context) {
        return readKey(context, KEY_MAP_INTERVAL, DEFAULT_MAP_INTERVAL);
    }

    public static void setBrightness(Context context, int value) {
        writeKey(context, KEY_BRIGHTNESS, value);
        context.sendBroadcast(new Intent(ACTION_SETTINGS_CHANGED));
    }

    public static void setMapUpdateInterval(Context context, int value) {
        writeKey(context, KEY_MAP_INTERVAL, value);
        context.sendBroadcast(new Intent(ACTION_SETTINGS_CHANGED));
    }

    private static File cfgFile(Context context) {
        return new File(context.getFilesDir(), CFG_NAME);
    }

    private static List<String> readAllLines(File f) {
        List<String> lines = new ArrayList<>();
        if (!f.exists()) return lines;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) lines.add(line);
        } catch (IOException e) {
            Log.w(TAG, "read failed", e);
        }
        return lines;
    }

    private static int readKey(Context context, String key, int def) {
        boolean inGroup = false;
        for (String raw : readAllLines(cfgFile(context))) {
            String line = raw.trim();
            if (line.startsWith("[")) {
                inGroup = line.equalsIgnoreCase(GROUP);
                continue;
            }
            if (!inGroup) continue;
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            if (!line.substring(0, eq).trim().equals(key)) continue;
            try {
                return Integer.parseInt(line.substring(eq + 1).trim());
            } catch (NumberFormatException e) {
                return def;
            }
        }
        return def;
    }

    private static synchronized void writeKey(Context context, String key, int value) {
        File f = cfgFile(context);
        List<String> lines = readAllLines(f);
        String entry = key + " = " + value;

        int groupStart = -1;
        for (int i = 0; i < lines.size(); i++) {
            if (lines.get(i).trim().equalsIgnoreCase(GROUP)) {
                groupStart = i;
                break;
            }
        }

        if (groupStart < 0) {
            if (!lines.isEmpty() && !lines.get(lines.size() - 1).trim().isEmpty()) lines.add("");
            lines.add(GROUP);
            lines.add(entry);
        } else {
            int groupEnd = lines.size();
            for (int i = groupStart + 1; i < lines.size(); i++) {
                if (lines.get(i).trim().startsWith("[")) {
                    groupEnd = i;
                    break;
                }
            }
            boolean replaced = false;
            for (int i = groupStart + 1; i < groupEnd; i++) {
                String line = lines.get(i).trim();
                int eq = line.indexOf('=');
                if (eq > 0 && line.substring(0, eq).trim().equals(key)) {
                    lines.set(i, entry);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) lines.add(groupEnd, entry);
        }

        writeAllLines(f, lines);
    }

    private static void writeAllLines(File f, List<String> lines) {
        File tmp = new File(f.getParentFile(), CFG_NAME + ".tmp");
        try (BufferedWriter w = new BufferedWriter(new FileWriter(tmp))) {
            for (String line : lines) {
                w.write(line);
                w.write("\n");
            }
        } catch (IOException e) {
            Log.e(TAG, "write failed", e);
            return;
        }
        if (!tmp.renameTo(f)) {
            try (BufferedWriter w = new BufferedWriter(new FileWriter(f))) {
                for (String line : lines) {
                    w.write(line);
                    w.write("\n");
                }
            } catch (IOException e) {
                Log.e(TAG, "write-in-place failed", e);
            }
        }
    }
}
