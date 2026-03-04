/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sdl2_gles_v.cpp Implementation of the OpenGL ES backend for SDL2 video driver. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../spritecache.h"
#include "../blitter/factory.hpp"
#include "../debug.h"
#include "../framerate_type.h"
#include "../map_func.h"
#include "../tile_type.h"
#include "../viewport_func.h"
#include "../window_func.h"
#include "../station_base.h"
#include "../town.h"
#include "sdl2_gles_v.h"
#include "gles_backend.h"
#include "gles_waypoints.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <atomic>
#include <cstdlib>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../safeguards.h"

/** Set to true from Java before pause; processed in Paint() to advance camera to next POI. */
static std::atomic<bool> _gles_jump_waypoint{false};

/** Scanned POI list (top 10 by score), rebuilt when map changes. */
static std::vector<GlesPOI> _gles_poi_list;
static int _gles_poi_idx = 0;
static uint _gles_poi_map_tiles = 0; ///< Map::SizeX()*SizeY() at last scan; triggers rescan on change.

/**
 * Score and collect all interesting locations on the current map.
 *
 * Scoring:
 *  Airport          +4
 *  Rail station     +3
 *  Dock             +2
 *  Bus/truck stop   +1 each
 *  Nearby town pop  +1 per 500 people (up to +5, within 50 tiles)
 *
 * Stations dominate; large uncovered towns are added as fallback.
 * Results are sorted by score descending, top 10 kept.
 */
static void ScanMapPOIs()
{
	_gles_poi_list.clear();
	_gles_poi_map_tiles = Map::SizeX() * Map::SizeY();

	std::vector<GlesPOI> candidates;
	candidates.reserve(64);

	/* --- Stations -------------------------------------------------------- */
	for (Station *st : Station::Iterate()) {
		if (st->xy == INVALID_TILE) continue;

		int score = 0;
		if (st->facilities.Test(StationFacility::Airport))   score += 4;
		if (st->facilities.Test(StationFacility::Train))     score += 3;
		if (st->facilities.Test(StationFacility::Dock))      score += 2;
		if (st->facilities.Test(StationFacility::BusStop))   score += 1;
		if (st->facilities.Test(StationFacility::TruckStop)) score += 1;
		if (score == 0) continue;

		/* Bonus from nearest large town within 50 tiles. */
		for (Town *t : Town::Iterate()) {
			if (t->xy == INVALID_TILE) continue;
			if (DistanceManhattan(st->xy, t->xy) < 50) {
				score += std::min(5, (int)(t->cache.population / 500));
				break; /* one bonus per station */
			}
		}

		float fx = (float)TileX(st->xy) / Map::SizeX();
		float fy = (float)TileY(st->xy) / Map::SizeY();
		int   zoom = (score >= 8) ? 1 : (score >= 5 ? 0 : -1);
		candidates.push_back({fx, fy, score, zoom, 5000});
	}

	/* --- Towns not already covered by a nearby station ------------------- */
	for (Town *t : Town::Iterate()) {
		if (t->xy == INVALID_TILE || t->cache.population < 500) continue;

		/* Skip if a station POI already represents this town (within 30 tiles). */
		bool covered = false;
		for (const GlesPOI &poi : candidates) {
			TileIndex poi_tile = TileXY(
				(uint)(poi.map_fx * Map::SizeX()),
				(uint)(poi.map_fy * Map::SizeY()));
			if (DistanceManhattan(t->xy, poi_tile) < 30) { covered = true; break; }
		}
		if (covered) continue;

		int score = std::min(5, (int)(t->cache.population / 500));
		float fx  = (float)TileX(t->xy) / Map::SizeX();
		float fy  = (float)TileY(t->xy) / Map::SizeY();
		candidates.push_back({fx, fy, score, 0, 5000});
	}

	/* Sort by score descending, keep top 10. */
	std::sort(candidates.begin(), candidates.end(),
		[](const GlesPOI &a, const GlesPOI &b) { return a.score > b.score; });

	int n = std::min(10, (int)candidates.size());
	_gles_poi_list.assign(candidates.begin(), candidates.begin() + n);
	_gles_poi_idx = 0;

	Debug(driver, 0, "GLES ScanMapPOIs: {} POIs (map {}x{})",
		(int)_gles_poi_list.size(), Map::SizeX(), Map::SizeY());
	for (int i = 0; i < (int)_gles_poi_list.size(); i++) {
		const GlesPOI &p = _gles_poi_list[i];
		Debug(driver, 0, "  POI[{}] score={} fx={:.2f} fy={:.2f} zoom={}", i, p.score, p.map_fx, p.map_fy, p.zoom_adjust);
	}
}

