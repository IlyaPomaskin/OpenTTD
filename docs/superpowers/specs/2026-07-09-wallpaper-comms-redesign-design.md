# Wallpaper Settings + Native/Android Comms Redesign

Date: 2026-07-09
Status: Ready for implementation plan
**Plan-confidence status:** Ready for implementation plan (iteration 2, confidence 90%)

## Goal

Restructure the Android live-wallpaper boundary toward the h2lwp shape:

1. Wallpaper settings live in `openttd.cfg` (native reads them from there).
2. The Java↔native command surface shrinks to **one JNI symbol**.
3. Map-rotation **cadence is owned by native**, not an Android timer.
4. **Android is only a view**: it writes settings to the cfg and forwards commands; it holds no wallpaper state or logic.

## Constraints discovered

- The settings UI and the running wallpaper are in **different processes**:
  `WallpaperSettingsActivity` runs in the main process, `OpenTTDWallpaperService`
  in `:wallpaper`. A JNI call only connects the service to *its* engine — the
  settings view cannot call the running engine. The cfg file is therefore the
  cross-process settings channel.
- On Android `openttd.cfg` lives in `getFilesDir()`, which is shared across the
  app's processes — the settings Activity writes the same file the wallpaper
  process reads.
- OpenTTD's `IniFile` round-trips groups it does not recognize; only explicitly
  listed obsolete groups are `RemoveGroup`'d. An ad-hoc `[wallpaper]` section
  survives a native `SaveToConfig()`. To avoid a cross-process write race, native
  is **read-only** on the cfg in wallpaper mode; the Android view is the sole
  writer of `[wallpaper]`.

## Architecture

```
┌─ Android (VIEW only) ──────────────┐        ┌─ Native (OWNS state) ─────────┐
│ WallpaperSettingsActivity          │        │ wallpaper.cpp / sdl2_gles_v   │
│   └ writes [wallpaper] in cfg  ────┼──file──▶│   reads [wallpaper] on start  │
│ Broadcast receivers (adb + UI)     │        │   + on RELOAD_SETTINGS        │
│   └ WallpaperNative.command(...) ──┼──JNI───▶│ single dispatch → atomics     │
│ Engine lifecycle (surface/vis)     │  (1 fn) │   drained in Tick()/GL thread │
└────────────────────────────────────┘        │ owns cadence timer            │
                                               └───────────────────────────────┘
```

Three roles: Android = view (writes cfg, forwards commands, relays lifecycle).
Native = owner (settings state, cadence, all logic). Two channels: the cfg file
(settings, Android→native) and one JNI function (commands, Android→native).

## Component 1 — the single JNI symbol

New tiny Java class `org.openttd.android.WallpaperNative` holding exactly one
native method, so there is literally one C++ symbol, called by both the wallpaper
service and `GameActivity`:

```java
public final class WallpaperNative {
    public static native void nativeWallpaperCommand(String command, int arg1, int arg2);
}
```

C++ entry (in `sdl2_gles_v.cpp`):

```
Java_org_openttd_android_WallpaperNative_nativeWallpaperCommand(env, cls, command, a1, a2)
    dispatch on the command NAME (string) → writes the SAME atomics already drained in Tick()
```

Command names are passed as **strings** — self-describing, so there is no numeric
enum to keep in sync between Java and C++. The set of command names is the shared
contract; native ignores an unknown name (forward-compatible).

### Command set

| command | arg1 / arg2 | maps to (existing mechanism) |
|---|---|---|
| `"PREPARE_BG"` | — | hide hook: rotate-map-if-elapsed **else** jump POI |
| `"ROTATE_MAP"` | delta | `_gles_rotate_map` (SWITCH_MAP = +1) |
| `"NAVIGATE_POI"` | delta | `_gles_navigate_poi` |
| `"SCROLL_CAMERA"` | dx, dy | `_gles_scroll_dx` / `_gles_scroll_dy` |
| `"SET_PAUSED"` | 0/1 | `SetGameThreadPaused` |
| `"SURFACE_CHANGED"` | — | `_gles_surface_changed` |
| `"RELOAD_SETTINGS"` | — | `_gles_reload_settings` (new) → re-read cfg |
| `"REFRESH_TITLE_MAPS"` | — | `_gles_refresh_title_maps` |

