/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sdl2_gles_v.cpp Implementation of the OpenGL ES backend for SDL2 video driver. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../core/bitmath_func.hpp"
#include "../core/geometry_func.hpp"
#include "../gfx_func.h"
#include "../spritecache.h"
#include "../blitter/factory.hpp"
#include "../blitter/snapshot.hpp"
#include "../debug.h"
#include "../framerate_type.h"
#include "../window_func.h"
#include "../window_gui.h"
#include "../viewport_type.h"
#include "../viewport_func.h"
#include "../zoom_func.h"
#include "../palette_func.h"
#include "../wallpaper.h"
#include "../table/sprites.h"
#include "sdl2_gles_v.h"
#include "draw_snapshot.h"
#include "gles_backend.h"
#include "gles_poi.h"
#include "gles_perf.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#ifdef __ANDROID__
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#endif

#include "../safeguards.h"

/** Set to true from Java on hide; processed in Tick() to advance camera to next POI. */
std::atomic<bool> _gles_jump_waypoint{false};
/** POI navigation delta requested from Java; processed in Tick(). */
std::atomic<int> _gles_navigate_poi{0};
/** Map rotation delta requested from Java; processed in Tick(). */
std::atomic<int> _gles_rotate_map{0};
/** Camera scroll delta requested from Java; processed in Tick(). */
std::atomic<int> _gles_scroll_dx{0};
std::atomic<int> _gles_scroll_dy{0};
/** Set from Java when the wallpaper surface changes; GL thread re-binds EGL. */
std::atomic<bool> _gles_surface_changed{false};

#ifdef __ANDROID__

