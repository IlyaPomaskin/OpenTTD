#!/bin/bash
# Collect and display OpenTTD GLES performance statistics from Android device.
# Usage: ./tools/perf_collect.sh [SECONDS]  (default: 20)

set -euo pipefail

DUR=${1:-20}
TMP="/tmp/openttd/perf_collect_$$.txt"
mkdir -p /tmp/openttd

echo "==> Clearing logcat and collecting ${DUR}s of data..."
adb logcat -c
sleep "$DUR"
adb logcat -d 2>&1 | grep -i openttd | grep "TICK \|EXTRA \|VP \|PERF " > "$TMP"

TICK_N=$(grep -c "TICK " "$TMP" || true)
EXTRA_N=$(grep -c "EXTRA " "$TMP" || true)
VP_N=$(grep -c "VP " "$TMP" || true)

if [ "$TICK_N" -eq 0 ]; then
    echo "No TICK data found. Is the wallpaper running?"
    exit 1
fi

echo ""
echo "Collected: ${TICK_N} TICK, ${VP_N} VP, ${EXTRA_N} EXTRA samples in ${DUR}s"
echo ""

# === TICK breakdown ===
echo "============================================"
echo "  DRAW THREAD — TICK breakdown (avg)"
echo "============================================"
grep "TICK " "$TMP" | \
  sed 's/.*total=\([0-9]*\)us.*lock_video=\([0-9]*\)us.*game_mutex=\([0-9]*\)us.*input_poll=\([0-9]*\)us.*update_windows=\([0-9]*\)us.*check_palette=\([0-9]*\)us.*paint=\([0-9]*\)us.*gpu_render=\([0-9]*\)us.*egl_swap=\([0-9]*\)us.*/\1 \2 \3 \4 \5 \6 \7 \8 \9/' | \
  awk '{t+=$1; lock+=$2; mutex+=$3; input+=$4; uw+=$5; pal+=$6; p+=$7; gpu+=$8; swap+=$9; n++} END {
    printf "%-22s %7d us  (100%%)\n", "TICK total", t/n
    printf "%-22s %7d us  (%d%%)\n", "  update_windows", uw/n, uw*100/t
    printf "%-22s %7d us  (%d%%)\n", "  paint", p/n, p*100/t
    printf "%-22s %7d us  (%d%%)\n", "    gpu_render", gpu/n, gpu*100/t
    printf "%-22s %7d us  (%d%%)\n", "    egl_swap", swap/n, swap*100/t
    printf "%-22s %7d us  (%d%%)\n", "  game_mutex", mutex/n, mutex*100/t
    printf "%-22s %7d us  (%d%%)\n", "  input_poll", input/n, input*100/t
    printf "%-22s %7d us  (%d%%)\n", "  lock_video", lock/n, lock*100/t
    printf "%-22s %7d us  (%d%%)\n", "  check_palette", pal/n, pal*100/t
  }'

# === VP / update_windows breakdown ===
if [ "$VP_N" -gt 0 ]; then
    echo ""
    echo "============================================"
    echo "  DRAW THREAD — update_windows breakdown"
    echo "============================================"
    grep "VP " "$TMP" | \
      sed 's/.*landscape=\([0-9]*\)us.*vehicles=\([0-9]*\)us.*ground_sprites=\([0-9]*\)us.*sprite_sort=\([0-9]*\)us.*sprite_draw=\([0-9]*\)us.*update_windows=\([0-9]*\)us.*/\1 \2 \3 \4 \5 \6/' | \
      awk '{land+=$1; veh+=$2; ground+=$3; sort+=$4; draw+=$5; updwin+=$6; n++} END {
        if (n==0) { print "(no VP data parsed)"; exit }
        printf "%-22s %7d us  (100%%)\n", "update_windows", updwin/n
        printf "%-22s %7d us  (%d%%)\n", "  landscape", land/n, land*100/updwin
        printf "%-22s %7d us  (%d%%)\n", "  vehicles", veh/n, veh*100/updwin
        printf "%-22s %7d us  (%d%%)\n", "  ground_sprites", ground/n, ground*100/updwin
        printf "%-22s %7d us  (%d%%)\n", "  sprite_sort", sort/n, sort*100/updwin
        printf "%-22s %7d us  (%d%%)\n", "  sprite_draw", draw/n, draw*100/updwin
        printf "%-22s %7d us  (%d%%)\n", "  unaccounted", (updwin-land-veh-ground-sort-draw)/n, (updwin-land-veh-ground-sort-draw)*100/updwin
      }'

    # viewport size and MRT stats
    grep "VP " "$TMP" | \
      sed 's/.*viewport=\([0-9]*\)x\([0-9]*\).*full_renders=\([0-9]*\).*resolve_only=\([0-9]*\).*idle_blit=\([0-9]*\).*/\1 \2 \3 \4 \5/' | \
      awk '{vw+=$1; vh+=$2; fr+=$3; ro+=$4; ib+=$5; n++} END {
        if (n==0) exit
        printf "\n  viewport avg: %dx%d\n", vw/n, vh/n
        printf "  MRT: full_renders=%d resolve=%d idle=%d (per 500ms)\n", fr/n, ro/n, ib/n
      }'
fi

# === EXTRA / game thread breakdown ===
if [ "$EXTRA_N" -gt 0 ]; then
    echo ""
    echo "============================================"
    echo "  GAME THREAD breakdown (per 500ms window)"
    echo "============================================"
    grep "EXTRA " "$TMP" | \
      sed 's/.*gameloop=\([0-9]*\)us.*tileloop=\([0-9]*\)us.*vehtick=\([0-9]*\)us.*ticks=\([0-9]*\).*/\1 \2 \3 \4/' | \
      awk '{gl+=$1; tl+=$2; vt+=$3; tk+=$4; n++} END {
        if (n==0) { print "(no EXTRA data parsed)"; exit }
        other=(gl-vt-tl)
        printf "%-22s %7d us  (100%%)\n", "gameloop total", gl/n
        printf "%-22s %7d us  (%d%%)\n", "  vehtick", vt/n, vt*100/gl
        printf "%-22s %7d us  (%d%%)\n", "  tileloop", tl/n, tl*100/gl
        printf "%-22s %7d us  (%d%%)\n", "  other", other/n, other*100/gl
        printf "\n  Per tick: gameloop=%dus vehtick=%dus tileloop=%dus\n", gl/tk, vt/tk, tl/tk
      }'

    # vehicle counts
    grep "EXTRA " "$TMP" | tail -1 | \
      sed 's/.*vehicles: //' | \
      awk '{printf "  Vehicles: %s\n", $0}'
fi

# === PERF / FPS summary ===
echo ""
echo "============================================"
echo "  FPS & GPU summary"
echo "============================================"
grep "PERF " "$TMP" | \
  sed 's/.*fps=\([0-9]*\).*frames=\([0-9]*\).*/\1 \2/' | \
  awk '{
    fps+=$1; frames+=$2; n++
    if ($1<min || min==0) min=$1
    if ($1>max) max=$1
  } END {
    if (n==0) { print "(no PERF data)"; exit }
    printf "  fps: avg=%d min=%d max=%d\n", fps/n, min, max
    printf "  frames per 500ms window: %d\n", frames/n
  }'

echo ""
echo "Raw data saved to: $TMP"
