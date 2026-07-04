#!/usr/bin/env python3
"""Build, deploy, and run OpenTTD live wallpaper on Android.

Usage:
    run_android.py                     # full flow: build → install → activate → perf cycles → logs
    run_android.py all [SEC] [CYCLES]  # full flow with SEC seconds per cycle (default 5), CYCLES (default 3)
    run_android.py build               # build APK only
    run_android.py deploy              # build + install only
    run_android.py perf [SEC]          # measure perf on running app for SEC seconds (default 10)
    run_android.py fps [SEC]           # measure FPS on running app for SEC seconds (default 5)
    run_android.py record [SEC]        # record running app for SEC seconds (default 5)
    run_android.py jump                # trigger POI jump on running wallpaper
    run_android.py switch              # trigger map switch on running wallpaper
    run_android.py logs                # dump logcat
"""

import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

PACKAGE = "org.openttd.android"
ACTIVITY = f"{PACKAGE}/.WallpaperSettingsActivity"
PROJECT_DIR = Path(__file__).resolve().parent.parent
APK = PROJECT_DIR / "android/app/build/outputs/apk/debug/app-debug.apk"
LOG_DIR = Path("/tmp/openttd")
DEVICE_PERF_DATA = "/data/local/tmp/perf.data"
JAVA_HOME = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home"

os.environ["JAVA_HOME"] = JAVA_HOME
os.environ["PATH"] = f"{JAVA_HOME}/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:" + os.environ.get("PATH", "")


def sh(cmd):
    """Run a shell command, print it, return exit code."""
    print(f"  $ {cmd}", flush=True)
    return os.system(cmd)


def sh_capture(cmd):
    """Run a shell command and capture stdout via temp file."""
    import tempfile
    print(f"  $ {cmd}", flush=True)
    with tempfile.NamedTemporaryFile(mode='r', suffix='.txt', dir='/tmp', delete=False) as f:
        tmp = f.name
    os.system(f"{cmd} > {tmp} 2>&1")
    try:
        with open(tmp) as f:
            return f.read()
    finally:
        os.unlink(tmp)


def adb(args):
    return sh(f"adb {args}")


def adb_capture(args):
    return sh_capture(f"adb {args}")


def broadcast(action, extras=""):
    adb(f"shell am broadcast -a {PACKAGE}.{action} {extras}")


def vibrate(count=1):
    """Vibrate the device. count=1 single 500ms, count=2 double 300ms pulses."""
    if count <= 1:
        adb("shell cmd vibrator_manager synced -d 1000 oneshot 500 255")
    else:
        adb("shell cmd vibrator_manager synced -d 500 oneshot 300 255")
        time.sleep(0.1)
        adb("shell cmd vibrator_manager synced -d 500 oneshot 300 255")


def get_pid():
    """Get PID of the running app."""
    output = adb_capture(f"shell pidof {PACKAGE}").strip()
    pid = output.split()[0] if output else None
    if pid:
        print(f"  PID: {pid}", flush=True)
    return pid


# --- Commands ---

def build():
    print("==> Building APK...", flush=True)
    sh(f"cd {PROJECT_DIR / 'android'} && ./gradlew assembleDebug")


def install():
    print("==> Stopping app...", flush=True)
    adb(f"shell am force-stop {PACKAGE}")
    print("==> Installing APK...", flush=True)
    adb(f"install {APK}")


def activate():
    print("==> Going to home screen...", flush=True)
    adb("shell input keyevent KEYCODE_HOME")
    time.sleep(1)

    print("==> Opening WallpaperSettingsActivity...", flush=True)
    adb(f"shell am start -n {ACTIVITY}")
    time.sleep(2)

    print("==> Tapping center of screen (Set Wallpaper)...", flush=True)
    output = adb_capture("shell wm size")
    match = re.search(r"(\d+)x(\d+)", output)
    if match:
        w, h = int(match.group(1)), int(match.group(2))
        adb(f"shell input tap {w // 2} {h // 2}")

    print("==> Waiting 2 seconds for warmup...", flush=True)
    time.sleep(2)


def kill():
    print("==> Killing app...", flush=True)
    adb(f"shell am force-stop {PACKAGE}")
    adb("shell am force-stop com.android.wallpaper.livepicker")