/**
 * Advance the camera to the next POI in the ranked rotation.
 * Falls back to kDefaultWaypoints if no POIs were found.
 */
static void PrepareBackground()
{
	if (Map::SizeX() == 0 || Map::SizeY() == 0) return;

	/* Rescan if map changed or not yet scanned. */
	if (_gles_poi_list.empty() || Map::SizeX() * Map::SizeY() != _gles_poi_map_tiles) {
		ScanMapPOIs();
	}

	float fx, fy;
	int zoom_adjust;

	if (!_gles_poi_list.empty()) {
		/* Cycle through POIs in score order. */
		const GlesPOI &poi = _gles_poi_list[_gles_poi_idx];
		fx          = poi.map_fx;
		fy          = poi.map_fy;
		zoom_adjust = poi.zoom_adjust;
		_gles_poi_idx = (_gles_poi_idx + 1) % (int)_gles_poi_list.size();
		Debug(driver, 0, "GLES PrepareBackground: POI[{}] score={} fx={:.2f} fy={:.2f}",
			_gles_poi_idx == 0 ? (int)_gles_poi_list.size() - 1 : _gles_poi_idx - 1,
			_gles_poi_list[_gles_poi_idx == 0 ? (int)_gles_poi_list.size() - 1 : _gles_poi_idx - 1].score,
			fx, fy);
	} else {
		/* Fallback: random static waypoint. */
		int idx = std::rand() % (int)kDefaultWaypoints.size();
		fx          = kDefaultWaypoints[idx].map_fx;
		fy          = kDefaultWaypoints[idx].map_fy;
		zoom_adjust = kDefaultWaypoints[idx].zoom_adjust;
	}

	int world_x = (int)(fx * Map::SizeX() * TILE_SIZE);
	int world_y = (int)(fy * Map::SizeY() * TILE_SIZE);
	ScrollMainWindowTo(world_x, world_y, -1, true);
	FixTitleGameZoom(zoom_adjust);
	MarkWholeScreenDirty();
}

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativePrepareBackground(JNIEnv *, jclass)
{
	_gles_jump_waypoint = true;
}
#endif

static FVideoDriver_SDL_GLES iFVideoDriver_SDL_GLES;

bool VideoDriver_SDL_GLES::CreateMainWindow(uint w, uint h, uint flags)
{
	return this->VideoDriver_SDL_Base::CreateMainWindow(w, h, flags | SDL_WINDOW_OPENGL);
}

std::optional<std::string_view> VideoDriver_SDL_GLES::AllocateContext()
{
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	this->gl_context = SDL_GL_CreateContext(this->sdl_window);
	if (this->gl_context == nullptr) {
		Debug(driver, 0, "GLES: SDL_GL_CreateContext failed: {}", SDL_GetError());
		return "SDL2: Can't create GLES context";
	}

	Debug(driver, 0, "GLES: EGL after CreateContext: display={} surface={} context={} err=0x{:04X}",
		(void *)eglGetCurrentDisplay(), (void *)eglGetCurrentSurface(EGL_DRAW),
		(void *)eglGetCurrentContext(), eglGetError());

	if (!GLESBackend::Create()) return "Failed to initialize GLES backend";

	return std::nullopt;
}

void VideoDriver_SDL_GLES::DestroyContext()
{
	GLESBackend::Destroy();

	if (this->gl_context != nullptr) {
		SDL_GL_DeleteContext(this->gl_context);
		this->gl_context = nullptr;
	}
}

