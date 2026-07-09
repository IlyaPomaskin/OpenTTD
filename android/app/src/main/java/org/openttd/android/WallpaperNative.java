package org.openttd.android;

/**
 * The single Java-to-native command surface for the wallpaper engine.
 * Both the wallpaper service and GameActivity forward every command through
 * {@link #nativeWallpaperCommand}. Command names are strings (the shared
 * contract); native ignores an unknown name (forward-compatible).
 */
public final class WallpaperNative {
    public static final String CMD_PREPARE_BG = "PREPARE_BG";
    public static final String CMD_ROTATE_MAP = "ROTATE_MAP";
    public static final String CMD_NAVIGATE_POI = "NAVIGATE_POI";
    public static final String CMD_SCROLL_CAMERA = "SCROLL_CAMERA";
    public static final String CMD_SET_PAUSED = "SET_PAUSED";
    public static final String CMD_SURFACE_CHANGED = "SURFACE_CHANGED";
    public static final String CMD_RELOAD_SETTINGS = "RELOAD_SETTINGS";
    public static final String CMD_REFRESH_TITLE_MAPS = "REFRESH_TITLE_MAPS";

    private WallpaperNative() {}

    /** arg1/arg2 meaning is per-command (see the command table in the spec). */
    public static native void nativeWallpaperCommand(String command, int arg1, int arg2);
}
