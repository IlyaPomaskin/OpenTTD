#!/usr/bin/env python3
"""
OpenTTD GLES performance monitor & wallpaper remote control.
Reads PERF logs from adb or file, draws live braille graphs.
Screen 0 (default): hotkeys for map/POI/camera control via adb.
Screens 1-8: performance metrics.

Usage:
    python3 tools/perf_monitor.py              # live adb logcat
    python3 tools/perf_monitor.py log.txt      # from file
    adb logcat | python3 tools/perf_monitor.py # pipe
"""

import sys
import re
import subprocess
import shutil
import tty
import termios
import select
import os

# ── Braille dot plotting ────────────────────────────────────────────
BRAILLE_BASE = 0x2800
BRAILLE_DOTS = {
    (0, 0): 0x01, (0, 1): 0x02, (0, 2): 0x04, (0, 3): 0x40,
    (1, 0): 0x08, (1, 1): 0x10, (1, 2): 0x20, (1, 3): 0x80,
}
GRAPH_ROWS = 3

# ── Colors ──────────────────────────────────────────────────────────
C_R = "\033[0m"
C_L = "\033[36m"      # label
C_V = "\033[33m"      # value
C_D = "\033[2m"       # dim
C_G = "\033[32m"      # graph
C_B = "\033[1m"       # bold
C_H = "\033[1;37;44m" # tab highlight
C_T = "\033[2;37m"    # tab normal
C_SUB = "\033[36;2m"  # sub-metric label

HISTORY = 120

# ── ADB broadcast helpers ──────────────────────────────────────────
_ADB_PKG = "org.openttd.android"
SCROLL_PX = 300

def _adb_broadcast(action, extras=""):
    cmd = f"adb shell am broadcast -a {_ADB_PKG}.{action}"
    if extras:
        cmd += " " + extras
    subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ── 9 screens with hierarchical metrics ─────────────────────────────
