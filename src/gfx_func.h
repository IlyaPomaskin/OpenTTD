/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gfx_func.h Functions related to the gfx engine. */

/**
 * @defgroup dirty Dirty
 *
 * Handles the repaint of some part of the screen.
 *
 * Some places in the code are called functions which makes something "dirty".
 * This has nothing to do with making a Tile or Window darker or less visible.
 * This term comes from memory caching and is used to define an object must
 * be repaint. If some data of an object (like a Tile, Window, Vehicle, whatever)
 * are changed which are so extensive the object must be repaint its marked
 * as "dirty". The video driver repaint this object instead of the whole screen
 * (this is btw. also possible if needed). This is used to avoid a
 * flickering of the screen by the video driver constantly repainting it.
 *
 * This whole mechanism is controlled by an rectangle defined in #_invalid_rect. This
 * rectangle defines the area on the screen which must be repaint. If a new object
 * needs to be repainted this rectangle is extended to 'catch' the object on the
 * screen. At some point (which is normally uninteresting for patch writers) this
 * rectangle is send to the video drivers method
 * VideoDriver::MakeDirty and it is truncated back to an empty rectangle. At some
 * later point (which is uninteresting, too) the video driver
 * repaints all these saved rectangle instead of the whole screen and drop the
 * rectangle information. Then a new round begins by marking objects "dirty".
 *
 * @see VideoDriver::MakeDirty
 * @see _invalid_rect
 * @see _screen
 */


#ifndef GFX_FUNC_H
#define GFX_FUNC_H

#include "gfx_type.h"
#include "strings_type.h"
#include "string_type.h"

void GameLoop();

void CreateConsole();

extern uint8_t _dirkeys;        ///< 1 = left, 2 = up, 4 = right, 8 = down
extern bool _fullscreen;
extern uint8_t _support8bpp;
extern CursorVars _cursor;
extern bool _ctrl_pressed;   ///< Is Ctrl pressed?
extern bool _shift_pressed;  ///< Is Shift pressed?
extern uint16_t _game_speed;

extern bool _left_button_down;
extern bool _left_button_clicked;
extern bool _right_button_down;
extern bool _right_button_clicked;

extern DrawPixelInfo _screen;
extern bool _screen_disable_anim;   ///< Disable palette animation (important for 32bpp-anim blitter during giant screenshot)
extern bool _gles_video_active;     ///< When true, GLES video driver is active (enables dirty block coalescing).
extern bool _gles_context_lost;     ///< Set on SDL_RENDER_DEVICE_RESET; consumed by GLES Paint() to trigger GPU rebuild.

/** Per-frame rendering performance counters. Reset after logging. */
struct GLESPerfCounters {
	/* Viewport phases (microseconds) */
	int64_t vp_land_us = 0;        ///< ViewportAddLandscape time.
	int64_t vp_vehicles_us = 0;    ///< ViewportAddVehicles time.
	int64_t vp_signs_tiles_us = 0; ///< Signs + tile sprites + text effects time.
	int64_t vp_kdtree_us = 0;      ///< ViewportAddKdtreeSigns time.
	int64_t vp_texteff_us = 0;     ///< DrawTextEffects time.
	int64_t vp_tilesprites_us = 0; ///< ViewportDrawTileSprites time.
	int vp_kdtree_found = 0;       ///< K-d tree items found (stations+towns+signs).
	int vp_strings_queued = 0;     ///< Strings queued for rendering.
	int vp_tile_sprites = 0;       ///< Tile sprites drawn.
	int64_t vp_sort_us = 0;        ///< Sprite sort time.
	int64_t vp_draw_us = 0;        ///< ViewportDrawParentSprites time (CPU compositing).
	int vp_parent_sprites = 0;     ///< Number of parent sprites drawn.
	int vp_child_sprites = 0;      ///< Number of child sprites drawn.
	int vp_tiles_iterated = 0;     ///< Tiles iterated in ViewportAddLandscape.
	int vp_area_w = 0;             ///< Viewport dirty area width.
	int vp_area_h = 0;             ///< Viewport dirty area height.
	int vp_calls = 0;              ///< Number of ViewportDoDraw calls this period.

