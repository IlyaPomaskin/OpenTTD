#!/usr/bin/env python3
"""Build, deploy, and run OpenTTD live wallpaper on Android.

Usage:
    run_android.py                     # full flow: build → install → activate → perf cycles → logs → kill
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
import subprocess
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

ENV = {
    **os.environ,
    "JAVA_HOME": "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home",
    "PATH": "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin:"
            "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:" + os.environ.get("PATH", ""),
}


def run(cmd, check=True, capture=False, **kwargs):
    """Run a command, print it, return CompletedProcess."""
    print(f"  $ {cmd}")
    return subprocess.run(cmd, shell=True, env=ENV, check=check,
                          capture_output=capture, text=True, **kwargs)


def adb(args, check=True, capture=False):
    return run(f"adb {args}", check=check, capture=capture)


def adb_popen(args):
    """Start adb command in background, return Popen."""
    cmd = f"adb {args}"
    print(f"  $ {cmd} &")
    return subprocess.Popen(cmd, shell=True, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def broadcast(action, extras=""):
    adb(f"shell am broadcast -a {PACKAGE}.{action} {extras}")


def toast(msg):
    """Show a toast message on the device via app broadcast."""
    broadcast("TOAST", f'--es msg "{msg}"')


def get_pid():
    """Get PID of the running app."""
    result = adb(f"shell pidof {PACKAGE}", capture=True, check=False)
    pid = result.stdout.strip().split()[0] if result.stdout.strip() else None
    if pid:
        print(f"  PID: {pid}")
    return pid


# --- Commands ---

def build():
    print("==> Building APK...")
    run("./gradlew assembleDebug", cwd=PROJECT_DIR / "android")


def install():
    print("==> Stopping app...")
    adb(f"shell am force-stop {PACKAGE}", check=False)
    print("==> Installing APK...")
    adb(f"install {APK}")


def activate():
    print("==> Going to home screen...")
    adb("shell input keyevent KEYCODE_HOME")
    time.sleep(1)

    print("==> Opening WallpaperSettingsActivity...")
    adb(f"shell am start -n {ACTIVITY}")
    time.sleep(2)

    print("==> Tapping center of screen (Set Wallpaper)...")
    result = adb("shell wm size", capture=True)
    match = re.search(r"(\d+)x(\d+)", result.stdout)
    if match:
        w, h = int(match.group(1)), int(match.group(2))
        adb(f"shell input tap {w // 2} {h // 2}")

    print("==> Waiting 2 seconds for warmup...")
    time.sleep(2)


def kill():
    print("==> Killing app...")
    adb(f"shell am force-stop {PACKAGE}", check=False)
    adb("shell am force-stop com.android.wallpaper.livepicker", check=False)


def logcat_dump(grep=None):
    result = adb("logcat -d -s OpenTTD", capture=True)
    if grep:
        return "\n".join(l for l in result.stdout.splitlines() if grep in l)
    return result.stdout


def cmd_build():
    build()


def cmd_deploy():
    build()
    install()


def cmd_fps(dur=5):
    print(f"==> Measuring FPS for {dur}s...")
    adb("logcat -c")
    time.sleep(dur)
    print("--- FPS ---")
    print(logcat_dump("FPS:"))


def cmd_perf(dur=10):
    print(f"==> Collecting perf logs for {dur}s...")
    adb("logcat -c")
    time.sleep(dur)
    print("--- PERF LOGS ---")
    print(logcat_dump("PERF "))


def cmd_record(dur=5):
    print(f"==> Recording {dur}s video...")
    adb(f"shell screenrecord --time-limit {dur} /sdcard/openttd_rec.mp4")
    adb("pull /sdcard/openttd_rec.mp4 /tmp/openttd_rec.mp4")
    adb("shell rm /sdcard/openttd_rec.mp4")
    print("==> Saved to /tmp/openttd_rec.mp4")


def cmd_jump():
    broadcast("JUMP_POI")


def cmd_switch():
    broadcast("SWITCH_MAP")


def cmd_logs():
    print("==> Dumping logs...")
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
        kill()
        return

    adb("logcat -c")
    toast("monitoring start")

    # Start simpleperf in background for the entire measurement period.
    total_dur = dur * cycles + cycles * 2  # extra seconds for map switch overhead
    print(f"==> Starting simpleperf for ~{total_dur}s on PID {pid}...")
    simpleperf_proc = adb_popen(
        f"shell simpleperf record -p {pid} --duration {total_dur} "
        f"-o {DEVICE_PERF_DATA} -g --no-dump-symbols"
    )

    for i in range(1, cycles + 1):
        print(f"==> Cycle {i}/{cycles}: measuring {dur}s...")
        time.sleep(dur)
        print(f"==> Cycle {i}/{cycles}: switching map...")
        broadcast("SWITCH_MAP")

    # Wait for simpleperf to finish.
    print("==> Waiting for simpleperf to finish...")
    simpleperf_proc.wait()

    # Pull simpleperf data.
    perf_file = LOG_DIR / f"perf_{timestamp}.data"
    adb(f"pull {DEVICE_PERF_DATA} {perf_file}", check=False)
    adb(f"shell rm -f {DEVICE_PERF_DATA}", check=False)

    toast("monitoring done")

    print("--- PERF ---")
    print(logcat_dump("PERF "))

    logfile = LOG_DIR / f"openttd_run_{timestamp}.log"
    logfile.write_text(logcat_dump())

    print(f"FULL LOGS: {logfile}")
    if perf_file.exists():
        print(f"SIMPLEPERF FILE: {perf_file}")


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
