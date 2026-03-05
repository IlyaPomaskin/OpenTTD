# Claude CLI — OpenTTD Project Instructions

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