	/* Blitter counters */
	int blit_draw_calls = 0;       ///< Total Blitter::Draw() calls.
	int64_t blit_draw_pixels = 0;  ///< Total pixels composited by blitter.
	int blit_fillrect_calls = 0;   ///< GfxFillRect calls.
	int blit_drawstring_glyphs = 0;///< DrawString glyph count.

	/* Upload / Paint */
	int64_t upload_us = 0;         ///< UploadVideoBuffer time.
	int upload_bytes = 0;          ///< Bytes uploaded.
	int upload_rows = 0;           ///< Rows uploaded (0 = skipped).
	int64_t gpu_paint_us = 0;      ///< GLESBackend::Paint time.
	int64_t swap_us = 0;           ///< SDL_GL_SwapWindow time.
	int gpu_draw_cmds = 0;         ///< GPU draw commands queued.
	int gpu_batches = 0;           ///< GPU draw batches (actual glDrawArrays calls).
	int gpu_sprites_missing = 0;   ///< Sprites not found in atlas (silently skipped).
	int gpu_sprites_new = 0;       ///< New sprites packed into atlas this period.
	int gpu_sprites_repacked = 0;  ///< Sprites evicted and repacked (dimension change).
	int gpu_sprites_reuploaded = 0;///< Sprites re-uploaded due to cache key collision.
	int gpu_dim_mismatches = 0;   ///< Sprites where computed dims != atlas entry dims.
	int gpu_skip_offscreen = 0;   ///< Draws skipped: dst outside screen buffer.
	int gpu_zoom_counts[8] = {};  ///< Draw calls per zoom level.
	int gpu_scaled_hits = 0;      ///< Draws using base-zoom atlas entry (GPU-scaled).
	int gpu_scaled_fallbacks = 0; ///< Draws where base-zoom not found, used original zoom.

	/* PBO async upload */
	int64_t pbo_batches_submitted = 0;  ///< PBO batches submitted to GPU.
	int64_t pbo_batches_completed = 0;  ///< PBO batches completed (fence signaled).
	int64_t pbo_sprites_uploaded = 0;   ///< Sprites uploaded via PBO.
	int64_t pbo_bytes_uploaded = 0;     ///< Bytes uploaded via PBO.
	int64_t pbo_fence_waits = 0;        ///< Times fence was not ready (skipped).
	int pbo_rounds = 0;                 ///< PBO fill+submit rounds per ProcessPBOUploads.
	int pbo_candidates = 0;             ///< Staged keys not yet in sprites map.
	int pbo_already_uploaded = 0;       ///< Keys skipped (already in sprites map).
	int pbo_staged_gone = 0;            ///< Keys missing from staged when accessed.
	int pbo_pbo_full = 0;               ///< Sprites skipped (PBO buffer full).
	int pbo_pack_colour_fail = 0;       ///< PackRegion failures (colour atlas).
	int pbo_pack_remap_fail = 0;        ///< PackRegion failures (remap atlas).
	int pbo_map_fail = 0;               ///< glMapBufferRange failures.
	int pbo_palette_only = 0;           ///< Sprites with palette_only flag.
	int pbo_no_remap = 0;               ///< Sprites without remap data.
	int pbo_alloc_pages = 0;            ///< AllocPage calls during PBO fill.
	int64_t pbo_fill_us = 0;            ///< PBOFillBatch time (microseconds).
	int64_t pbo_submit_us = 0;          ///< PBOSubmitBatch time (microseconds).