**Removed from JNI:** `nativeSetBrightness` (→ cfg), `nativeSetIntervalActive`
(→ native cadence), `nativeDumpAtlas` (feature dropped — the atlas bug it debugged
is fixed). `nativeSwitchMap` folds into `ROTATE_MAP(+1)`. Net: ~15 JNI functions
(11 service + 5 `GameActivity` duplicates, minus overlap) collapse to **1**.

## Component 2 — settings in `openttd.cfg`

```ini
[wallpaper]
brightness = 100          ; 0..100 percent
map_update_interval = 2   ; 0=every POI-wrap, 1=10min, 2=30min, 3=2h, 4=24h
```

**Native read** — `WallpaperReadConfig()`, called from `LoadWallpaperGame()` for
the initial read (wallpaper-mode entry, after config is already loaded, on the game
thread) and again whenever `RELOAD_SETTINGS` is drained on the game thread:

- open `IniFile(_config_file)` → `GetGroup("wallpaper")` → `GetItem("brightness")`,
  `GetItem("map_update_interval")`; parse ints, defaults 100 / 2 on missing.
- apply brightness: `GLESBackend::SetBrightness(brightness / 100.0f)` called
  directly — a single benign float store read by GL draw; safe from the game thread.
- store `map_update_interval` into native `_wp_interval_index`.

**Android write** — a new `WallpaperConfig` class (Kotlin/Java) that rewrites only
the two `[wallpaper]` keys in `getFilesDir()/openttd.cfg`, leaving every other line
untouched (mirrors h2lwp `WallpaperConfigRepository`). After writing, broadcast
`SETTINGS_CHANGED`; the service receiver calls
`nativeWallpaperCommand("RELOAD_SETTINGS", 0, 0)`.

### Settings-change flow

```
SeekBar / dialog → WallpaperConfig.setBrightness(70)   (edits [wallpaper] in cfg)
  → sendBroadcast(SETTINGS_CHANGED)
  → service receiver → nativeWallpaperCommand("RELOAD_SETTINGS", 0, 0)
  → _gles_reload_settings = true → Tick() drains → WallpaperReadConfig() → apply
```

Brightness units: cfg stores int percent (0..100); native converts `/100.0f` for
`GLESBackend::SetBrightness` (which clamps 0..1).

## Component 3 — native-owned cadence (rotate-on-hide)

Native state: `_wp_interval_index` (from cfg) and `_wp_last_rotation` (wall clock).
`INTERVAL_MS` mapping moves from Java into native: `{0, 10min, 30min, 2h, 24h}`.

`CMD_PREPARE_BG` handler is the hide hook. The service sends it in
`onVisibilityChanged(false)` before its existing delayed pause. Drained on the
game thread:

```
elapsed = now - _wp_last_rotation
if (_wp_interval_index >= 1 && elapsed >= INTERVAL_MS[_wp_interval_index]):
      RotateTitleMap(+1); _wp_last_rotation = now   // loads next map; warm-up renders it
else: jump POI (existing _gles_jump_waypoint path)
```

This unifies "jump POI on hide" and "rotate map on interval" behind one hook, so a
reveal after the interval elapsed shows a fresh, already-loaded map (seamless).

Index 0 = rotate on POI-wrap: `RequestNextTitleMap`'s guard becomes
`if (_wp_interval_index != 0) return;` — reading native state instead of the
`_gles_interval_active` Java flag.

**Removed:** Android `INTERVAL_MS` + `Handler` / `armIntervalTimer` /
`cancelIntervalTimer`, `nativeSetIntervalActive`, and the `_gles_interval_active`
atomic.

**Threading:** the reload read and the cadence check both run on the game thread in
the existing `Tick()` atomic-drain — consistent with the in-flight lwp2
single-producer model (`gameloop_ticks`, game-thread sprite-dimension reads). No new
thread or queue is introduced; the string command only sets the same atomics.

## Removals summary

| Layer | Removed | Replaced by |
|---|---|---|
| Java | `SettingsHelper` brightness/interval prefs | cfg `[wallpaper]` editor |
| Java | interval `Handler` / timer methods | native cadence |
| Java | 11 service natives + 5 `GameActivity` dups | `WallpaperNative.nativeWallpaperCommand` |
| Java | `SETTINGS_CHANGED` value extras | bare reload trigger |
| Java | `DUMP_ATLAS` broadcast + receiver | — (dropped) |
| C++ | ~15 JNI fns, `nativeSetBrightness`, `nativeSetIntervalActive`, `_gles_interval_active` | 1 JNI fn + `WallpaperReadConfig()` + native interval state |
| C++ | `nativeDumpAtlas`, `_gles_dump_atlas`, `_gles_dump_atlas_dir` | — (dropped) |

