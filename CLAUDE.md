# Claude CLI — OpenTTD Project Instructions

In all interactions and commit messages, always be extremely concise and sacrifice grammar for the sake of concision.

## Sandbox Environment

This session runs inside a macOS sandbox (`sandbox-exec`). Filesystem access is restricted to specific directories only.

### Allowed directories

| Path | Access | Purpose |
|---|---|---|
| `~/work/OpenTTD` | read/write | Project source code |
| `/tmp/openttd` | read/write | Build directory |
| `~/.claude/` | read/write | Claude CLI config |
| `~/.cache/` | read/write | Cache |
| `/tmp/` | read/write | Temporary files |

### Forbidden directories

Everything outside the allowed list is blocked by the sandbox, including but not limited to:
- `~/.ssh/`
- `~/Documents/`, `~/Desktop/`, `~/Downloads/`
- Any other project directories

### Sandbox errors

If any command fails with `Operation not permitted`, this is a **sandbox restriction**. When this happens:

1. **Do not retry** the same command — it will fail again.
2. **Report the error to the user** immediately, specifying which path or operation was blocked.
3. **Suggest an alternative** that uses an allowed directory (e.g. use `/tmp/openttd` instead of an arbitrary temp path).

## Build

Build directory: `/tmp/openttd`

```sh
cmake -B /tmp/openttd -S ~/work/OpenTTD
cmake --build /tmp/openttd -j4
```

**Important:** Always use `-j4` for macOS builds — do not use `$(sysctl -n hw.ncpu)` or other dynamic core detection.

### Running app

To start app use binary from inside app.

## Android

### Python must use system interpreter

Always use `/usr/bin/python3` for Python scripts. Homebrew python3 is blocked by sandbox.

### run_android.py

Main automation script for Android development: `tools/run_android.py`

```sh
/usr/bin/python3 tools/run_android.py [command] [args...]
```

| Command | Description |
|---|---|
| *(no args)* / `all [SEC] [CYCLES]` | Full flow: build, install, activate wallpaper, simpleperf + map switch cycles, save logs |
| `build` | Build APK only |
| `deploy` | Build + install APK |
| `perf [SEC]` | Collect PERF log lines for SEC seconds (default 10) |
| `fps [SEC]` | Collect FPS log lines for SEC seconds (default 5) |
| `record [SEC]` | Record screen video for SEC seconds |
| `jump` | Send JUMP_POI broadcast (move camera to next POI) |
| `switch` | Send SWITCH_MAP broadcast (regenerate map) |
| `logs` | Dump logcat |

Full flow outputs:
- `FULL LOGS: /tmp/openttd/openttd_run_*.log` — all app logs
- `SIMPLEPERF FILE: /tmp/openttd/perf_*.data` — CPU profile

### adb broadcast commands

Control the running wallpaper service remotely:

```sh
adb shell am broadcast -a org.openttd.android.JUMP_POI      # move camera to next POI
adb shell am broadcast -a org.openttd.android.SWITCH_MAP     # regenerate map
adb shell am broadcast -a org.openttd.android.TOAST --es msg "text"  # show toast
```