# (key, display_name, unit, max_hint, is_child)
# All time values displayed in ms. C++ emits us — converted in process_line.
SCREENS = {
    "0": {
        "title": "CTRL — Hotkeys",
        "metrics": [],
    },
    "1": {
        "title": "PERF — Main",
        "metrics": [
            ("fps",              "FPS",              "",   80,    False),
            ("frames",           "-frames",          "",   80,    True),
            ("gpu_paint",        "GPU paint",        "ms", 20,    False),
            ("egl_swap",         "-EGL swap",        "ms", 20,    True),
            ("gpu_cmds",         "GPU cmds",         "",   15000, False),
            ("gl_batches",       "-GL batches",      "",   2000,  True),
            ("overdraw",         "Overdraw",         "x",  100,   False),
            ("batch_eff",        "-batch eff",       "",   50,    True),
            ("cache_hit",        "-cache hit",       "%",  100,   True),
        ],
    },
    "2": {
        "title": "VP — Viewport",
        "metrics": [
            ("landscape",        "VP land",          "ms", 200,   False),
            ("ground_sprites",   "-ground sprites",  "ms", 30,    True),
            ("vehicles",         "VP vehicles",      "ms", 10,    False),
            ("sprite_sort",      "Sprite sort",      "ms", 30,    False),
            ("sprite_draw",      "Sprite draw",      "ms", 20,    False),
            ("tiles_iterated",   "-tiles",           "",   20000, True),
            ("parent_sprites",   "-parent sprites",  "",   5000,  True),
            ("child_sprites",    "-child sprites",   "",   5000,  True),
            ("full_renders",     "Full renders",     "",   60,    False),
            ("resolve_only",     "-resolve only",    "",   60,    True),
            ("idle_blit",        "-idle blit",       "",   60,    True),
        ],
    },
    "3": {
        "title": "TICK — Draw Thread",
        "metrics": [
            ("total",            "Tick total",       "ms", 30,    False),
            ("lock_video",       "-lock video",      "ms", 5,     True),
            ("game_mutex",       "-game mutex",      "ms", 15,    True),
            ("input_poll",       "-input poll",      "ms", 2,     True),
            ("update_windows",   "-update windows",  "ms", 20,    True),
            ("populate_sprites", "-populate sprites","ms", 5,     True),
            ("check_palette",    "-check palette",   "ms", 1,     True),
            ("paint",            "-paint",           "ms", 20,    True),
            ("unlock_video",     "-unlock video",    "ms", 2,     True),
            ("unaccounted",      "-unaccounted",     "ms", 5,     True),
        ],
    },
    "4": {
        "title": "CPU — Game Thread",
        "metrics": [
            ("gameloop",         "Gameloop",         "ms", 15,    False),
            ("vehtick",          "-veh tick",        "ms", 10,    True),
            ("tileloop",         "-tile loop",       "ms", 5,     True),
            ("snap_total",       "Snap total",       "ms", 15,    False),
            ("record",           "-record",          "ms", 10,    True),
            ("validate",         "-validate",        "ms", 2,     True),
            ("cmds",             "-commands",        "",   500,   True),
            ("ticks",            "Game ticks",       "",   80,    False),
        ],
    },
    "5": {
        "title": "EXTRA — Atlas & Quality",
        "metrics": [
            ("gpu_actual",       "GPU actual",       "ms", 20,    False),
            ("atlas_miss",       "Atlas miss",       "",   100,   False),
            ("reuploaded",       "-reuploaded",      "",   100,   True),
            ("gpu_sprites_new",  "-new sprites",     "",   100,   True),
            ("color",            "Atlas color",      "%",  100,   False),
            ("remap",            "-atlas remap",     "%",  100,   True),
            ("jank",             "Jank",             "",   10,    False),
            ("stddev",           "-stddev",          "ms", 10,    True),
            ("p95",              "-P95",             "ms", 50,    True),
            ("p99",              "-P99",             "ms", 50,    True),
        ],
    },
    "6": {
        "title": "SPRITE — Loading Pipeline",
        "metrics": [
            ("cache_hits",       "Cache hits",       "",   5000,  False),
            ("loads",            "Disk loads",       "",   500,   False),
            ("disk",             "Disk read",        "ms", 50,    False),
            ("resize",           "-resize",          "ms", 20,    True),
            ("encode",           "-encode",          "ms", 20,    True),
            ("stage",            "-stage",           "ms", 10,    True),
            ("upload_count",     "GPU uploads",      "",   500,   False),
            ("upload",           "-upload time",     "ms", 20,    True),
            ("staged_count",     "Staged sprites",   "",   5000,  False),
            ("staged_kb",        "-staged RAM",      "KB", 100000,False),
        ],
    },
    "7": {
        "title": "PBO — Async Upload",
        "metrics": [
            ("rounds",           "PBO rounds",       "",   10,    False),
            ("batches_sub",      "-batches submit",  "",   10,    True),
            ("done",             "-batches done",    "",   10,    True),
            ("sprites",          "Sprites uploaded",  "",  2000,  False),
            ("fill",             "PBO fill",         "ms", 10,    False),
            ("submit",           "-PBO submit",      "ms", 10,    True),
            ("fence_waits",      "Fence waits",      "",   10,    False),
            ("candidates",       "Candidates",       "",   5000,  False),
            ("already_up",       "-already up",      "",   50000, True),
            ("staged_gone",      "-staged gone",     "",   100,   True),
            ("pbo_full",         "-PBO full",        "",   100,   True),
            ("pack_fail_c",      "-pack fail color", "",   10,    True),
            ("pack_fail_r",      "-pack fail remap", "",   10,    True),
            ("map_fail",         "-map fail",        "",   10,    True),
            ("alloc_pages",      "Alloc pages",      "",   10,    False),
        ],
    },
    "8": {
        "title": "GPU_SNAP — Snapshot Replay",
        "metrics": [
            ("snap_clear",       "Atlas clear",      "ms", 1,     False),
            ("snap_pbo",         "PBO process",      "ms", 5,     False),
            ("snap_replay",      "Cmd replay",       "ms", 10,    False),
            ("snap_cmds",        "-replayed cmds",   "",   15000, True),
            ("snap_null",        "-null entries",    "",   100,   True),
            ("snap_palette",     "Palette upload",   "ms", 2,     False),
            ("snap_paint",       "PaintFBO",         "ms", 20,    False),
            ("snap_blit",        "Blit to screen",   "ms", 10,    False),
            ("snap_swap",        "EGL swap",         "ms", 10,    False),
        ],
    },
}