**Kept:** broadcast receivers (external `adb` control API) — each now just calls
`nativeWallpaperCommand("NAME", …)`; the `title_assets_provisioned` SharedPreference
(app state, not a wallpaper setting); all SDL surface/lifecycle natives.

## Error handling / edge cases

- Missing/empty `[wallpaper]` section or unparseable values → in-memory defaults
  (100 / 2). Native never writes the section; the Android view creates it on the
  first settings change (first run before any write → defaults apply).
- `RELOAD_SETTINGS` arriving before the engine is initialized → the atomic is
  simply drained once the game loop starts; harmless.
- Native stays read-only on the cfg in wallpaper mode to avoid a cross-process
  write race with the Android view.
- Concurrent read-during-write (Android mid-write while native reads): accepted as
  negligible — writes are tiny and rare (a user tapping a setting), and a rare
  malformed parse falls back to defaults. No atomic-write / locking added.

## Testing

- **Native cfg read**: write a temp cfg with a `[wallpaper]` section, call
  `WallpaperReadConfig()`, assert brightness/interval parsed and defaults applied
  on missing keys.
- **Round-trip preserve**: load a cfg containing `[wallpaper]` plus other groups,
  `SaveToConfig()`, assert `[wallpaper]` still present.
- **Command dispatch**: invoke `nativeWallpaperCommand` for each command name,
  assert the corresponding atomic / native state changes; an unknown name is a no-op.
- **On-device** (`tools/run_android.py all`): brightness slider changes live;
  interval change causes a map swap on lock/unlock after the interval elapses;
  `adb` broadcasts still drive POI / map / scroll.

## Out of scope

- Migrating existing users' SharedPreferences values into the cfg (fresh defaults
  are acceptable; no migration shim).
- Any change to the typed OpenTTD settings-table framework — `[wallpaper]` is a
  deliberately ad-hoc section read directly via `IniFile`.
- Desktop (non-`WALLPAPER_BUILD`) behavior.

## Confidence Survey

No open questions. All Iteration 1 questions were answered interactively and folded
into the plan body (see Reconciliation Log).

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-09
- **Confidence:** 80% (cap from Risk: cross-boundary cfg read/write failure mode not analyzed; and Readiness: enough underspecification that two engineers would diverge)
- **Resolved:** (none — first iteration)
- **Still uncertain:** concurrent file-access safety (Q1.1), enum-sync mechanism (Q1.2), startup read wiring (Q1.3), threading fit for reload/cadence (Q1.4/Q1.5), Android writer component (Q1.6), dump-atlas dir (Q1.7), first-run defaults (Q1.8). Universal cap: spec did not mention interaction with in-flight lwp2 threading work (addressed by Q1.5).
- **New questions:** Q1.1 … Q1.8

### Iteration 2 — 2026-07-09
- **Confidence:** 90% (universal in-flight cap lifted via Q1.5; Risk and Readiness gaps closed). Answered interactively, folded into body.
- **Resolved:**
  - Q1.1 → accept partial-read race as negligible, no atomic-write/locking → Error handling.
  - Q1.2 → command names as **strings** over JNI, no enum to sync → Component 1 (signature + command table).
  - Q1.3 → `WallpaperReadConfig()` initial read in `LoadWallpaperGame()` → Component 2.
  - Q1.4 → direct `GLESBackend::SetBrightness` in reload path (benign float store) → Component 2.
  - Q1.5 → reload + cadence on game thread in existing `Tick()` drain → Component 3 (Threading note).
  - Q1.6 → new `WallpaperConfig` Android class editing only `[wallpaper]` keys → Component 2.
  - Q1.7 → **drop DUMP_ATLAS** entirely (atlas bug fixed; feature unneeded) → Component 1 + Removals.
  - Q1.8 → native in-memory defaults, never writes; Android creates section on first change → Error handling.
- **Still uncertain:** none blocking. Remaining detail (exact command-name strings, `WallpaperConfig` write method) is implementation-plan granularity.
- **New questions:** (none)