	/* Encode/Upload lifecycle */
	int encode_total = 0;          ///< Total Encode() calls this period.
	int encode_skipped = 0;        ///< Encode() calls skipped (no backend).
	int encode_uploaded = 0;       ///< Encode() calls that uploaded to atlas.
	int encode_all_transparent = 0;///< Sprites uploaded with all alpha=0.

	/* MRT palette resolve */
	int full_renders = 0;          ///< Frames with full sprite render (draw_queue non-empty).
	int resolve_passes = 0;        ///< Frames with palette resolve pass (palette-only update).
	int idle_blits = 0;            ///< Frames with only FBO blit (nothing changed).
	int64_t resolve_us = 0;        ///< Total time in resolve passes.

	int64_t update_windows_us = 0; ///< Total UpdateWindows time.
	int64_t lock_video_us = 0;     ///< LockVideoBuffer time (vsync wait).
	int64_t mutex_wait_us = 0;     ///< game_state_mutex acquisition time.
	int mutex_skipped = 0;         ///< Frames where mutex try_lock failed (wallpaper mode).
	int64_t input_poll_us = 0;     ///< PollEvent + InputLoop + DrainCommandQueue time.
	int64_t populate_us = 0;       ///< PopulateSystemSprites time.
	int64_t check_palette_us = 0;  ///< CheckPaletteAnim time.
	int64_t paint_full_us = 0;     ///< Full Paint() time (from Tick perspective).
	int64_t unlock_video_us = 0;   ///< UnlockVideoBuffer time.
	int64_t tick_total_us = 0;     ///< Total Tick() time (draw section only).
	int64_t last_frame_us = 0;     ///< Most recent single-frame tick time (for jank detection).

	/* GPU timer query (microseconds, read from previous frame) */
	int64_t gpu_time_us = 0;       ///< Actual GPU execution time (timer query).

	/* Jank detection */
	int jank_count = 0;            ///< Frames where frame_time > 2x rolling average.

	/* Game loop timing (game thread, microseconds) */
	int64_t gameloop_us = 0;       ///< Total StateGameLoop time.
	int64_t tileloop_us = 0;       ///< RunTileLoop time within StateGameLoop.
	int64_t vehicletick_us = 0;    ///< CallVehicleTicks time within StateGameLoop.
	int tileloop_count = 0;        ///< Tiles processed in RunTileLoop.
	int gameloop_ticks = 0;        ///< Number of game ticks this period.

	/* Snapshot recording (CPU game thread, microseconds) */
	int64_t snap_record_us = 0;    ///< DrawDirtyBlocks recording time.
	int64_t snap_validate_us = 0;  ///< Coordinate validation time.
	int64_t snap_total_us = 0;     ///< Total snapshot path time (palette+record+validate+publish).
	int snap_commands = 0;         ///< Commands recorded per snapshot.

	/* Vehicle counts (snapshot from game thread) */
	int vehicle_trains = 0;        ///< Train vehicle count.
	int vehicle_road = 0;          ///< Road vehicle count.
	int vehicle_ships = 0;         ///< Ship count.
	int vehicle_aircraft = 0;      ///< Aircraft count.

	/* Viewport intelligence */
	int vp_sprites_generated = 0;  ///< Parent sprites generated (before sort/draw).

	/* Sprite loading pipeline */
	int sprite_cache_hits = 0;     ///< Sprites served from spritecache (no disk read).
	int sprite_loads = 0;          ///< Sprites loaded from disk (cache miss).
	int64_t sprite_disk_us = 0;    ///< GRF file read + RLE decode time.
	int64_t sprite_resize_us = 0;  ///< ResizeSprites (multi-zoom generation) time.
	int sprite_resize_in = 0;      ///< ResizeSpriteIn calls (upscale to root).
	int sprite_resize_out = 0;     ///< ResizeSpriteOut calls (generate lower zooms).
	int sprite_resize_pad = 0;     ///< PadSingleSprite calls.
	int sprite_avail_zooms = 0;    ///< Total zoom levels available from GRF.
	int sprite_generated_zooms = 0;///< Total zoom levels generated by resize.
	int64_t sprite_encode_us = 0;  ///< Blitter::Encode time (incl Stage).
	int64_t sprite_stage_us = 0;   ///< Atlas::Stage() time alone.
	int sprite_upload_count = 0;   ///< Atlas::Upload() calls (GPU thread).
	int64_t sprite_upload_us = 0;  ///< Atlas::Upload() time (pack + convert + GL).
	int sprite_staged_count = 0;   ///< Sprites currently in staged map.
	int64_t sprite_staged_bytes = 0; ///< RAM used by staged pixel data.

