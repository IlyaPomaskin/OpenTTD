# Dev Tooling (tools/)

Observability layer for `_gles_perf` counters — the C++ PERF log format (rendering.md) and these parsers are a matched pair. All PERF lines logged every 500ms at debug level 3, tag `OpenTTD`.

## run_android.py — main automation

`/usr/bin/python3 tools/run_android.py [cmd]` (homebrew python blocked by sandbox).

| Cmd | Does |
|---|---|
| *(none)* / `all [SEC] [CYCLES]` | build → install → activate wallpaper → simpleperf + SWITCH_MAP cycles → save logs (`/tmp/openttd/openttd_run_*.log`, `perf_*.data`) |
| `build` / `deploy` | gradle assembleDebug / + adb install |
| `perf [SEC]` / `fps [SEC]` | collect PERF / FPS logcat lines |
| `record [SEC]` | screen video |
| `jump` / `switch` | JUMP_POI / SWITCH_MAP broadcast |
| `logs` | logcat dump |

Helpers: device vibrate feedback, pid lookup, wallpaper activation via `WallpaperSettingsActivity`.

## perf_monitor.py — live TUI dashboard + remote

`python3 tools/perf_monitor.py [log.txt]` — live adb logcat, file, or pipe.

- Parses PERF/VP/TICK/CPU/EXTRA/GPU_SNAP lines → braille-graph history per metric.
- 9 screens (keys 0–8): screen 0 = hotkey remote (i/o map ±, t/y POI ±, h/j/k/u scroll — all via adb broadcasts), screens 1–8 hierarchical metric groups (frame time, GPU, snapshot stages, PBO, atlas, gameloop...).

## perf_stats.py — offline statistics

`perf_stats.py [seconds | - | file.log]` — collects from adb for N sec (default 10), stdin, or file. Per-metric min/avg/max/p95 for PERF, VP, TICK sections.

## perf_collect.sh

Wrapper: collect N sec (default 20) from device → run perf_stats.

## test_wallpaper.sh

E2E smoke test: build → install → set live wallpaper → screenshot to `/tmp/openttd/`. Hardcoded JAVA_HOME (temurin-25).

## extract_vehicles.py

Dump all sprites from a GRF (container v1+v2) to `sprites-original/*.png`. Used to debug sprite decode/atlas pipeline. Default input `/tmp/openttd/baseset/ogfx1_base.grf`.

## Dev environment files (repo root)

- `CLAUDE.md` — Claude CLI project instructions: sandbox dirs, build commands (`cmake -B /tmp/openttd`, `-j4`), run_android usage, adb broadcast reference.
- `claude-cli.sb` — macOS `sandbox-exec` profile restricting the CLI to project/build/tmp dirs.

Both dev-only; drop or keep as-is on reimpl.
