# GPU Sprite Rendering Specification

## Executive Summary

Replace CPU-composited sprite rendering with GPU-batched textured quads for viewport sprites in the OpenTTD GLES backend. This eliminates the 69% CPU bottleneck caused by uploading the full framebuffer via `glTexImage2D` every frame.

## Problem Statement

**Current pipeline:**
1. CPU blitter draws all sprites into a video buffer (CPU memory)
2. `GLESBackend::UploadVideoBuffer()` uploads entire buffer via `glTexImage2D`
3. GPU renders a fullscreen quad with that texture

**Profiling results (Android ARM64):**
- `glTexImage2D` → GPU driver `ioctl`: 69% of CPU time
- This happens every frame regardless of what changed
- The NEON-optimized blitter (now ~0% overhead) is pointless when upload dominates

**Solution:**
- Queue draw commands during `Draw()` instead of CPU-blitting
- GPU renders sprites directly from the atlas (already uploaded during `Encode()`)
- Skip the expensive full-framebuffer upload

## Success Criteria

| Metric | Current | Target |
|--------|---------|--------|
| CPU time in Paint() | ~98% | <20% |
| glTexImage2D overhead | 69% | 0% (viewport) |
| Frame time (60fps target) | ? | <16.6ms |
| Visual output | N/A | Recognizable sprites in correct positions |

## User Personas

**Primary:** Android live wallpaper users
- Need smooth performance on mid-range ARM64 devices
- Visual quality less critical than performance

**Secondary:** Developers
- Need debug tools to compare GPU vs CPU rendering
- Need to identify unsupported sprites

## User Journey

1. User launches OpenTTD (Android wallpaper mode)
2. Game initializes GLES backend, sprites uploaded to atlas during loading
3. During gameplay, viewport sprites render via GPU-batched quads
4. UI elements continue using CPU path (stubbed for future)
5. Developer can toggle GPU/CPU mode via debug hotkey for comparison

## Functional Requirements

### Must Have (P0)

#### FR-1: GPU Draw Command Queue
- `Blitter_GLES::Draw()` queues `GLESDrawCommand` instead of CPU-blitting
- Commands include: sprite atlas key, screen position, dimensions, mode, zoom
- Queue preserves exact submission order for correct transparency

**Acceptance criteria:**
- Draw() returns immediately without touching the CPU video buffer
- Commands accumulate in `GLESBackend::draw_queue`

#### FR-2: Sprite-to-Atlas Key Mapping
- During `Encode()`, store atlas key in `sprite->data` (first 8 bytes)
- During `Draw()`, read atlas key from sprite data to find GPU texture coordinates

**Acceptance criteria:**
- `Encode()` writes: `*(uint64_t*)sprite->data = atlas_key`
- `Draw()` reads: `atlas_key = *(uint64_t*)bp->sprite`
- Existing CPU sprite data offset adjusted accordingly

#### FR-3: GPU Batch Rendering
- `GLESBackend::Paint()` renders all queued commands via batched quads
- Use existing shader programs (prog_normal, prog_remap, prog_palette, etc.)
- Standard GL alpha blending: `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`

**Acceptance criteria:**
- All queued sprites render to FBO
- Sprites appear in correct positions and sizes
- Transparent sprites blend correctly with background

#### FR-4: Skip CPU Buffer Upload
- Add flag `_gles_gpu_sprites_enabled` (default: true)
- When enabled, skip `UploadVideoBuffer()` call for viewport
- Keep upload code intact for debug mode

**Acceptance criteria:**
- `glTexImage2D` not called when GPU sprites enabled
- Can re-enable upload via debug flag for comparison

#### FR-5: Debug Toggle
- Debug hotkey (e.g., F12) toggles `_gles_gpu_sprites_enabled`
- Visual indicator when GPU mode active (optional)

**Acceptance criteria:**
- Pressing hotkey switches between GPU and CPU rendering
- State persists until next toggle

### Should Have (P1)

#### FR-6: Missing Sprite Warning
- Log warning when sprite not found in atlas (first occurrence only)
- Include sprite pointer address in log for debugging

**Acceptance criteria:**
- Warning appears in debug log: "GPU sprite not in atlas: 0x..."
- Same sprite doesn't spam repeated warnings

#### FR-7: Mode Support
- Support all BlitterModes: Normal, ColourRemap, Transparent, CrashRemap, BlackRemap
- ColourRemap: draw base sprite colors (ignore remap table for now)

**Acceptance criteria:**
- All modes produce visible output
- ColourRemap sprites visible (may have wrong company colors)

### Nice to Have (P2)

#### FR-8: UI Stub Functions
- Add `QueueUIRect()`, `QueueUIText()` stub functions
- Return false to indicate "not implemented, use CPU fallback"

**Acceptance criteria:**
- Stubs exist and compile
- UI rendering continues to work via CPU path

#### FR-9: Partial Buffer Upload (Future)
- Infrastructure for uploading only UI regions
- `UploadVideoBufferRegion(x, y, w, h)`