std::optional<std::string_view> VideoDriver_SDL_GLES::Start(const StringList &param)
{
	Debug(driver, 0, "GLES: Start() entering, calling SDL_Base::Start...");
	auto error = VideoDriver_SDL_Base::Start(param);
	if (error) {
		Debug(driver, 0, "GLES: SDL_Base::Start failed: {}", *error);
		return error;
	}
	/* Threaded mode: Encode() stages pixel data (no GL calls), Draw()
	 * lazily uploads to GPU on first use from the GL thread. */
	this->is_game_threaded = true;

	Debug(driver, 0, "GLES: SDL_Base::Start OK, window={}", (void *)this->sdl_window);

	error = this->AllocateContext();
	if (error) {
		Debug(driver, 0, "GLES: AllocateContext failed: {}", *error);
		this->Stop();
		return error;
	}
	Debug(driver, 0, "GLES: AllocateContext OK");

	/* Select the GLES blitter to enable GPU sprite recording. */
	if (BlitterFactory::SelectBlitter("gles") == nullptr) {
		/* Fall back: the blitter might not be registered. */
		Debug(driver, 0, "GLES: Could not select 'gles' blitter, falling back to '32bpp-optimized'");
		BlitterFactory::SelectBlitter("32bpp-optimized");
	}

	/* Prevent SwitchNewGRFBlitter() from replacing our GLES blitter. */
	_blitter_autodetected = false;

	/* Enable dirty block coalescing for GLES (single RedrawScreenRect). */
	_gles_video_active = true;

	/* Enable GPU sprite rendering — blitter queues draw commands instead of CPU blitting. */
	_gles_gpu_sprites = true;

	/* Force a client-size-changed event to allocate buffers. */
	int w, h;
	SDL_GetWindowSize(this->sdl_window, &w, &h);
	this->ClientSizeChanged(w, h, true);

	if (_screen.dst_ptr == nullptr) {
		this->Stop();
		return "Can't get pointer to screen buffer";
	}

	return std::nullopt;
}

void VideoDriver_SDL_GLES::Stop()
{
	_gles_gpu_sprites = false;
	_gles_video_active = false;
	this->DestroyContext();
	this->VideoDriver_SDL_Base::Stop();
}

void VideoDriver_SDL_GLES::ToggleVsync(bool vsync)
{
	SDL_GL_SetSwapInterval(vsync ? 1 : 0);
}

bool VideoDriver_SDL_GLES::AllocateBackingStore(int w, int h, bool force)
{
	Debug(driver, 0, "GLES AllocateBackingStore: w={} h={} force={} gl_context={}", w, h, force, (void *)this->gl_context);
	if (this->gl_context == nullptr) return false;

	w = std::max(w, 64);
	h = std::max(h, 64);

	this->video_buffer.resize(static_cast<size_t>(w) * h);
	std::fill(this->video_buffer.begin(), this->video_buffer.end(), 0);

	_screen.width = w;
	_screen.height = h;
	_screen.pitch = w;
	_screen.dst_ptr = this->video_buffer.data();

	GLESBackend::Get()->Resize(w, h);

	CopyPalette(this->local_palette, true);

	return true;
}

void *VideoDriver_SDL_GLES::GetVideoPointer()
{
	return this->video_buffer.data();
}

void VideoDriver_SDL_GLES::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;

	if (_gles_gpu_sprites) {
		/* GPU sprites: mark all dirty blocks so DrawDirtyBlocks() re-renders
		 * the full viewport next frame with new palette colours.  Do NOT call
		 * MakeDirty() here — it would clear the FBO this frame while sprites
		 * are only queued next frame, causing a black flash. */
		MarkWholeScreenDirty();
		return;
	}
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