# Keys whose log values are in us — convert to ms for display.
_US_KEYS = set()
for _s in SCREENS.values():
    for _key, _, _unit, _, _ in _s["metrics"]:
        if _unit == "ms":
            _US_KEYS.add(_key)


def parse_kv(line):
    result = {}
    for m in re.finditer(r'(\w+)=([\d.]+)', line):
        key, val = m.group(1), m.group(2)
        try:
            v = float(val) if '.' in val else int(val)
            if key in _US_KEYS:
                v = v / 1000.0
            result[key] = v
        except ValueError:
            pass
    return result


def braille_graph(values, max_val, width, height_chars):
    if not values or max_val <= 0:
        return [" " * width] * height_chars

    dot_rows = height_chars * 4
    dot_cols = width * 2
    grid = [[False] * dot_cols for _ in range(dot_rows)]

    n = len(values)
    for i, v in enumerate(values):
        x = int(i * (dot_cols - 1) / max(n - 1, 1)) if n > 1 else dot_cols // 2
        y = int(min(v / max_val, 1.0) * (dot_rows - 1))
        if 0 <= x < dot_cols and 0 <= y < dot_rows:
            grid[y][x] = True
            if i > 0:
                prev_v = values[i - 1]
                prev_x = int((i - 1) * (dot_cols - 1) / max(n - 1, 1)) if n > 1 else dot_cols // 2
                prev_y = int(min(prev_v / max_val, 1.0) * (dot_rows - 1))
                dx = abs(x - prev_x)
                dy = abs(y - prev_y)
                sx = 1 if x > prev_x else -1
                sy = 1 if y > prev_y else -1
                cx, cy = prev_x, prev_y
                if dx > dy:
                    err = dx // 2
                    while cx != x:
                        if 0 <= cx < dot_cols and 0 <= cy < dot_rows:
                            grid[cy][cx] = True
                        err -= dy
                        if err < 0:
                            cy += sy
                            err += dx
                        cx += sx
                else:
                    err = dy // 2
                    while cy != y:
                        if 0 <= cx < dot_cols and 0 <= cy < dot_rows:
                            grid[cy][cx] = True
                        err -= dx
                        if err < 0:
                            cx += sx
                            err += dy
                        cy += sy

    rows_out = []
    for row_idx in range(height_chars):
        chars = []
        for col_idx in range(width):
            code = BRAILLE_BASE
            for dc in range(2):
                for dr in range(4):
                    gy = (dot_rows - 1) - (row_idx * 4 + dr)
                    gx = col_idx * 2 + dc
                    if 0 <= gy < dot_rows and 0 <= gx < dot_cols and grid[gy][gx]:
                        code |= BRAILLE_DOTS[(dc, dr)]
            chars.append(chr(code))
        rows_out.append("".join(chars))
    return rows_out


def fmt_val(v, unit):
    if isinstance(v, float):
        if v >= 1000:
            return f"{v/1000:.1f}k{unit}"
        return f"{v:.1f}{unit}"
    if v >= 100000:
        return f"{v//1000}k{unit}"
    return f"{v}{unit}"


def render_hotkeys():
    lines = []
    lines.append("")
    lines.append(f"  {C_B}OpenTTD Wallpaper Remote Control{C_R}")
    lines.append("")
    lines.append(f"  {C_L}Map navigation{C_R}")
    lines.append(f"    {C_V}[{C_R}  prev map          {C_D}adb broadcast PREV_MAP{C_R}")
    lines.append(f"    {C_V}]{C_R}  next map          {C_D}adb broadcast NEXT_MAP{C_R}")
    lines.append("")
    lines.append(f"  {C_L}POI navigation (within current map){C_R}")
    lines.append(f"    {C_V};{C_R}  prev POI          {C_D}adb broadcast PREV_POI{C_R}")
    lines.append(f"    {C_V}'{C_R}  next POI          {C_D}adb broadcast NEXT_POI{C_R}")
    lines.append("")
    lines.append(f"  {C_L}Camera scroll ({SCROLL_PX}px per press){C_R}")
    lines.append(f"    {C_V}\u2190{C_R}  scroll left       {C_D}adb broadcast SCROLL_CAMERA dx=-{SCROLL_PX}{C_R}")
    lines.append(f"    {C_V}\u2192{C_R}  scroll right      {C_D}adb broadcast SCROLL_CAMERA dx=+{SCROLL_PX}{C_R}")
    lines.append(f"    {C_V}\u2191{C_R}  scroll up         {C_D}adb broadcast SCROLL_CAMERA dy=-{SCROLL_PX}{C_R}")
    lines.append(f"    {C_V}\u2193{C_R}  scroll down       {C_D}adb broadcast SCROLL_CAMERA dy=+{SCROLL_PX}{C_R}")
    lines.append("")
    lines.append(f"  {C_L}Screens{C_R}")
    lines.append(f"    {C_V}0-8{C_R}  switch screen     {C_V}q{C_R}  quit")
    lines.append("")
    return lines