## Technical Architecture

### Data Flow

```
Encode Phase (loading):
  SpriteLoader → Blitter_GLES::Encode()
                    ├→ Blitter_32bppOptimized::Encode() (CPU data)
                    ├→ GLESSpriteAtlas::Upload() (GPU texture)
                    └→ Write atlas_key to sprite->data[0:8]

Draw Phase (per frame):
  ViewportDraw → GfxBlitter → Blitter_GLES::Draw()
                                 ├→ Read atlas_key from sprite
                                 ├→ Lookup GLESSpriteEntry
                                 └→ GLESBackend::QueueDraw(cmd)

Paint Phase (per frame, in GLESBackend::Paint):
  1. Clear FBO
  2. For each draw command in queue (in order):
     - Accumulate vertex (x, y, u, v, ru, rv)
     - On batch break (mode/atlas change): FlushBatch()
  3. Blit FBO to screen
  4. Clear draw queue
```

### Key Data Structures

```cpp
// Stored at sprite->data[0:8] during Encode()
struct GLESSpriteKey {
    uint32_t sprite_id;   // Unique ID for this sprite
    ZoomLevel zoom;       // Zoom level (4 bytes with padding)
};

// Draw command queued during Draw()
struct GLESDrawCommand {
    GLESSpriteKey key;
    int16_t screen_x, screen_y;
    int16_t width, height;
    int16_t skip_left, skip_top;    // Clipping offsets
    BlitterMode mode;
    uint8_t remap_idx;              // For future remap support
};

// Looked up from atlas during Paint()
struct GLESSpriteEntry {
    uint16_t atlas_page;
    uint16_t x, y, w, h;            // Position in atlas
    float u0, v0, u1, v1;           // Pre-computed UVs
    uint16_t remap_page;            // M-channel atlas page
    float ru0, rv0, ru1, rv1;       // Remap UVs
    bool has_remap;
};
```

### System Components

| Component | Responsibility |
|-----------|----------------|
| `Blitter_GLES::Encode()` | Upload to atlas, write key to sprite |
| `Blitter_GLES::Draw()` | Queue draw command (no CPU blit) |
| `GLESBackend::QueueDraw()` | Append to draw queue |
| `GLESBackend::Paint()` | Flush queue as batched GL draws |
| `GLESSpriteAtlas` | Manage atlas textures, sprite→UV lookup |

### Shader Usage

| BlitterMode | Shader | Notes |
|-------------|--------|-------|
| Normal | `prog_normal` | Simple texture sample |
| ColourRemap | `prog_normal` | Use base colors (remap later) |
| Transparent | `prog_transparent` | Special blend mode |
| CrashRemap | `prog_palette` | Palette lookup |
| BlackRemap | `prog_solid` | Solid black with alpha |

## Non-Functional Requirements

- **Performance**: Paint() under 5ms on target device
- **Memory**: No additional GPU memory (atlas already exists)
- **Compatibility**: GLES 3.0 minimum
- **Correctness**: Sprites recognizable, positions correct (not pixel-perfect)

## Out of Scope

- UI rendering (text, windows, buttons) — stays on CPU
- Pixel-perfect visual matching with CPU blitter
- Remap table support (company colors may be wrong)
- Multiple zoom levels (normal zoom only)
- Dirty-rect optimization
- Desktop OpenGL support (GLES only)

## Open Questions for Implementation

1. **Sprite key storage**: How to handle the 8-byte key without breaking CPU sprite data format? May need to add header before existing data.

2. **Vertex buffer sizing**: Current VBO is 64K vertices. Is this enough for max viewport sprites?

3. **Atlas lookup speed**: HashMap vs array lookup for sprite→entry mapping?

4. **Clipping**: Should clipping be done in vertex generation or fragment shader?

5. **Debug hotkey**: Which key? Platform-specific or SDL-level?

## Appendix: Existing Code Analysis

### Files to Modify

| File | Changes |
|------|---------|
| `src/blitter/gles.cpp` | Modify `Draw()` to queue commands |
| `src/blitter/gles.hpp` | Add sprite key structure |
| `src/video/gles_backend.cpp` | Add batch rendering in `Paint()` |
| `src/video/gles_backend.h` | Add draw queue, sprite key lookup |
| `src/video/gles_sprite.cpp` | Add sprite key storage/lookup |
| `src/video/gles_sprite.h` | Add sprite key to entry |

### Existing Infrastructure

- **Shaders**: All needed shaders exist in `src/table/gles_shader.h`
- **VBO**: Vertex buffer for batched quads exists in `GLESBackend`
- **Atlas**: `GLESSpriteAtlas` already manages texture uploads
- **Draw commands**: `GLESDrawCommand` struct already defined

### Key Functions to Modify

1. `Blitter_GLES::Encode()` — add atlas key storage
2. `Blitter_GLES::Draw()` — replace CPU blit with queue
3. `GLESBackend::Paint()` — add batch flush loop
4. `GLESSpriteAtlas::Upload()` — return/store sprite key
