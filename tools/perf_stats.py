#!/usr/bin/env python3
"""Parse GLES PERF/VP/TICK logs and show statistics.

Usage:
    perf_stats.py [seconds]       # collect live from adb for N seconds (default 10)
    perf_stats.py -               # read from stdin
    perf_stats.py file.log        # read from file

Examples:
    python3 tools/perf_stats.py 20
    adb logcat -d -s OpenTTD | python3 tools/perf_stats.py -
"""

import os
import re
import sys
import tempfile
from collections import defaultdict


def parse_line(line):
    """Extract key=value pairs from a log line. Returns (section, dict)."""
    if "PERF fps=" in line:
        section = "PERF"
        # Extract after "PERF "
        m = line.find("PERF fps=")
        if m < 0:
            return None, {}
        text = line[m + 5:]
    elif "VP landscape=" in line:
        section = "VP"
        m = line.find("VP landscape=")
        if m < 0:
            return None, {}
        text = line[m + 3:]
    elif "TICK total=" in line:
        section = "TICK"
        m = line.find("TICK total=")
        if m < 0:
            return None, {}
        text = line[m + 5:]
    else:
        return None, {}

    data = {}

    # Match key=NUMBERus patterns (microseconds)
    for m in re.finditer(r'(\w+)=(-?\d+)us', text):
        data[m.group(1)] = int(m.group(2))

    # Match key=NUMBER patterns (plain integers, not followed by 'us')
    for m in re.finditer(r'(\w+)=(-?\d+)(?!us)(?=[\s|,)\]]|$)', text):
        key = m.group(1)
        if key not in data:  # don't overwrite us-suffixed values
            data[key] = int(m.group(2))

    # Match (key=NUMBERus) inside parentheses like paint=X(gpu_render=Y egl_swap=Z)
    for m in re.finditer(r'\(([^)]+)\)', text):
        inner = m.group(1)
        for im in re.finditer(r'(\w+)=(-?\d+)us', inner):
            data[im.group(1)] = int(im.group(2))
        for im in re.finditer(r'(\w+)=(-?\d+)(?!us)(?=[\s|,)]|$)', inner):
            key = im.group(1)
            if key not in data:
                data[key] = int(im.group(2))

    # Match zoom=[a/b/c/d/e/f]
    zm = re.search(r'zoom=\[([^\]]+)\]', text)
    if zm:
        parts = zm.group(1).split("/")
        for i, v in enumerate(parts):
            data[f"zoom_{i}"] = int(v)

    # Match viewport=WxH
    vp = re.search(r'viewport=(\d+)x(\d+)', text)
    if vp:
        data["viewport_w"] = int(vp.group(1))
        data["viewport_h"] = int(vp.group(2))

    return section, data