def render(histories, screen_key):
    term_w = shutil.get_terminal_size((120, 40)).columns
    screen = SCREENS[screen_key]
    label_w = 18
    val_w = 12
    graph_w = max(20, term_w - label_w - val_w - 4)

    lines = []

    # Tab bar
    tabs = []
    for k in sorted(SCREENS.keys()):
        s = SCREENS[k]
        if k == screen_key:
            tabs.append(f"{C_H} {k}:{s['title']} {C_R}")
        else:
            tabs.append(f"{C_T} {k}:{s['title']} {C_R}")
    lines.append(" ".join(tabs))
    lines.append("")

    if screen_key == "0":
        lines.extend(render_hotkeys())
        return lines

    for key, name, unit, max_hint, is_child in screen["metrics"]:
        hist = histories.get(key, [])
        if not hist:
            continue

        cur = hist[-1]
        peak = max(max(hist), max_hint)
        mn = min(hist)

        display = hist[-graph_w * 2:]
        graph_lines = braille_graph(display, peak, graph_w, GRAPH_ROWS)

        val_str = fmt_val(cur, unit)
        peak_str = fmt_val(peak, unit)
        min_str = fmt_val(mn, unit)

        lbl_color = C_SUB if is_child else C_L

        lines.append(
            f"{lbl_color}{name:<{label_w}}{C_R}"
            f"{C_V}{val_str:>{val_w}}{C_R} "
            f"{C_G}{graph_lines[0]}{C_R}"
            f" {C_D}{peak_str}{C_R}"
        )
        for r in range(1, GRAPH_ROWS - 1):
            lines.append(f"{'':<{label_w}}{'':{val_w}} {C_G}{graph_lines[r]}{C_R}")
        if GRAPH_ROWS > 1:
            lines.append(
                f"{'':<{label_w}}{'':{val_w}} "
                f"{C_G}{graph_lines[-1]}{C_R}"
                f" {C_D}{min_str}{C_R}"
            )
        lines.append(f"{C_D}{'·' * min(term_w, label_w + val_w + 1 + graph_w + 10)}{C_R}")

    return lines


def clear_and_draw(histories, screen_key):
    lines = render(histories, screen_key)
    sys.stdout.write("\033[H\033[J")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def process_line(line, histories):
    if "PERF fps=" not in line and "VP landscape=" not in line and \
       "TICK total=" not in line and "CPU gameloop=" not in line and \
       "EXTRA gpu_actual=" not in line and "SPRITE cache_hits=" not in line and \
       "PBO rounds=" not in line and "PBO_DETAIL candidates=" not in line and \
       "GPU_SNAP clear=" not in line:
        return False

    kv = parse_kv(line)
    if not kv:
        return False

    # Map GPU_SNAP keys → screen 8 metric keys
    if "GPU_SNAP clear=" in line:
        snap_map = {
            "clear": "snap_clear", "pbo": "snap_pbo", "replay": "snap_replay",
            "cmds": "snap_cmds", "null": "snap_null", "palette": "snap_palette",
            "paint": "snap_paint", "blit": "snap_blit", "swap": "snap_swap",
        }
        mapped = {}
        for src, dst in snap_map.items():
            if src in kv:
                v = kv[src]
                if dst in _US_KEYS:
                    v = v / 1000.0
                mapped[dst] = v
        kv = mapped

    # Collect all known keys from all screens
    all_keys = set()
    for s in SCREENS.values():
        for key, _, _, _, _ in s["metrics"]:
            all_keys.add(key)

    for key in all_keys:
        if key in kv:
            if key not in histories:
                histories[key] = []
            histories[key].append(kv[key])
            if len(histories[key]) > HISTORY:
                histories[key] = histories[key][-HISTORY:]
    return True


