/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sdl2_gles_v.cpp Implementation of the OpenGL ES backend for SDL2 video driver. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../core/geometry_func.hpp"
#include "../gfx_func.h"
#include "../spritecache.h"
#include "../blitter/factory.hpp"
#include "../blitter/gles.hpp"
#include "../debug.h"
#include "../framerate_type.h"
#include "../window_func.h"
#include "../window_gui.h"
#include "../viewport_type.h"
#include "../zoom_func.h"
#include "sdl2_gles_v.h"
#include "draw_snapshot.h"
#include "gles_backend.h"
#include "gles_poi.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../safeguards.h"

/** Set to true from Java on hide; processed in Paint() to advance camera to next POI. */
static std::atomic<bool> _gles_jump_waypoint{false};

#ifdef __ANDROID__
#include "../wallpaper.h"

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativePrepareBackground(JNIEnv *, jclass)
{
	_gles_jump_waypoint = true;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeSwitchMap(JNIEnv *, jclass)
{
	RotateTitleMap(1);
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
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
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

	/* Transfer sprites that were encoded before backend was ready. */
	FlushEarlyStaged();

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
	/* Two-threaded snapshot rendering: CPU thread records draw commands,
	 * GPU thread (main) renders from snapshots via lock-free triple buffer. */
	this->is_game_threaded = true;
	this->snapshot_buffer = std::make_unique<SnapshotTripleBuffer>();
	Debug(driver, 0, "GLES: snapshot rendering enabled, is_game_threaded=true");

	Debug(driver, 0, "GLES: SDL_Base::Start OK, window={}", (void *)this->sdl_window);

	error = this->AllocateContext();
	if (error) {
		Debug(driver, 0, "GLES: AllocateContext failed: {}", *error);
		this->Stop();
		return error;
	}
	Debug(driver, 0, "GLES: AllocateContext OK");

	/* Select the snapshot blitter for two-thread recording. */
	if (BlitterFactory::SelectBlitter("snapshot") == nullptr) {
		Debug(driver, 0, "GLES: Could not select 'snapshot' blitter, falling back to '32bpp-optimized'");
		BlitterFactory::SelectBlitter("32bpp-optimized");
	}

	/* Prevent SwitchNewGRFBlitter() from replacing our GLES blitter. */
	_blitter_autodetected = false;

	/* Enable dirty block coalescing for GLES (single RedrawScreenRect). */
	_gles_video_active = true;


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

void VideoDriver_SDL_GLES::MakeDirty(int left, int top, int width, int height)
{
	Rect r = {left, top, left + width, top + height};
	this->gles_dirty_rects.push_back(r);
	this->dirty_rect = BoundingRect(this->dirty_rect, r);
}

void VideoDriver_SDL_GLES::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;

	if (this->snapshot_buffer != nullptr) {
		/* Snapshot mode: palette change is handled by the resolve pass
		 * in Paint(). No need for MarkWholeScreenDirty(). */
		return;
	}
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

bool VideoDriver_SDL_GLES::PaintFromSnapshot()
{
	if (this->snapshot_buffer == nullptr) return false;

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return false;

	/* Try to acquire a new snapshot from triple buffer. */
	bool new_snapshot = this->snapshot_buffer->Acquire();
	DrawSnapshot &snap = this->snapshot_buffer->GetReadBuffer();

	if (new_snapshot && !snap.commands.empty()) {
		auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };

		/* Process deferred atlas clear before replaying. */
		auto t0 = std::chrono::steady_clock::now();
		backend->GetSpriteAtlas().ProcessPendingClear();
		auto t1 = std::chrono::steady_clock::now();
		_gles_perf.snap_clear_us += us(t0, t1);

		/* Process PBO uploads so newly enqueued sprites are available for LookupOrUpload. */
		backend->GetSpriteAtlas().ProcessPBOUploads();
		auto t2 = std::chrono::steady_clock::now();
		_gles_perf.snap_pbo_us += us(t1, t2);

		/* Replay draw commands into the GPU queue.
		 * Use read-only Lookup; missing sprites are deferred to after SwapWindow
		 * so that uploads don't add jitter to frame time. */
		GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
		backend->ClearQueue();
		this->deferred_upload_keys.clear();

		int replayed = 0, null_entries = 0;
		for (const auto &cmd : snap.commands) {
			GLESSpriteID key = MakeGLESSpriteKey(cmd.sprite, cmd.zoom);
			const GLESSpriteEntry *entry = atlas.Lookup(key);
			if (entry == nullptr) {
				this->deferred_upload_keys.push_back(key);
				null_entries++;
				continue;
			}

			GLESDrawCommand gcmd;
			gcmd.sprite_key = key;
			gcmd.screen_x = cmd.x;
			gcmd.screen_y = cmd.y;
			gcmd.width = cmd.width;
			gcmd.height = cmd.height;
			gcmd.skip_left = cmd.skip_left;
			gcmd.skip_top = cmd.skip_top;
			gcmd.sprite_width = cmd.sprite_width;
			gcmd.sprite_height = cmd.sprite_height;
			gcmd.zoom = cmd.zoom;
			gcmd.mode = cmd.mode;
			gcmd.remap_idx = 0;
			gcmd.remap = cmd.remap;
			gcmd.palette_only = entry->palette_only;

			backend->QueueDraw(gcmd);
			replayed++;
		}
		auto t3 = std::chrono::steady_clock::now();
		_gles_perf.snap_replay_us += us(t2, t3);
		_gles_perf.snap_replayed_cmds += replayed;
		_gles_perf.snap_null_entries += null_entries;

		backend->AddDirtyRect(0, 0, _screen.width, _screen.height);

		this->CheckPaletteAnim();

		/* Upload palette and render sprites into FBO. */
		GLESBackend::Get()->UpdatePalette(this->local_palette.palette, 0, 256);
		GLESBackend::Get()->SetPaletteDirty(true);
		this->local_palette.count_dirty = 0;
		auto t4 = std::chrono::steady_clock::now();
		_gles_perf.snap_palette_us += us(t3, t4);

		_gles_perf.gpu_draw_cmds += static_cast<int>(backend->GetDrawQueueSize());
		backend->PaintFBO();
		auto t5 = std::chrono::steady_clock::now();
		_gles_perf.gpu_paint_us += us(t4, t5);

		/* Record snapshot scroll state for interpolation.
		 * prev = where we were (previous snapshot), curr = where FBO is now.
		 * We interpolate from prev→curr over one tick, so at t=1 we match the FBO. */
		this->snap_prev_scrollpos_x = this->snap_curr_scrollpos_x;
		this->snap_prev_scrollpos_y = this->snap_curr_scrollpos_y;
		this->snap_curr_scrollpos_x = snap.scrollpos_x;
		this->snap_curr_scrollpos_y = snap.scrollpos_y;
		this->snap_scroll_zoom = snap.scroll_zoom;
		this->snap_time = std::chrono::steady_clock::now();
	} else if (!backend->HasFBOContent()) {
		return false;
	}

	auto t_blit0 = std::chrono::steady_clock::now();
	backend->BlitToScreen(0.0f, 0.0f);
	auto t_blit1 = std::chrono::steady_clock::now();
	SDL_GL_SwapWindow(this->sdl_window);
	auto t_swap1 = std::chrono::steady_clock::now();

	{
		auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
		_gles_perf.blit_to_screen_us += us(t_blit0, t_blit1);
		_gles_perf.swap_us += us(t_blit1, t_swap1);
	}
	_gles_perf.frames++;

	/* Upload missing sprites after SwapWindow (idle time between frames).
	 * They'll appear next frame — 1 frame latency for new sprites only. */
	if (!this->deferred_upload_keys.empty()) {
		GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
		atlas.ResetFrameLoadCounter();
		for (GLESSpriteID key : this->deferred_upload_keys) {
			atlas.LookupOrUpload(key);
		}
		this->deferred_upload_keys.clear();
	}

	/* Run Paint() for PERF logging, POI handling, context recovery.
	 * It early-returns before GL work in snapshot mode. */
	this->Paint();

	return true;
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
		/* Trigger a full map reload so sprites are re-encoded from scratch. */
		_switch_mode = (_game_mode == GM_WALLPAPER) ? SM_WALLPAPER : SM_MENU;
		/* Force a full-screen dirty so the viewport redraws next frame. */
		this->MakeDirty(0, 0, _screen.width, _screen.height);
		return; /* Skip this frame; draw_queue was cleared, FBO is black anyway. */
	}

	/* Jump to next POI (requested from Java onVisibilityChanged hide). */
	if (_gles_jump_waypoint.exchange(false)) {
		PrepareBackground();
	}

	/* Log EGL context state every 60 frames to detect context loss. */
	static int paint_count = 0;
	paint_count++;
	if (paint_count % 60 == 1) {
		EGLContext ctx = eglGetCurrentContext();
		EGLDisplay dpy = eglGetCurrentDisplay();
		EGLSurface srf = eglGetCurrentSurface(EGL_DRAW);
		EGLint err = eglGetError();
		Debug(driver, 3, "GLES ctx: frame={} context={} display={} surface={} egl_err=0x{:04X}",
			paint_count, (void *)ctx, (void *)dpy, (void *)srf, err);
		if (ctx == EGL_NO_CONTEXT) {
			Debug(driver, 0, "GLES ctx: WARNING - EGL_NO_CONTEXT! Context has been lost.");
		}
	}

	static int fps_frames = 0;
	static auto fps_last = std::chrono::steady_clock::now();

	/* Frame time ring buffer for jank detection and statistics (persists across 500ms windows). */
	static int64_t ft_ring[128] = {};
	static int ft_idx = 0, ft_count = 0;

	fps_frames++;
	auto fps_now = std::chrono::steady_clock::now();

	/* Record per-frame time and detect jank. */
	if (_gles_perf.last_frame_us > 0) {
		ft_ring[ft_idx] = _gles_perf.last_frame_us;
		ft_idx = (ft_idx + 1) % 128;
		ft_count = std::min(ft_count + 1, 128);

		if (ft_count > 4) {
			int64_t sum = 0;
			for (int i = 0; i < ft_count; i++) sum += ft_ring[i];
			int64_t avg = sum / ft_count;
			if (_gles_perf.last_frame_us > avg * 2) _gles_perf.jank_count++;
		}
	}

	if (fps_now - fps_last >= std::chrono::milliseconds(500)) {
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - fps_last).count();
		int fps = (elapsed_ms > 0) ? static_cast<int>(fps_frames * 1000 / elapsed_ms) : 0;
		auto &p = _gles_perf;
		int n = std::max(1, p.frames);
		auto &atlas = GLESBackend::Get()->GetSpriteAtlas();
		Debug(driver, 0, "PERF fps={} frames={} | blit_calls={} gpu_cmds={} atlas_miss={} offscreen_skip={} | gl_batches={} reuploaded={} dim_mismatch={} zoom=[{}/{}/{}/{}/{}/{}] scaled_hits={} scaled_fallback={} gpu_paint={}us egl_swap={}us | encode: total={} uploaded={} all_transparent={} | atlas: color_pages={} remap_pages={} gpu_entries={} registered={} new_sprites={} repacked={} | gl: loads={} fails={} budget_skip={} pbo_up={} lookup={}/{}",
			fps, p.frames,
			p.blit_draw_calls / n, p.gpu_draw_cmds / n, p.gpu_sprites_missing, p.gpu_skip_offscreen,
			p.gpu_batches / n, p.gpu_sprites_reuploaded, p.gpu_dim_mismatches,
			p.gpu_zoom_counts[0], p.gpu_zoom_counts[1], p.gpu_zoom_counts[2], p.gpu_zoom_counts[3], p.gpu_zoom_counts[4], p.gpu_zoom_counts[5],
			p.gpu_scaled_hits, p.gpu_scaled_fallbacks,
			p.gpu_paint_us / n, p.swap_us / n,
			p.encode_total, p.encode_uploaded, p.encode_all_transparent,
			atlas.GetColourPageCount(), atlas.GetRemapPageCount(),
			atlas.GetSpriteCount(), GetRegisteredSpriteCount(),
			p.gpu_sprites_new, p.gpu_sprites_repacked,
			p.gl_thread_loads, p.gl_thread_fails, p.gl_budget_skips, p.pbo_uploaded_this_period, p.lookup_hits, p.lookup_total);
		Debug(driver, 0, "  VP landscape={}us vehicles={}us ground_sprites={}us sprite_sort={}us sprite_draw={}us update_windows={}us | tiles_iterated={} parent_sprites={} child_sprites={} sprites_generated={} vp_draw_calls={} viewport={}x{} | mrt: full_renders={} resolve_only={} idle_blit={} resolve={}us",
			p.vp_land_us / n, p.vp_vehicles_us / n, p.vp_signs_tiles_us / n,
			p.vp_sort_us / n, p.vp_draw_us / n, p.update_windows_us / n,
			p.vp_tiles_iterated / n, p.vp_parent_sprites / n, p.vp_child_sprites / n,
			p.vp_sprites_generated / n,
			p.vp_calls, p.vp_area_w, p.vp_area_h,
			p.full_renders, p.resolve_passes, p.idle_blits, p.resolve_us / n);
		auto tick_accounted = p.lock_video_us + p.mutex_wait_us + p.input_poll_us + p.update_windows_us + p.populate_us + p.check_palette_us + p.paint_full_us + p.unlock_video_us;
		Debug(driver, 0, "  TICK total={}us | lock_video={}us game_mutex={}us(skipped={}) input_poll={}us update_windows={}us populate_sprites={}us check_palette={}us paint={}us(gpu_render={}us egl_swap={}us) unlock_video={}us unaccounted={}us",
			p.tick_total_us / n,
			p.lock_video_us / n, p.mutex_wait_us / n, p.mutex_skipped, p.input_poll_us / n,
			p.update_windows_us / n, p.populate_us / n, p.check_palette_us / n,
			p.paint_full_us / n, p.gpu_paint_us / n, p.swap_us / n, p.unlock_video_us / n,
			(p.tick_total_us - tick_accounted) / n);

		/* === 4th PERF line: extended metrics === */
		{
			/* Frame time statistics from ring buffer. */
			int64_t ft_sum = 0;
			for (int i = 0; i < ft_count; i++) ft_sum += ft_ring[i];
			int64_t ft_avg = ft_count > 0 ? ft_sum / ft_count : 0;
			int64_t ft_var = 0;
			for (int i = 0; i < ft_count; i++) {
				int64_t d = ft_ring[i] - ft_avg;
				ft_var += d * d;
			}
			ft_var = ft_count > 1 ? ft_var / (ft_count - 1) : 0;
			int ft_stddev = static_cast<int>(std::sqrt(static_cast<double>(ft_var)));

			/* Percentiles from sorted copy. */
			int64_t ft_sorted[128];
			std::copy(ft_ring, ft_ring + ft_count, ft_sorted);
			std::sort(ft_sorted, ft_sorted + ft_count);
			int ft_p95 = ft_count > 0 ? static_cast<int>(ft_sorted[ft_count * 95 / 100]) : 0;
			int ft_p99 = ft_count > 0 ? static_cast<int>(ft_sorted[ft_count * 99 / 100]) : 0;

			/* Derived metrics. */
			int screen_area = GLESBackend::Get()->GetScreenWidth() * GLESBackend::Get()->GetScreenHeight();
			float overdraw = (screen_area > 0 && n > 0) ? static_cast<float>(p.blit_draw_pixels) / (static_cast<float>(screen_area) * n) : 0;
			float batch_eff = p.gpu_batches > 0 ? static_cast<float>(p.gpu_draw_cmds) / p.gpu_batches : 0;
			int atlas_lookups = p.blit_draw_calls;
			int atlas_misses = p.gpu_sprites_missing + p.gpu_sprites_new;
			float cache_hit = atlas_lookups > 0 ? (1.0f - static_cast<float>(atlas_misses) / atlas_lookups) * 100.0f : 100.0f;

			{
			int gt = std::max(1, p.gameloop_ticks);
			Debug(driver, 0, "  CPU gameloop={}us snap_total={}us(record={}us validate={}us) cmds={} | tileloop={}us({}tiles) vehtick={}us ticks={}",
				p.gameloop_us / gt,
				p.snap_total_us / gt, p.snap_record_us / gt, p.snap_validate_us / gt,
				p.snap_commands / gt,
				p.tileloop_us / gt, p.tileloop_count,
				p.vehicletick_us / gt, p.gameloop_ticks);
		}
		Debug(driver, 0, "  EXTRA gpu_actual={}us | overdraw={:.1f}x batch_eff={:.1f} cache_hit={:.1f}% | atlas_occ: color={}% remap={}% | jank={} stddev={}us p95={}us p99={}us | vehicles: T={} R={} S={} A={}",
				p.gpu_time_us,
				overdraw, batch_eff, cache_hit,
				atlas.GetColourOccupancyPercent(), atlas.GetRemapOccupancyPercent(),
				p.jank_count, ft_stddev, ft_p95, ft_p99,
				p.vehicle_trains, p.vehicle_road, p.vehicle_ships, p.vehicle_aircraft);
		Debug(driver, 0, "  GPU_SNAP clear={}us pbo={}us replay={}us(cmds={} null={}) palette={}us paint={}us blit={}us swap={}us",
				p.snap_clear_us / n, p.snap_pbo_us / n, p.snap_replay_us / n,
				p.snap_replayed_cmds / n, p.snap_null_entries / n,
				p.snap_palette_us / n, p.gpu_paint_us / n,
				p.blit_to_screen_us / n, p.swap_us / n);
		}

		p = {};  /* Reset counters. */
		fps_frames = 0;
		fps_last = fps_now;
	}

	/* In snapshot mode, PaintFromSnapshot handles palette, FBO render, blit and swap. */
	if (this->snapshot_buffer != nullptr) return;

	/* Always upload full palette every frame — treat it as perpetually dirty. */
	GLESBackend::Get()->UpdatePalette(this->local_palette.palette, 0, 256);
	GLESBackend::Get()->SetPaletteDirty(true);
	this->local_palette.count_dirty = 0;

	/* Forward individual dirty rectangles to the GLES backend. */
	if (GLESBackend::Get() != nullptr) {
		for (const Rect &r : this->gles_dirty_rects) {
			GLESBackend::Get()->AddDirtyRect(r.left, r.top, r.right, r.bottom);
		}
		this->gles_dirty_rects.clear();
	}

	this->dirty_rect = {};

	auto t_upload0 = std::chrono::steady_clock::now();
	auto t_upload1 = t_upload0;

	_gles_perf.gpu_draw_cmds += static_cast<int>(GLESBackend::Get()->GetDrawQueueSize());
	bool did_render = GLESBackend::Get()->Paint();
	auto t_paint1 = std::chrono::steady_clock::now();

	if (did_render) {
		SDL_GL_SwapWindow(this->sdl_window);
	}
	auto t_swap1 = std::chrono::steady_clock::now();

	/* Accumulate GPU timing. */
	auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
	_gles_perf.upload_us += us(t_upload0, t_upload1);
	_gles_perf.gpu_paint_us += us(t_upload1, t_paint1);
	_gles_perf.swap_us += us(t_paint1, t_swap1);
	_gles_perf.frames++;
}