def logcat_dump(grep=None):
    output = adb_capture("logcat -d -s OpenTTD")
    if grep:
        return "\n".join(l for l in output.splitlines() if grep in l)
    return output


def cmd_build():
    build()


def cmd_deploy():
    build()
    install()


def cmd_fps(dur=5):
    print(f"==> Measuring FPS for {dur}s...", flush=True)
    adb("logcat -c")
    time.sleep(dur)
    print("--- FPS ---")
    print(logcat_dump("FPS:"))


def cmd_perf(dur=10):
    print(f"==> Collecting perf logs for {dur}s...", flush=True)
    adb("logcat -c")
    time.sleep(dur)
    print("--- PERF LOGS ---")
    print(logcat_dump("PERF "))


def cmd_record(dur=5):
    print(f"==> Recording {dur}s video...", flush=True)
    adb(f"shell screenrecord --time-limit {dur} /sdcard/openttd_rec.mp4")
    adb("pull /sdcard/openttd_rec.mp4 /tmp/openttd_rec.mp4")
    adb("shell rm /sdcard/openttd_rec.mp4")
    print("==> Saved to /tmp/openttd_rec.mp4")


def cmd_jump():
    broadcast("JUMP_POI")


def cmd_switch():
    broadcast("SWITCH_MAP")


def cmd_logs():
    print("==> Dumping logs...", flush=True)
    print(logcat_dump())


def cmd_full(dur=5, cycles=3):
    build()
    install()
    activate()

    timestamp = f"{datetime.now():%Y%m%d_%H%M%S}"
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    pid = get_pid()
    if not pid:
        print("ERROR: could not find app PID")
        return

    adb("logcat -c")
    vibrate(1)

    # Start simpleperf in background for the entire measurement period.
    total_dur = dur * cycles + cycles * 2  # extra seconds for map switch overhead
    print(f"==> Starting simpleperf for ~{total_dur}s...", flush=True)
    sh(f"adb shell simpleperf record --app {PACKAGE} --duration {total_dur} "
       f"-o {DEVICE_PERF_DATA} -g --no-dump-symbols &")

    for i in range(1, cycles + 1):
        print(f"==> Cycle {i}/{cycles}: measuring {dur}s...", flush=True)
        time.sleep(dur)
        print(f"==> Cycle {i}/{cycles}: switching map...", flush=True)
        broadcast("SWITCH_MAP")

    # Wait for simpleperf to finish (it has its own --duration timer).
    remaining = total_dur - dur * cycles
    if remaining > 0:
        print(f"==> Waiting {remaining}s for simpleperf to finish...", flush=True)
        time.sleep(remaining + 1)

    # Pull simpleperf data.
    perf_file = LOG_DIR / f"perf_{timestamp}.data"
    adb(f"pull {DEVICE_PERF_DATA} {perf_file}")
    adb(f"shell rm -f {DEVICE_PERF_DATA}")

    vibrate(2)

    print("--- PERF ---")
    print(logcat_dump("PERF "))

    logfile = LOG_DIR / f"openttd_run_{timestamp}.log"
    logfile.write_text(logcat_dump())

    print(f"FULL LOGS: {logfile}", flush=True)
    if perf_file.exists():
        print(f"SIMPLEPERF FILE: {perf_file}", flush=True)


# --- Main ---

def main():
    args = sys.argv[1:]
    cmd = args[0] if args else "all"

    def int_arg(idx, default):
        return int(args[idx]) if len(args) > idx else default

    commands = {
        "build":  lambda: cmd_build(),
        "deploy": lambda: cmd_deploy(),
        "fps":    lambda: cmd_fps(int_arg(1, 5)),
        "perf":   lambda: cmd_perf(int_arg(1, 10)),
        "record": lambda: cmd_record(int_arg(1, 5)),
        "jump":   lambda: cmd_jump(),
        "switch": lambda: cmd_switch(),
        "logs":   lambda: cmd_logs(),
        "all":    lambda: cmd_full(int_arg(1, 5), int_arg(2, 3)),
    }

    if cmd in commands:
        commands[cmd]()
    else:
        cmd_full(int_arg(1, 5), int_arg(2, 3))


if __name__ == "__main__":
    main()