def read_from_adb():
    proc = subprocess.Popen(
        ["adb", "logcat", "-v", "brief", "OpenTTD:D", "*:S"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, bufsize=1,
    )
    try:
        for line in proc.stdout:
            yield line.rstrip()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()


def read_from_file(path):
    with open(path, "r") as f:
        for line in f:
            yield line.rstrip()


def read_from_stdin():
    try:
        for line in sys.stdin:
            yield line.rstrip()
    except KeyboardInterrupt:
        pass


def check_keypress():
    """Non-blocking check for keypress. Returns key string or None.
    Reads all available bytes at once to avoid splitting escape sequences."""
    if not select.select([sys.stdin], [], [], 0)[0]:
        return None
    buf = os.read(sys.stdin.fileno(), 32)
    if not buf:
        return None
    if buf == b'\x1b[A': return 'UP'
    if buf == b'\x1b[B': return 'DOWN'
    if buf == b'\x1b[C': return 'RIGHT'
    if buf == b'\x1b[D': return 'LEFT'
    if buf[0:1] == b'\x1b': return 'ESC'
    return buf[0:1].decode('utf-8', errors='ignore')


def handle_hotkey(key, histories, screen_key):
    """Handle control hotkeys. Returns (new_screen_key, should_quit, redraw)."""
    if key in SCREENS:
        return key, False, True
    if key == 'q':
        return screen_key, True, False
    if key == '[':
        _adb_broadcast("PREV_MAP")
        return screen_key, False, False
    if key == ']':
        _adb_broadcast("NEXT_MAP")
        return screen_key, False, False
    if key == ';':
        _adb_broadcast("PREV_POI")
        return screen_key, False, False
    if key == "'":
        _adb_broadcast("NEXT_POI")
        return screen_key, False, False
    if key == 'LEFT':
        _adb_broadcast("SCROLL_CAMERA", f"--ei dx -{SCROLL_PX} --ei dy 0")
        return screen_key, False, False
    if key == 'RIGHT':
        _adb_broadcast("SCROLL_CAMERA", f"--ei dx {SCROLL_PX} --ei dy 0")
        return screen_key, False, False
    if key == 'UP':
        _adb_broadcast("SCROLL_CAMERA", f"--ei dx 0 --ei dy -{SCROLL_PX}")
        return screen_key, False, False
    if key == 'DOWN':
        _adb_broadcast("SCROLL_CAMERA", f"--ei dx 0 --ei dy {SCROLL_PX}")
        return screen_key, False, False
    return screen_key, False, False


def main():
    histories = {}
    screen_key = "0"

    if len(sys.argv) > 1:
        source = read_from_file(sys.argv[1])
    elif not os.isatty(0):
        source = read_from_stdin()
    else:
        source = read_from_adb()

    # Alt screen, hide cursor, raw mode for key detection
    sys.stdout.write("\033[?1049h\033[?25l")
    sys.stdout.flush()

    old_settings = None
    stdin_is_tty = os.isatty(0)
    if stdin_is_tty:
        old_settings = termios.tcgetattr(sys.stdin)
        tty.setcbreak(sys.stdin.fileno())

    # Show hotkeys screen immediately
    clear_and_draw(histories, screen_key)

    try:
        for line in source:
            # Check for key press
            if stdin_is_tty:
                key = check_keypress()
                if key is not None:
                    screen_key, quit_flag, redraw = handle_hotkey(key, histories, screen_key)
                    if quit_flag:
                        break
                    if redraw:
                        clear_and_draw(histories, screen_key)

            if process_line(line, histories):
                if "SPRITE cache_hits=" in line or "PBO_DETAIL candidates=" in line \
                   or "GPU_SNAP clear=" in line:
                    clear_and_draw(histories, screen_key)
    except KeyboardInterrupt:
        pass
    finally:
        if old_settings is not None:
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
        sys.stdout.write("\033[?25h\033[?1049l")
        sys.stdout.flush()
        print("Done.")


if __name__ == "__main__":
    main()
