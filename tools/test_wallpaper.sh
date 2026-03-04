#!/bin/bash
# Test OpenTTD live wallpaper: build, install, set as wallpaper, screenshot.
# Usage: ./tools/test_wallpaper.sh

set -e

export JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:$PATH"

PACKAGE="org.openttd.android"
ACTIVITY="$PACKAGE/.WallpaperSettingsActivity"
APK="android/app/build/outputs/apk/debug/app-debug.apk"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCREENSHOT_DIR="/tmp/openttd"

mkdir -p "$SCREENSHOT_DIR"

cd "$SCRIPT_DIR"

SIZE=$(adb shell wm size | grep -o '[0-9]*x[0-9]*' | tail -1)
W=${SIZE%x*}
H=${SIZE#*x}
CX=$((W / 2))
CY=$((H / 2))
SET_BTN_X=640
SET_BTN_Y=2707  # "Set wallpaper" button center (from uiautomator: [484,2640][796,2775])
DIALOG_X=640
DIALOG_Y=1681   # "Home screen and lock screen" dialog item center (from uiautomator: [119,1620][1161,1742])

# Step 1: Kill all OpenTTD processes
echo "==> Killing existing OpenTTD processes..."
adb shell am force-stop "$PACKAGE" || true
adb shell am force-stop com.android.wallpaper.livepicker || true
sleep 1

# Step 2: Build APK
echo "==> Building APK..."
cd "$SCRIPT_DIR/android" && ./gradlew assembleDebug
cd "$SCRIPT_DIR"

# Step 3: Install APK
echo "==> Installing APK..."
adb install "$APK"

# Step 4: Launch WallpaperSettingsActivity
echo "==> Launching WallpaperSettingsActivity..."
adb shell am start -n "$ACTIVITY"
sleep 1

# Step 5: Tap center of screen
echo "==> Tapping center ($CX, $CY)..."
adb shell input tap "$CX" "$CY"
sleep 3

# Step 6: Screenshot preview
echo "==> Taking screenshot: preview..."
adb shell screencap -p /sdcard/openttd_preview.png
adb pull /sdcard/openttd_preview.png "$SCREENSHOT_DIR/preview.png"
adb shell rm /sdcard/openttd_preview.png
echo "SCREENSHOT1: $SCREENSHOT_DIR/preview.png"

# Step 7: Tap "Set Wallpaper" button
echo "==> Tapping 'Set Wallpaper' ($SET_BTN_X, $SET_BTN_Y)..."
adb shell input tap "$SET_BTN_X" "$SET_BTN_Y"
sleep 2

# Step 7b: Tap "Home screen and lock screen" in dialog (if it appears)
echo "==> Tapping 'Home screen and lock screen' ($DIALOG_X, $DIALOG_Y)..."
adb shell input tap "$DIALOG_X" "$DIALOG_Y"
sleep 2


# Step 8: Go to home screen
echo "==> Going to home screen..."
adb shell input keyevent KEYCODE_HOME
sleep 3

# Step 9: Screenshot home screen
echo "==> Taking screenshot: home screen..."
adb shell screencap -p /sdcard/openttd_home.png
adb pull /sdcard/openttd_home.png "$SCREENSHOT_DIR/home.png"
adb shell rm /sdcard/openttd_home.png
echo "SCREENSHOT2: $SCREENSHOT_DIR/home.png"

# Step 10: Check if home screen looks black (small PNG = mostly black)
HOME_SIZE=$(wc -c < "$SCREENSHOT_DIR/home.png")
PREVIEW_SIZE=$(wc -c < "$SCREENSHOT_DIR/preview.png")
echo "==> File sizes: preview=${PREVIEW_SIZE}b home=${HOME_SIZE}b"

if [ "$HOME_SIZE" -lt $((PREVIEW_SIZE / 3)) ]; then
    echo "==> WARNING: Home screen PNG is suspiciously small (likely black). Dumping ADB logs..."
    echo "--- OpenTTD logs ---"
    adb logcat -d -s OpenTTD | tail -50
    echo "--- Wallpaper service errors ---"
    adb logcat -d | grep -i "wallpaper\|openttd\|gles\|fatal\|crash" | tail -30
fi

echo "==> Done. Screenshots saved to $SCREENSHOT_DIR/"
