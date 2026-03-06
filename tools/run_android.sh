#!/bin/bash
# Build, deploy, and run OpenTTD live wallpaper on Android.
# Usage:
#   ./tools/run_android.sh              # full flow: build → install → activate → perf → logs → kill
#   ./tools/run_android.sh all [SEC]    # full flow with SEC seconds measurement (default 5)
#   ./tools/run_android.sh perf [SEC]   # measure perf on running app for SEC seconds (default 10)
#   ./tools/run_android.sh fps [SEC]    # measure FPS on running app for SEC seconds (default 5)
#   ./tools/run_android.sh record [SEC] # record running app for SEC seconds (default 5)
#   ./tools/run_android.sh jump          # trigger POI jump on running wallpaper
#   ./tools/run_android.sh switch        # trigger map switch on running wallpaper
#   ./tools/run_android.sh logs         # tail logcat
#   ./tools/run_android.sh deploy       # build + install only (no activate/measure)

set -e

export JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:$PATH"

PACKAGE="org.openttd.android"
ACTIVITY="$PACKAGE/.WallpaperSettingsActivity"
APK="android/app/build/outputs/apk/debug/app-debug.apk"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

cd "$SCRIPT_DIR"

do_fps() {
    local dur=${1:-5}
    echo "==> Measuring FPS for ${dur}s..."
    adb logcat -c
    sleep "$dur"
    echo "--- FPS ---"
    adb logcat -d -s OpenTTD | grep "FPS:"
}

do_perf() {
    local dur=${1:-10}
    echo "==> Collecting perf logs for ${dur}s..."
    adb logcat -c
    sleep "$dur"
    echo "--- PERF LOGS ---"
    adb logcat -d -s OpenTTD | grep "PERF "
}

do_record() {
    local dur=${1:-5}
    echo "==> Recording ${dur}s video..."
    adb shell screenrecord --time-limit "$dur" /sdcard/openttd_rec.mp4
    adb pull /sdcard/openttd_rec.mp4 /tmp/openttd_rec.mp4
    adb shell rm /sdcard/openttd_rec.mp4
    echo "==> Saved to /tmp/openttd_rec.mp4"
}

do_logs() {
    echo "==> Dumping logs..."
    adb logcat -d -s OpenTTD
}

do_build_install() {
    echo "==> Building APK..."
    cd "$SCRIPT_DIR/android" && ./gradlew assembleDebug
    cd "$SCRIPT_DIR"

    echo "==> Stopping app..."
    adb shell am force-stop "$PACKAGE" || true

    echo "==> Installing APK..."
    adb install "$APK"
}

do_activate() {
    echo "==> Going to home screen..."
    adb shell input keyevent KEYCODE_HOME
    sleep 1

    echo "==> Opening WallpaperSettingsActivity..."
    adb shell am start -n "$ACTIVITY"
    sleep 2

    echo "==> Tapping center of screen (Set Wallpaper)..."
    SIZE=$(adb shell wm size | grep -o '[0-9]*x[0-9]*' | tail -1)
    W=${SIZE%x*}
    H=${SIZE#*x}
    adb shell input tap $((W / 2)) $((H / 2))

    echo "==> Waiting 2 seconds for warmup..."
    sleep 2
}

do_kill() {
    echo "==> Killing app..."
    adb shell am force-stop "$PACKAGE" || true
    adb shell am force-stop com.android.wallpaper.livepicker || true
}

# Standalone commands
case "${1:-}" in
    fps)    do_fps "${2:-5}";    exit 0 ;;
    perf)   do_perf "${2:-10}";  exit 0 ;;
    record) do_record "${2:-5}"; exit 0 ;;
    jump)   adb shell am broadcast -a org.openttd.android.JUMP_POI; exit 0 ;;
    switch) adb shell am broadcast -a org.openttd.android.SWITCH_MAP; exit 0 ;;
    logs)   do_logs;             exit 0 ;;
    deploy) do_build_install;    exit 0 ;;
esac

# Full flow
DUR=${2:-5}
CYCLES=${3:-3}
LOGFILE="/tmp/openttd/openttd_run_$(date +%Y%m%d_%H%M%S).log"
mkdir -p /tmp/openttd

do_build_install
do_activate

adb logcat -c

for i in $(seq 1 "$CYCLES"); do
    echo "==> Cycle $i/$CYCLES: measuring ${DUR}s..."
    sleep "$DUR"
    echo "==> Cycle $i/$CYCLES: switching map..."
    adb shell am broadcast -a org.openttd.android.SWITCH_MAP
done

echo "--- PERF ---"
adb logcat -d -s OpenTTD | grep "PERF "

adb logcat -d -s OpenTTD > "$LOGFILE"

do_kill
echo "FULL LOGS: $LOGFILE"