static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context *context, void *arg)
{
	int *depth = static_cast<int *>(arg);
	uintptr_t pc = _Unwind_GetIP(context);
	if (pc) {
		Dl_info info;
		if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_sname) {
			__android_log_print(4, "OpenTTD", "  #%d: %s (%s+%p)", *depth, info.dli_sname, info.dli_fname, reinterpret_cast<void *>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
		} else {
			__android_log_print(4, "OpenTTD", "  #%d: pc=%p", *depth, reinterpret_cast<void *>(pc));
		}
	}
	(*depth)++;
	return (*depth > 20) ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void crash_handler(int sig)
{
	__android_log_print(6, "OpenTTD", "CRASH: signal %d", sig);
	int depth = 0;
	_Unwind_Backtrace(unwind_callback, &depth);
	_exit(1);
}

static struct CrashHandlerInstaller {
	CrashHandlerInstaller() {
		struct sigaction sa{};
		sa.sa_handler = crash_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESETHAND;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGABRT, &sa, nullptr);
	}
} _crash_handler_installer;

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

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeNavigatePOI(JNIEnv *, jclass, jint delta)
{
	_gles_navigate_poi = delta;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeRotateMap(JNIEnv *, jclass, jint delta)
{
	_gles_rotate_map = delta;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_GameActivity_nativeRotateMap(JNIEnv *, jclass, jint delta)
{
	_gles_rotate_map = delta;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_GameActivity_nativeNavigatePOI(JNIEnv *, jclass, jint delta)
{
	_gles_navigate_poi = delta;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_GameActivity_nativeScrollCamera(JNIEnv *, jclass, jint dx, jint dy)
{
	_gles_scroll_dx = dx;
	_gles_scroll_dy = dy;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_GameActivity_nativeSetGamePaused(JNIEnv *, jclass, jboolean paused)
{
	auto *drv = VideoDriver::GetInstance();
	if (drv != nullptr) {
		Debug(driver, 0, "[LOAD] app_pause: paused={}", paused ? "true" : "false");
		drv->SetGameThreadPaused(paused);
	}
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_GameActivity_nativeSetBrightness(JNIEnv *, jclass, jfloat brightness)
{
	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->SetBrightness(brightness);
	}
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeScrollCamera(JNIEnv *, jclass, jint dx, jint dy)
{
	_gles_scroll_dx = dx;
	_gles_scroll_dy = dy;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeSetGamePaused(JNIEnv *, jclass, jboolean paused)
{
	auto *drv = VideoDriver::GetInstance();
	if (drv != nullptr) {
		Debug(driver, 0, "[LOAD] app_pause: paused={}", paused ? "true" : "false");
		drv->SetGameThreadPaused(paused);
	}
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeSetBrightness(JNIEnv *, jclass, jfloat brightness)
{
	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->SetBrightness(brightness);
	}
}

extern "C" JNIEXPORT void JNICALL
Java_org_openttd_android_OpenTTDWallpaperService_nativeSurfaceChanged(JNIEnv *, jclass)
{
	Debug(driver, 0, "[CTX] nativeSurfaceChanged: signalling GL thread");
	_gles_surface_changed = true;
}

#endif /* __ANDROID__ */

static FVideoDriver_SDL_GLES iFVideoDriver_SDL_GLES;

/** Resolve a recorded PaletteID to a 256-byte remap table on the GL thread.
 *  Recolour sprites are stable once loaded (same assumption the reference
 *  made for its raw pointer). Returns nullptr when there is no remap. */
static const uint8_t *ResolveRemap(PaletteID pal)
{
	if (pal == PAL_NONE) return nullptr;
	return GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1;
}

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

	/* Capture EGL state before eglGetError() clears it. */
	EGLContext cur_ctx  = eglGetCurrentContext();
	EGLSurface cur_surf = eglGetCurrentSurface(EGL_DRAW);
	EGLint     egl_err  = eglGetError();
	Debug(driver, 0, "GLES: EGL after CreateContext: display={} surface={} context={} err=0x{:04X}",
		(void *)eglGetCurrentDisplay(), (void *)cur_surf, (void *)cur_ctx, egl_err);

	/* SDL_GL_CreateContext can return a non-null handle even when the internal
	 * eglMakeCurrent fails (observed on gfxstream after surface-lifecycle transitions).
	 * Proceeding with GL initialisation in that state crashes the emulator host. */
	if (cur_ctx == EGL_NO_CONTEXT || cur_surf == EGL_NO_SURFACE || egl_err != EGL_SUCCESS) {
		Debug(driver, 0, "GLES: EGL not ready after CreateContext (ctx={} surf={} err={:#x}) — aborting",
			(void *)cur_ctx, (void *)cur_surf, egl_err);
		SDL_GL_DeleteContext(this->gl_context);
		this->gl_context = nullptr;
		return "EGL not ready after context creation";
	}

	if (!GLESBackend::Create()) return "Failed to initialize GLES backend";

	return std::nullopt;
}

void VideoDriver_SDL_GLES::DestroyContext()
{
	Debug(driver, 0, "[CTX] DestroyContext: egl_ctx={} egl_surf={} gl_context={}",
		(void *)eglGetCurrentContext(), (void *)eglGetCurrentSurface(EGL_DRAW),
		(void *)this->gl_context);
	GLESBackend::Destroy();

	if (this->gl_context != nullptr) {
		SDL_GL_DeleteContext(this->gl_context);
		this->gl_context = nullptr;
	}
	Debug(driver, 0, "[CTX] DestroyContext: done");
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
	Debug(driver, 0, "[CTX] AllocateBackingStore: w={} h={} force={} gl_context={} egl_ctx={}",
		w, h, force, (void *)this->gl_context, (void *)eglGetCurrentContext());
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

	Debug(driver, 0, "[CTX] AllocateBackingStore: done _screen={}x{}", _screen.width, _screen.height);
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
}

void VideoDriver_SDL_GLES::OnGameLoopDone()
{
	if (this->snapshot_buffer == nullptr || _screen.width <= 0 || _screen.height <= 0) return;

	/* Update viewport scroll position before recording so POI jumps
	 * (set from GL thread) take effect in the next snapshot. */
	Window *mw = GetMainWindow();
	if (mw != nullptr && mw->viewport != nullptr) {
		UpdateViewportPosition(mw, this->GetGameInterval().count());
	}
	this->RecordSnapshot();
}

void VideoDriver_SDL_GLES::RecordSnapshot()
{
	[[maybe_unused]] auto t_snap0 = std::chrono::steady_clock::now();

	DrawSnapshot &snap = this->snapshot_buffer->GetWriteBuffer();
	snap.Clear();

	for (int i = 0; i < 256; i++) {
		snap.palette[i] = _cur_palette.palette[i].data;
	}

	int w = _screen.width;
	int h = _screen.height;

	/* Q2.1 pointer-math coords: redirect the screen backing pointer to a stable dummy buffer
	 * and point the snapshot blitter at it, so recorded dst-pointer offsets yield absolute
	 * screen coords (correct even for the zoomed viewport, whose dpi->left/top are virtual).
	 * The snapshot blitter no-ops all pixel writes; RedrawScreenRect only exercises MoveTo/Draw.
	 * Bypass the dirty-block system and redraw the full screen area directly. */
	void *save_dst = _screen.dst_ptr;
	static std::vector<uint8_t> dummy_buf;
	size_t needed = static_cast<size_t>(_screen.pitch) * h * 4;
	if (dummy_buf.size() < needed) dummy_buf.resize(needed);
	_screen.dst_ptr = dummy_buf.data();

	auto *snap_blitter = dynamic_cast<Blitter_Snapshot *>(BlitterFactory::GetActiveBlitter().get());
	if (snap_blitter != nullptr) snap_blitter->SetRecordingBuffer(dummy_buf.data(), _screen.pitch);

	StartRecording(snap);
	[[maybe_unused]] auto t_rec0 = std::chrono::steady_clock::now();
	RedrawScreenRect(0, 0, w, h);
	[[maybe_unused]] auto t_rec1 = std::chrono::steady_clock::now();
	StopRecording();

	/* Restore the real backing pointer (guard against a concurrent AllocateBackingStore). */
	if (_screen.dst_ptr == dummy_buf.data()) _screen.dst_ptr = save_dst;

	/* Validate coordinates before publishing to GPU thread. */
	[[maybe_unused]] auto t_val0 = std::chrono::steady_clock::now();
	int sw = _screen.width;
	int sh = _screen.height;
	int bad = 0;
	for (auto it = snap.commands.begin(); it != snap.commands.end(); ) {
		const auto &c = *it;
		if (c.x < -4096 || c.y < -4096 || c.x > sw + 4096 || c.y > sh + 4096) {
			++bad;
			it = snap.commands.erase(it);
		} else {
			++it;
		}
	}
	[[maybe_unused]] auto t_val1 = std::chrono::steady_clock::now();
	if (bad > 0) {
		Debug(driver, 1, "SNAP_VALIDATE: dropped {} commands with out-of-range coords (screen {}x{})", bad, sw, sh);
	}

	this->snapshot_buffer->Publish();

	GLES_PERF_COUNT({
		auto t_snap1 = std::chrono::steady_clock::now();
		auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
		_gles_perf.snap_record_us += us(t_rec0, t_rec1);
		_gles_perf.snap_validate_us += us(t_val0, t_val1);
		_gles_perf.snap_total_us += us(t_snap0, t_snap1);
		_gles_perf.snap_commands += static_cast<int>(snap.commands.size());
		_gles_perf.gameloop_ticks++;
	});
}

void VideoDriver_SDL_GLES::ProcessOverlayActions()
{
	this->DrainCommandQueue();
	while (this->PollEvent()) {}

	/* Fast path: nothing pending, no lock. Overlay actions arrive rarely (JNI broadcasts). */
	bool any = _gles_jump_waypoint.load() || _gles_navigate_poi.load() != 0 ||
		_gles_rotate_map.load() != 0 || _gles_scroll_dx.load() != 0 || _gles_scroll_dy.load() != 0;
	if (!any) return;

	/* Overlay actions mutate window/viewport state; serialize against the game thread's
	 * window rebuild (SwitchToMode -> LoadWallpaperGame -> ResetWindowSystem runs under
	 * game_state_mutex) by taking the same mutex here on the draw thread. */
	std::lock_guard<std::mutex> lock(this->game_state_mutex);

	if (_gles_jump_waypoint.exchange(false)) {
		Debug(driver, 1, "Tick: jump_waypoint triggered");
		PrepareBackground();
	}
	int poi_delta = _gles_navigate_poi.exchange(0);
	if (poi_delta != 0) { Debug(driver, 1, "Tick: navigate_poi={}", poi_delta); NavigatePOI(poi_delta); }
	int map_delta = _gles_rotate_map.exchange(0);
	if (map_delta != 0) { Debug(driver, 1, "Tick: rotate_map={}", map_delta); RotateTitleMap(map_delta); }
	{
		int scroll_dx = _gles_scroll_dx.exchange(0);
		int scroll_dy = _gles_scroll_dy.exchange(0);
		if (scroll_dx != 0 || scroll_dy != 0) {
			Window *w = GetMainWindow();
			if (w != nullptr && w->viewport != nullptr) {
				w->viewport->dest_scrollpos_x += ScaleByZoom(scroll_dx, w->viewport->zoom);
				w->viewport->dest_scrollpos_y += ScaleByZoom(scroll_dy, w->viewport->zoom);
				w->viewport->follow_vehicle = VehicleID::Invalid();
			}
		}
	}
}

bool VideoDriver_SDL_GLES::SnapshotTick()
{
	/* Pre-Start ticks fall through to the legacy draw path. */
	if (this->snapshot_buffer == nullptr) return false;

	this->ProcessOverlayActions();
	if (this->RecoverContextIfLost()) {
		Debug(driver, 0, "[CTX] SnapshotTick: context recovered, skipping frame");
		return true;
	}
	this->PaintFromSnapshot();

	auto &tb = *this->snapshot_buffer;
	if (tb.swap_count % 60 == 0 && tb.swap_count > 0) {
		Debug(driver, 3, "TRIPLE_BUFFER: swaps={} cpu_ahead={} gpu_ahead={}",
			tb.swap_count, tb.cpu_ahead_count, tb.gpu_ahead_count);
		tb.ResetMetrics();
	}
	return true;
}

bool VideoDriver_SDL_GLES::PaintFromSnapshot()
{
	assert(this->snapshot_buffer != nullptr);

	{
		EGLContext ctx  = eglGetCurrentContext();
		EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
		if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) {
			Debug(driver, 0, "[CTX] PaintFromSnapshot: EGL not current at entry: ctx={} surf={}",
				(void *)ctx, (void *)surf);
			_gles_context_lost = true;
			return false;
		}
	}

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return false;

	[[maybe_unused]] auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };

	/* Try to acquire a new snapshot from triple buffer. */
	bool new_snapshot = this->snapshot_buffer->Acquire();
	DrawSnapshot &snap = this->snapshot_buffer->GetReadBuffer();

	if (new_snapshot && !snap.commands.empty()) {
		/* Process deferred atlas clear before replaying. */
		[[maybe_unused]] auto t0 = std::chrono::steady_clock::now();
		backend->GetSpriteAtlas().ProcessPendingClear();
		[[maybe_unused]] auto t1 = std::chrono::steady_clock::now();
		GLES_PERF_COUNT(_gles_perf.snap_clear_us += us(t0, t1));

		/* Replay draw commands into the GPU queue.
		 * Use read-only Lookup; missing sprites are deferred to after SwapWindow
		 * so that uploads don't add jitter to frame time. */
		GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
		backend->ClearQueue();
		this->deferred_upload_keys.clear();

		[[maybe_unused]] int replayed = 0;
		[[maybe_unused]] int null_entries = 0;
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
			gcmd.remap = ResolveRemap(cmd.palette);
			/* No remap table available: a ColourRemap without a table must be
			 * drawn as a plain sprite, otherwise the shader samples garbage. */
			if (gcmd.remap == nullptr && gcmd.mode == BlitterMode::ColourRemap) {
				gcmd.mode = BlitterMode::Normal;
			}
			gcmd.palette_only = entry->palette_only;

			backend->QueueDraw(gcmd);
			replayed++;
		}
		[[maybe_unused]] auto t3 = std::chrono::steady_clock::now();
		GLES_PERF_COUNT(_gles_perf.snap_replay_us += us(t1, t3));
		GLES_PERF_COUNT(_gles_perf.snap_replayed_cmds += replayed);
		GLES_PERF_COUNT(_gles_perf.snap_null_entries += null_entries);

		/* Full-frame recording ⇒ full clear. */
		backend->AddDirtyRect(0, 0, backend->GetScreenWidth(), backend->GetScreenHeight());

		this->CheckPaletteAnim();

		/* Upload palette and render sprites into FBO. */
		GLESBackend::Get()->UpdatePalette(this->local_palette.palette, 0, 256);
		GLESBackend::Get()->SetPaletteDirty(true);
		this->local_palette.count_dirty = 0;
		[[maybe_unused]] auto t4 = std::chrono::steady_clock::now();
		GLES_PERF_COUNT(_gles_perf.snap_palette_us += us(t3, t4));

		GLES_PERF_COUNT(_gles_perf.gpu_draw_cmds += static_cast<int>(backend->GetDrawQueueSize()));
		backend->PaintFBO();
		[[maybe_unused]] auto t5 = std::chrono::steady_clock::now();
		GLES_PERF_COUNT(_gles_perf.gpu_paint_us += us(t4, t5));
	} else if (!backend->HasFBOContent()) {
		return false;
	}

	[[maybe_unused]] auto t_blit0 = std::chrono::steady_clock::now();
	backend->BlitToScreen();
	[[maybe_unused]] auto t_blit1 = std::chrono::steady_clock::now();
	SDL_GL_SwapWindow(this->sdl_window);
	/* SDL_GL_SwapWindow returns void; check eglGetError() to detect swap failure
	 * (eglSwapBuffers returns EGL_FALSE on dead surface → error != EGL_SUCCESS). */
	EGLint swap_egl_err = eglGetError();
	[[maybe_unused]] auto t_swap1 = std::chrono::steady_clock::now();

	GLES_PERF_COUNT(_gles_perf.blit_to_screen_us += us(t_blit0, t_blit1));
	GLES_PERF_COUNT(_gles_perf.swap_us += us(t_blit1, t_swap1));
	GLES_PERF_COUNT(_gles_perf.frames++);

	/* Detect surface loss: EGL error from swap (dead surface), or context/surface
	 * gone (SDL unbinds on GL thread after onNativeSurfaceDestroyed). */
	{
		EGLContext ctx_after  = eglGetCurrentContext();
		EGLSurface surf_after = eglGetCurrentSurface(EGL_DRAW);
		if (swap_egl_err != EGL_SUCCESS || ctx_after == EGL_NO_CONTEXT || surf_after == EGL_NO_SURFACE) {
			Debug(driver, 0, "[CTX] PaintFromSnapshot: surface lost after swap: egl_err={:#x} ctx={} surf={}",
				swap_egl_err, (void *)ctx_after, (void *)surf_after);
			_gles_context_lost = true;
			this->deferred_upload_keys.clear();
			return false;
		}
	}

	/* Upload missing sprites after SwapWindow (idle time between frames).
	 * They'll appear next frame — 1 frame latency for new sprites only. */
	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	{
		int sprites_before = atlas.GetSpriteCount();
		if (!this->deferred_upload_keys.empty()) {
			atlas.ResetFrameLoadCounter();
			for (GLESSpriteID key : this->deferred_upload_keys) {
				atlas.LookupOrUpload(key);
			}
			this->deferred_upload_keys.clear();
		}

		/* Per-frame sprite load logging: active until stable after atlas clear. */
		if (atlas.IsPostClearMonitoring()) {
			int new_this_frame = atlas.GetSpriteCount() - sprites_before;
			int frame_idx = atlas.TickPostClearFrame(new_this_frame);
			if (frame_idx == -1) {
				Debug(driver, 0, "[LOAD] sprites_stable: total={}", atlas.GetSpriteCount());
			} else if (frame_idx >= 0) {
				Debug(driver, 0, "[LOAD] sprites_frame: since_clear={} total={} new={}",
				      frame_idx, atlas.GetSpriteCount(), new_this_frame);
			}
		}
	}

	/* Run Paint() for PERF logging and context recovery.
	 * It early-returns before GL work in snapshot mode. */
	this->Paint();

	return true;
}

bool VideoDriver_SDL_GLES::RecoverContextIfLost()
{
	/* Check if Java signalled a wallpaper surface change.
	 * SDL doesn't update its internal EGL surface when the wallpaper engine
	 * provides a new ANativeWindow — onNativeSurfaceChanged sets flags but
	 * SDL never calls eglCreateWindowSurface with the new window.
	 * Fix: directly destroy the old EGL surface, get the new ANativeWindow
	 * from Java via SDL's getNativeSurface(), create a new EGL surface, and
	 * make it current.  This bypasses SDL's surface management. */
	if (_gles_surface_changed.exchange(false)) {
		EGLDisplay display = eglGetCurrentDisplay();
		EGLContext ctx      = eglGetCurrentContext();
		EGLSurface old_surf = eglGetCurrentSurface(EGL_DRAW);

		Debug(driver, 0, "[CTX] surface_changed: display={} ctx={} old_surf={}",
			(void *)display, (void *)ctx, (void *)old_surf);

		if (display != EGL_NO_DISPLAY && ctx != EGL_NO_CONTEXT) {
			/* Get the new ANativeWindow from Java's getNativeSurface(). */
			SDL_SysWMinfo wmi;
			SDL_VERSION(&wmi.version);
			ANativeWindow *nw = nullptr;
			if (this->sdl_window != nullptr && SDL_GetWindowWMInfo(this->sdl_window, &wmi)) {
				nw = wmi.info.android.window;
			}
			/* If SDL doesn't give us the window, get it via JNI. */
			if (nw == nullptr) {
				JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
				if (env != nullptr) {
					jclass cls = env->FindClass("org/libsdl/app/SDLActivity");
					if (cls) {
						jmethodID mid = env->GetStaticMethodID(cls, "getNativeSurface", "()Landroid/view/Surface;");
						if (mid) {
							jobject surface = env->CallStaticObjectMethod(cls, mid);
							if (surface != nullptr) {
								nw = ANativeWindow_fromSurface(env, surface);
							}
						}
					}
				}
			}

			if (nw != nullptr) {
				/* Get EGL config from the current context. */
				EGLint config_id;
				EGLConfig config = nullptr;
				eglQueryContext(display, ctx, EGL_CONFIG_ID, &config_id);
				EGLint num_configs;
				EGLint config_attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
				eglChooseConfig(display, config_attribs, &config, 1, &num_configs);

				/* Try to create a new EGL surface for the new ANativeWindow.
				 * Don't destroy the old surface first — it may belong to SDL,
				 * and an ANativeWindow can only have one EGL surface at a time.
				 * If creation fails (EGL_BAD_ALLOC), the window already has
				 * an EGL surface — use SDL_GL_CreateContext to force SDL to
				 * rebind its context to the current window. */
				EGLSurface new_surf = eglCreateWindowSurface(display, config, nw, nullptr);
				EGLint create_err = eglGetError();
				Debug(driver, 0, "[CTX] surface_changed: ANativeWindow={} new_surf={} err={:#x}",
					(void *)nw, (void *)new_surf, create_err);

				if (new_surf != EGL_NO_SURFACE && new_surf != old_surf) {
					/* New surface created — unbind old, destroy, bind new. */
					eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
					if (old_surf != EGL_NO_SURFACE) eglDestroySurface(display, old_surf);
					eglMakeCurrent(display, new_surf, new_surf, ctx);
					Debug(driver, 0, "[CTX] surface_changed: swapped surf {} → {}", (void *)old_surf, (void *)new_surf);
				} else {
					if (new_surf == old_surf) {
						/* Same surface returned — window unchanged. */
						Debug(driver, 0, "[CTX] surface_changed: same surface, just rebinding");
					} else {
						/* eglCreateWindowSurface failed (EGL_BAD_ALLOC) —
						 * SDL owns a surface on this window.  Force SDL to
						 * recreate its GL context which rebinds to the new window. */
						Debug(driver, 0, "[CTX] surface_changed: create failed, recreating SDL context");
						if (this->gl_context != nullptr) {
							SDL_GL_DeleteContext(this->gl_context);
						}
						this->gl_context = SDL_GL_CreateContext(this->sdl_window);
						if (this->gl_context != nullptr) {
							new_surf = eglGetCurrentSurface(EGL_DRAW);
							Debug(driver, 0, "[CTX] surface_changed: SDL context recreated, surf={}",
								(void *)new_surf);
							/* Full GPU state recovery needed after context recreation. */
							_gles_context_lost = true;
						} else {
							Debug(driver, 0, "[CTX] surface_changed: SDL_GL_CreateContext FAILED: {}", SDL_GetError());
						}
					}
				}

				/* Force FBO rebind for the new default framebuffer. */
				if (!_gles_context_lost && GLESBackend::Get() != nullptr) {
					GLESBackend *b = GLESBackend::Get();
					b->Resize(b->GetScreenWidth(), b->GetScreenHeight());
				}
				this->MakeDirty(0, 0, _screen.width, _screen.height);

				ANativeWindow_release(nw);
			} else {
				Debug(driver, 0, "[CTX] surface_changed: could not get ANativeWindow");
			}
		}
	}

	void *current_ctx  = eglGetCurrentContext();
	void *current_surf = eglGetCurrentSurface(EGL_DRAW);

	/* Mark context lost if EGL context is gone (surface destroyed). */
	if (current_ctx == EGL_NO_CONTEXT) {
		Debug(driver, 0, "[CTX] RecoverContextIfLost: ctx=NO_CONTEXT surf={} last={} → marking lost",
			(void *)current_surf, this->last_egl_context);
		_gles_context_lost = true;
		this->last_egl_context = EGL_NO_CONTEXT;
		return true;
	}

	/* Detect context recreation: old context was lost and a new one appeared
	 * without the GL thread ever seeing EGL_NO_CONTEXT (race between context
	 * destruction and creation, e.g. preview → live wallpaper transition). */
	bool ctx_actually_changed = (this->last_egl_context != nullptr &&
	                             this->last_egl_context != EGL_NO_CONTEXT &&
	                             current_ctx != this->last_egl_context);
	if (ctx_actually_changed) {
		Debug(driver, 0, "[CTX] RecoverContextIfLost: ctx changed {} → {}, surf={}, treating as loss",
			this->last_egl_context, current_ctx, (void *)current_surf);
		_gles_context_lost = true;
	}
	this->last_egl_context = current_ctx;

	/* Detect EGL surface swap: different surface from last tick.
	 * If ONLY the surface changed (same context), SDL briefly unbinds the
	 * context during the swap which may have set _gles_context_lost — clear
	 * it and just Resize the FBO.
	 * If BOTH context AND surface changed (SDL_GL_CreateContext recreated
	 * everything), keep _gles_context_lost so full RecoverGPUState runs. */
	if (this->last_egl_surface != nullptr && current_surf != EGL_NO_SURFACE &&
	    current_surf != this->last_egl_surface && GLESBackend::Get() != nullptr) {
		Debug(driver, 0, "[CTX] RecoverContextIfLost: EGL surface swapped {} → {}, ctx_changed={}",
			this->last_egl_surface, (void *)current_surf, ctx_actually_changed ? "yes" : "no");
		if (!ctx_actually_changed) {
			/* Surface-only swap: rebind FBO, clear false context-lost flag. */
			GLESBackend *b = GLESBackend::Get();
			b->Resize(b->GetScreenWidth(), b->GetScreenHeight());
			_gles_context_lost = false;
		}
		/* If ctx_actually_changed, leave _gles_context_lost=true for full recovery below. */
	}
	this->last_egl_surface = current_surf;

	/* Recover from GL context loss (SDL_RENDER_DEVICE_RESET). */
	if (_gles_context_lost && GLESBackend::Get() != nullptr) {
		_gles_context_lost = false;
		Debug(driver, 0, "[CTX] RecoverContextIfLost: starting recovery: ctx={} surf={} screen={}x{} game_mode={}",
			current_ctx, (void *)current_surf, _screen.width, _screen.height, static_cast<int>(_game_mode));
		GLESBackend::Get()->RecoverGPUState();
		CopyPalette(this->local_palette, true);
		_switch_mode = (_game_mode == GameMode::Wallpaper) ? SwitchMode::Wallpaper : SwitchMode::Menu;
		this->MakeDirty(0, 0, _screen.width, _screen.height);
		Debug(driver, 0, "[CTX] RecoverContextIfLost: recovery done, switch_mode={}", static_cast<int>(_switch_mode));
		return true;
	}

	return false;
}

void VideoDriver_SDL_GLES::Paint()
{
	PerformanceMeasurer framerate(PerformanceElement::Video);

	GLES_PERF_COUNT({
		static int fps_frames = 0;
		static auto fps_last = std::chrono::steady_clock::now();

		/* Frame time ring buffer for jank detection and statistics (persists across 500ms windows). */
		static int64_t ft_ring[128] = {};
		static int ft_idx = 0;
		static int ft_count = 0;

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
			Debug(driver, 3, "PERF fps={} frames={} | blit_calls={} gpu_cmds={} atlas_miss={} offscreen_skip={} | gl_batches={} reuploaded={} dim_mismatch={} zoom=[{}/{}/{}/{}/{}/{}] scaled_hits={} scaled_fallback={} gpu_paint={}us egl_swap={}us | encode: total={} uploaded={} all_transparent={} | atlas: color_pages={} remap_pages={} gpu_entries={} registered={} new_sprites={} repacked={} | gl: loads={} fails={} budget_skip={} pbo_up={} lookup={}/{}",
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
			Debug(driver, 3, "  VP landscape={}us vehicles={}us ground_sprites={}us sprite_sort={}us sprite_draw={}us update_windows={}us | tiles_iterated={} parent_sprites={} child_sprites={} sprites_generated={} vp_draw_calls={} viewport={}x{} | mrt: full_renders={} resolve_only={} idle_blit={} resolve={}us",
				p.vp_land_us / n, p.vp_vehicles_us / n, p.vp_signs_tiles_us / n,
				p.vp_sort_us / n, p.vp_draw_us / n, p.update_windows_us / n,
				p.vp_tiles_iterated / n, p.vp_parent_sprites / n, p.vp_child_sprites / n,
				p.vp_sprites_generated / n,
				p.vp_calls, p.vp_area_w, p.vp_area_h,
				p.full_renders, p.resolve_passes, p.idle_blits, p.resolve_us / n);
			auto tick_accounted = p.lock_video_us + p.mutex_wait_us + p.input_poll_us + p.update_windows_us + p.populate_us + p.check_palette_us + p.paint_full_us + p.unlock_video_us;
			Debug(driver, 3, "  TICK total={}us | lock_video={}us game_mutex={}us(skipped={}) input_poll={}us update_windows={}us populate_sprites={}us check_palette={}us paint={}us(gpu_render={}us egl_swap={}us) unlock_video={}us unaccounted={}us",
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
					Debug(driver, 3, "  CPU gameloop={}us snap_total={}us(record={}us validate={}us) cmds={} | tileloop={}us({}tiles) vehtick={}us ticks={}",
						p.gameloop_us / gt,
						p.snap_total_us / gt, p.snap_record_us / gt, p.snap_validate_us / gt,
						p.snap_commands / gt,
						p.tileloop_us / gt, p.tileloop_count,
						p.vehicletick_us / gt, p.gameloop_ticks);
				}
				Debug(driver, 3, "  EXTRA gpu_actual={}us | overdraw={:.1f}x batch_eff={:.1f} cache_hit={:.1f}% | atlas_occ: color={}% remap={}% | jank={} stddev={}us p95={}us p99={}us | vehicles: T={} R={} S={} A={}",
					p.gpu_time_us,
					overdraw, batch_eff, cache_hit,
					atlas.GetColourOccupancyPercent(), atlas.GetRemapOccupancyPercent(),
					p.jank_count, ft_stddev, ft_p95, ft_p99,
					p.vehicle_trains, p.vehicle_road, p.vehicle_ships, p.vehicle_aircraft);
				Debug(driver, 3, "  GPU_SNAP clear={}us pbo={}us replay={}us(cmds={} null={}) palette={}us paint={}us blit={}us swap={}us",
					p.snap_clear_us / n, p.snap_pbo_us / n, p.snap_replay_us / n,
					p.snap_replayed_cmds / n, p.snap_null_entries / n,
					p.snap_palette_us / n, p.gpu_paint_us / n,
					p.blit_to_screen_us / n, p.swap_us / n);
			}

			p = {};  /* Reset counters. */
			fps_frames = 0;
			fps_last = fps_now;
		}
	});
}