	int frames = 0;                ///< Frames in this measurement period.
};

extern GLESPerfCounters _gles_perf;

extern std::vector<Dimension> _resolutions;
extern Dimension _cur_resolution;
extern Palette _cur_palette; ///< Current palette

void HandleToolbarHotkey(int hotkey);
void HandleKeypress(uint keycode, char32_t key);
void HandleTextInput(std::string_view str, bool marked = false,
		std::optional<size_t> caret = std::nullopt,
		std::optional<size_t> insert_location = std::nullopt, std::optional<size_t> replacement_end = std::nullopt);
void HandleCtrlChanged();
void HandleMouseEvents();
void UpdateWindows();
void ChangeGameSpeed(bool enable_fast_forward);

void DrawMouseCursor();
void ScreenSizeChanged();
void GameSizeChanged();
void UpdateGUIZoom();
bool AdjustGUIZoom(bool automatic);
void UndrawMouseCursor();

void RedrawScreenRect(int left, int top, int right, int bottom);
void GfxScroll(int left, int top, int width, int height, int xo, int yo);

Dimension GetSpriteSize(SpriteID sprid, Point *offset = nullptr, ZoomLevel zoom = _gui_zoom);
Dimension GetScaledSpriteSize(SpriteID sprid); /* widget.cpp */
void DrawSpriteViewport(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub = nullptr);
void DrawSprite(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub = nullptr, ZoomLevel zoom = _gui_zoom);
void DrawSpriteIgnorePadding(SpriteID img, PaletteID pal, const Rect &r, StringAlignment align); /* widget.cpp */
std::unique_ptr<uint32_t[]> DrawSpriteToRgbaBuffer(SpriteID spriteId, ZoomLevel zoom = _gui_zoom);

int DrawString(int left, int right, int top, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = SA_LEFT, bool underline = false, FontSize fontsize = FS_NORMAL);
int DrawString(int left, int right, int top, StringID str, TextColour colour = TC_FROMSTRING, StringAlignment align = SA_LEFT, bool underline = false, FontSize fontsize = FS_NORMAL);
int DrawStringMultiLine(int left, int right, int top, int bottom, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL);
int DrawStringMultiLine(int left, int right, int top, int bottom, StringID str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL);
bool DrawStringMultiLineWithClipping(int left, int right, int top, int bottom, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL);

void DrawCharCentered(char32_t c, const Rect &r, TextColour colour);

void GfxFillRect(int left, int top, int right, int bottom, const std::variant<PixelColour, PaletteID> &colour, FillRectMode mode = FILLRECT_OPAQUE);
void GfxFillPolygon(std::span<const Point> shape, const std::variant<PixelColour, PaletteID> &colour, FillRectMode mode = FILLRECT_OPAQUE);
void GfxDrawLine(int left, int top, int right, int bottom, PixelColour colour, int width = 1, int dash = 0);
void DrawBox(int x, int y, int dx1, int dy1, int dx2, int dy2, int dx3, int dy3);
void DrawRectOutline(const Rect &r, PixelColour colour, int width = 1, int dash = 0);