def stats(values):
    """Compute min, avg, max, p50, p95 for a list of values."""
    if not values:
        return {"min": 0, "avg": 0, "max": 0, "p50": 0, "p95": 0}
    s = sorted(values)
    n = len(s)
    return {
        "min": s[0],
        "avg": sum(s) / n,
        "max": s[-1],
        "p50": s[n // 2],
        "p95": s[min(int(n * 0.95), n - 1)],
    }


def collect_from_adb(seconds):
    """Collect logs from adb for N seconds."""
    os.system("adb logcat -c")
    print(f"Collecting for {seconds}s...", flush=True)

    import time
    time.sleep(seconds)

    with tempfile.NamedTemporaryFile(mode='r', suffix='.log', delete=False) as f:
        tmp = f.name
    os.system(f"adb logcat -d -s OpenTTD > {tmp} 2>/dev/null")
    with open(tmp) as f:
        lines = f.readlines()
    os.unlink(tmp)
    return lines


def collect_from_stdin():
    return sys.stdin.readlines()


def collect_from_file(path):
    with open(path) as f:
        return f.readlines()


def print_section(name, samples, keys):
    """Print stats table for a section."""
    if not samples:
        return

    print(f"\n{'=' * 70}")
    print(f"  {name}  ({len(samples)} samples)")
    print(f"{'=' * 70}")
    print(f"  {'metric':<22s} {'min':>8s} {'avg':>8s} {'p50':>8s} {'p95':>8s} {'max':>8s}")
    print(f"  {'-' * 22} {'-' * 8} {'-' * 8} {'-' * 8} {'-' * 8} {'-' * 8}")

    for key in keys:
        values = [s[key] for s in samples if key in s]
        if not values:
            continue
        st = stats(values)
        # Format: use 'us' suffix for time metrics
        is_time = key.endswith("_us") or key in (
            "total", "lock_video", "game_mutex", "input_poll", "update_windows",
            "populate_sprites", "check_palette", "paint", "gpu_render", "egl_swap",
            "unlock_video", "unaccounted", "gpu_paint",
            "landscape", "vehicles", "signs_tiles", "sprite_sort", "sprite_draw",
            "resolve",
        )
        unit = "us" if is_time else ""
        fmt = lambda v: f"{int(v)}{unit}"
        print(f"  {key:<22s} {fmt(st['min']):>8s} {fmt(st['avg']):>8s} {fmt(st['p50']):>8s} {fmt(st['p95']):>8s} {fmt(st['max']):>8s}")


def main():
    args = sys.argv[1:]

    if not args or (len(args) == 1 and args[0].isdigit()):
        seconds = int(args[0]) if args else 10
        lines = collect_from_adb(seconds)
    elif args[0] == "-":
        lines = collect_from_stdin()
    else:
        lines = collect_from_file(args[0])

    # Parse all lines
    sections = defaultdict(list)
    for line in lines:
        section, data = parse_line(line)
        if section and data:
            sections[section].append(data)

    if not sections:
        print("No PERF/VP/TICK lines found.")
        return

    # Define key order for each section
    perf_keys = [
        "fps", "frames",
        "blit_calls", "gpu_cmds", "atlas_miss", "offscreen_skip",
        "gl_batches", "reuploaded", "dim_mismatch",
        "scaled_hits", "scaled_fallback",
        "gpu_paint", "egl_swap",
        "total", "uploaded", "all_transparent",
        "color_pages", "remap_pages", "gpu_entries", "registered",
        "new_sprites", "repacked",
    ]

    vp_keys = [
        "landscape", "vehicles", "signs_tiles", "sprite_sort", "sprite_draw",
        "update_windows",
        "tiles_iterated", "parent_sprites", "child_sprites",
        "vp_draw_calls", "viewport_w", "viewport_h",
        "full_renders", "resolve_only", "idle_blit", "resolve",
    ]

    tick_keys = [
        "total", "lock_video", "game_mutex", "skipped",
        "input_poll", "update_windows", "populate_sprites",
        "check_palette", "paint", "gpu_render", "egl_swap",
        "unlock_video", "unaccounted",
    ]

    print_section("PERF", sections.get("PERF", []), perf_keys)
    print_section("VP", sections.get("VP", []), vp_keys)
    print_section("TICK", sections.get("TICK", []), tick_keys)

    # Summary
    perf = sections.get("PERF", [])
    tick = sections.get("TICK", [])
    if perf and tick:
        fps_vals = [s["fps"] for s in perf if "fps" in s]
        total_vals = [s["total"] for s in tick if "total" in s]
        paint_vals = [s["paint"] for s in tick if "paint" in s]
        updwin_vals = [s["update_windows"] for s in tick if "update_windows" in s]

        fps_st = stats(fps_vals)
        total_st = stats(total_vals)

        print(f"\n{'=' * 70}")
        print(f"  SUMMARY")
        print(f"{'=' * 70}")
        print(f"  FPS: {fps_st['min']}-{fps_st['max']} (avg {fps_st['avg']:.0f}, p50 {fps_st['p50']})")
        print(f"  Frame budget: {total_st['avg']/1000:.1f}ms avg, {total_st['p95']/1000:.1f}ms p95")

        if paint_vals and updwin_vals:
            paint_st = stats(paint_vals)
            updwin_st = stats(updwin_vals)
            total_avg = total_st["avg"]
            if total_avg > 0:
                print(f"  Breakdown: paint {paint_st['avg']/total_avg*100:.0f}% + update_windows {updwin_st['avg']/total_avg*100:.0f}% + other {(1 - (paint_st['avg'] + updwin_st['avg'])/total_avg)*100:.0f}%")

        # Show frames where fps < 50
        slow = [s for s in perf if s.get("fps", 100) < 50]
        if slow:
            print(f"  Slow periods (fps<50): {len(slow)}/{len(perf)} ({len(slow)*100//len(perf)}%)")


if __name__ == "__main__":
    main()