void VideoDriver_SDL_GLES::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

	/* Recover from GL context loss (SDL_RENDER_DEVICE_RESET).
	 * Must run at the top of Paint() — we're on the GL thread with the new context current. */
	if (_gles_context_lost && GLESBackend::Get() != nullptr) {
		_gles_context_lost = false;
		Debug(driver, 0, "GLES: Paint: recovering from context loss");
		GLESBackend::Get()->RecoverGPUState();
		/* Force full palette re-upload into the new palette texture. */
		CopyPalette(this->local_palette, true);
		/* Force a full-screen dirty so the viewport redraws next frame. */
		this->MakeDirty(0, 0, _screen.width, _screen.height);
		return; /* Skip this frame; draw_queue was cleared, FBO is black anyway. */
	}

	/* Jump to a random waypoint before pause (requested from Java onVisibilityChanged). */
	if (_gles_jump_waypoint.exchange(false)) PrepareBackground();

	/* Log EGL context state every 60 frames to detect context loss. */
	static int paint_count = 0;
	paint_count++;
	if (paint_count % 60 == 1) {
		EGLContext ctx = eglGetCurrentContext();
		EGLDisplay dpy = eglGetCurrentDisplay();
		EGLSurface srf = eglGetCurrentSurface(EGL_DRAW);
		EGLint err = eglGetError();
		Debug(driver, 0, "GLES ctx: frame={} context={} display={} surface={} egl_err=0x{:04X}",
			paint_count, (void *)ctx, (void *)dpy, (void *)srf, err);
		if (ctx == EGL_NO_CONTEXT) {
			Debug(driver, 0, "GLES ctx: WARNING - EGL_NO_CONTEXT! Context has been lost.");
		}
	}

	static int fps_frames = 0;
	static auto fps_last = std::chrono::steady_clock::now();
	fps_frames++;
	auto fps_now = std::chrono::steady_clock::now();
	if (fps_now - fps_last >= std::chrono::milliseconds(500)) {
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - fps_last).count();
		int fps = (elapsed_ms > 0) ? static_cast<int>(fps_frames * 1000 / elapsed_ms) : 0;
		auto &p = _gles_perf;
		int n = std::max(1, p.frames);
		auto &atlas = GLESBackend::Get()->GetSpriteAtlas();
		Debug(driver, 0, "PERF fps={} frames={} | blit: draws={} gpu_cmds={} miss={} offscr={} | gpu: batches={} reup={} dimmis={} zoom=[{}/{}/{}/{}/{}/{}] paint={}us swap={}us | enc: total={} up={} transp={} | atlas: cpages={} rpages={} gpu={} stored={} reg={} new={} repacked={}",
			fps, p.frames,
			p.blit_draw_calls / n, p.gpu_draw_cmds / n, p.gpu_sprites_missing, p.gpu_skip_offscreen,
			p.gpu_batches / n, p.gpu_sprites_reuploaded, p.gpu_dim_mismatches,
			p.gpu_zoom_counts[0], p.gpu_zoom_counts[1], p.gpu_zoom_counts[2], p.gpu_zoom_counts[3], p.gpu_zoom_counts[4], p.gpu_zoom_counts[5],
			p.gpu_paint_us / n, p.swap_us / n,
			p.encode_total, p.encode_uploaded, p.encode_all_transparent,
			atlas.GetColourPageCount(), atlas.GetRemapPageCount(),
			atlas.GetSpriteCount(), atlas.GetStoredSpriteCount(), GetRegisteredSpriteCount(),
			p.gpu_sprites_new, p.gpu_sprites_repacked);
		Debug(driver, 0, "  VP land={}us vehi={}us signs={}us sort={}us draw={}us updwin={}us | tiles={} parents={} children={} calls={} area={}x{}",
			p.vp_land_us / n, p.vp_vehicles_us / n, p.vp_signs_tiles_us / n,
			p.vp_sort_us / n, p.vp_draw_us / n, p.update_windows_us / n,
			p.vp_tiles_iterated / n, p.vp_parent_sprites / n, p.vp_child_sprites / n,
			p.vp_calls, p.vp_area_w, p.vp_area_h);
		p = {};  /* Reset counters. */
		fps_frames = 0;
		fps_last = fps_now;
	}

	if (this->local_palette.count_dirty != 0) {
		GLESBackend::Get()->UpdatePalette(this->local_palette.palette,
			this->local_palette.first_dirty, this->local_palette.count_dirty);
		this->local_palette.count_dirty = 0;
	}

	/* Forward dirty rectangles to the GLES backend for selective FBO clearing. */
	if (_gles_gpu_sprites && this->dirty_rect.right > this->dirty_rect.left) {
		GLESBackend::Get()->AddDirtyRect(
			this->dirty_rect.left, this->dirty_rect.top,
			this->dirty_rect.right, this->dirty_rect.bottom);
	}

	auto t_upload0 = std::chrono::steady_clock::now();

	if (!_gles_gpu_sprites) {
		/* CPU mode: upload dirty rows of CPU buffer to GPU texture. */
		Rect upload_dirty = this->dirty_rect;
		GLESBackend::Get()->UploadVideoBuffer(this->video_buffer.data(),
			_screen.width, _screen.height, upload_dirty);
	}
	/* GPU sprites mode: no CPU buffer upload needed. */

	this->dirty_rect = {};
	auto t_upload1 = std::chrono::steady_clock::now();

	_gles_perf.gpu_draw_cmds += static_cast<int>(GLESBackend::Get()->GetDrawQueueSize());
	GLESBackend::Get()->Paint();
	auto t_paint1 = std::chrono::steady_clock::now();

	SDL_GL_SwapWindow(this->sdl_window);
	auto t_swap1 = std::chrono::steady_clock::now();

	/* Accumulate GPU timing. */
	auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
	_gles_perf.upload_us += us(t_upload0, t_upload1);
	_gles_perf.gpu_paint_us += us(t_upload1, t_paint1);
	_gles_perf.swap_us += us(t_paint1, t_swap1);
	_gles_perf.frames++;
}