/* Versions of DrawString/DrawStringMultiLine that accept a Rect instead of separate left, right, top and bottom parameters. */
inline int DrawString(const Rect &r, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = SA_LEFT, bool underline = false, FontSize fontsize = FS_NORMAL)
{
	return DrawString(r.left, r.right, r.top, str, colour, align, underline, fontsize);
}

inline int DrawString(const Rect &r, StringID str, TextColour colour = TC_FROMSTRING, StringAlignment align = SA_LEFT, bool underline = false, FontSize fontsize = FS_NORMAL)
{
	return DrawString(r.left, r.right, r.top, str, colour, align, underline, fontsize);
}

inline int DrawStringMultiLine(const Rect &r, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL)
{
	return DrawStringMultiLine(r.left, r.right, r.top, r.bottom, str, colour, align, underline, fontsize);
}

inline int DrawStringMultiLine(const Rect &r, StringID str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL)
{
	return DrawStringMultiLine(r.left, r.right, r.top, r.bottom, str, colour, align, underline, fontsize);
}

inline bool DrawStringMultiLineWithClipping(const Rect &r, std::string_view str, TextColour colour = TC_FROMSTRING, StringAlignment align = (SA_TOP | SA_LEFT), bool underline = false, FontSize fontsize = FS_NORMAL)
{
	return DrawStringMultiLineWithClipping(r.left, r.right, r.top, r.bottom, str, colour, align, underline, fontsize);
}

inline void GfxFillRect(const Rect &r, const std::variant<PixelColour, PaletteID> &colour, FillRectMode mode = FILLRECT_OPAQUE)
{
	GfxFillRect(r.left, r.top, r.right, r.bottom, colour, mode);
}

Dimension GetStringBoundingBox(std::string_view str, FontSize start_fontsize = FS_NORMAL);
Dimension GetStringBoundingBox(StringID strid, FontSize start_fontsize = FS_NORMAL);
uint GetStringListWidth(std::span<const StringID> list, FontSize fontsize = FS_NORMAL);
Dimension GetStringListBoundingBox(std::span<const StringID> list, FontSize fontsize = FS_NORMAL);
int GetStringHeight(std::string_view str, int maxw, FontSize fontsize = FS_NORMAL);
int GetStringHeight(StringID str, int maxw);
int GetStringLineCount(std::string_view str, int maxw);
Dimension GetStringMultiLineBoundingBox(StringID str, const Dimension &suggestion);
Dimension GetStringMultiLineBoundingBox(std::string_view str, const Dimension &suggestion, FontSize fontsize = FS_NORMAL);
void LoadStringWidthTable(FontSizes fontsizes = FONTSIZES_REQUIRED);

void DrawDirtyBlocks();
void AddDirtyBlock(int left, int top, int right, int bottom);
void MarkWholeScreenDirty();

void CheckBlitter();

bool FillDrawPixelInfo(DrawPixelInfo *n, int left, int top, int width, int height);

inline bool FillDrawPixelInfo(DrawPixelInfo *n, const Rect &r)
{
	return FillDrawPixelInfo(n, r.left, r.top, r.Width(), r.Height());
}

/* window.cpp */
void DrawOverlappedWindowForAll(int left, int top, int right, int bottom);

void SetMouseCursorBusy(bool busy);
void SetMouseCursor(CursorID cursor, PaletteID pal);
void SetAnimatedMouseCursor(std::span<const AnimCursor> table);
void SetCursor(CursorID cursor, PaletteID pal);
void CursorTick();
void UpdateCursorSize();
bool ChangeResInGame(int w, int h);
void SortResolutions();
bool ToggleFullScreen(bool fs);

/* gfx.cpp */
uint8_t GetCharacterWidth(FontSize size, char32_t key);
uint8_t GetDigitWidth(FontSize size = FS_NORMAL);
std::pair<uint8_t, uint8_t> GetBroadestDigit(FontSize size);

int GetCharacterHeight(FontSize size);

extern DrawPixelInfo *_cur_dpi;

#endif /* GFX_FUNC_H */
