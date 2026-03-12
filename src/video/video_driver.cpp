/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file video_driver.cpp Common code between video driver implementations. */

#include "../stdafx.h"
#include "../core/random_func.hpp"
#include "../network/network.h"
#include "../blitter/factory.hpp"
#include "../blitter/snapshot.hpp"
#include "../debug.h"
#include "../driver.h"
#include "../fontcache.h"
#include "../gfx_func.h"
#include "../gfxinit.h"
#include "../progress.h"
#include "../rev.h"
#include "../thread.h"
#include "../window_func.h"
#include "../window_gui.h"
#include "../viewport_func.h"
#include "../openttd.h"
#include "video_driver.hpp"
#include "draw_snapshot.h"
#include "gles_backend.h"
#include "../palette_func.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#endif

#ifdef __ANDROID__
static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context *context, void *arg) {
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

static void crash_handler(int sig) {
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
#endif

#include "../safeguards.h"

bool _video_hw_accel; ///< Whether to consider hardware accelerated video drivers on startup.
bool _video_vsync; ///< Whether we should use vsync (only if active video driver supports HW acceleration).

void VideoDriver::GameLoop()
{
	this->next_game_tick += this->GetGameInterval();

	/* Avoid next_game_tick getting behind more and more if it cannot keep up. */
	auto now = std::chrono::steady_clock::now();
	if (this->next_game_tick < now - ALLOWED_DRIFT * this->GetGameInterval()) this->next_game_tick = now;

	{
		std::lock_guard<std::mutex> lock(this->game_state_mutex);

		auto t_gl0 = std::chrono::steady_clock::now();
		::GameLoop();
		auto t_gl1 = std::chrono::steady_clock::now();

		if (this->snapshot_buffer != nullptr && _screen.width > 0 && _screen.height > 0) {
			this->RecordSnapshot(t_gl0, t_gl1);
		}
	}
}

void VideoDriver::RecordSnapshot(std::chrono::steady_clock::time_point t_gl0, std::chrono::steady_clock::time_point t_gl1)
{
	auto t_snap0 = std::chrono::steady_clock::now();

	DrawSnapshot &snap = this->snapshot_buffer->GetWriteBuffer();
	snap.Clear();

	for (int i = 0; i < 256; i++) {
		snap.palette[i] = _cur_palette.palette[i].data;
	}

	void *save_dst = _screen.dst_ptr;
	DrawPixelInfo *save_dpi = _cur_dpi;

	static std::vector<uint8_t> dummy_buf;
	size_t needed = (size_t)_screen.width * _screen.height * 4;
	if (dummy_buf.size() < needed) dummy_buf.resize(needed);

	_screen.dst_ptr = dummy_buf.data();
	_cur_dpi = &_screen;

	/* Set recording buffer on the snapshot blitter (already active). */
	auto *snap_blitter = dynamic_cast<Blitter_Snapshot *>(BlitterFactory::GetActiveBlitter().get());
	if (snap_blitter != nullptr) snap_blitter->SetRecordingBuffer(dummy_buf.data(), _screen.pitch);
	StartRecording(snap);
	MarkWholeScreenDirty();
	auto t_rec0 = std::chrono::steady_clock::now();
	DrawDirtyBlocks();
	auto t_rec1 = std::chrono::steady_clock::now();
	StopRecording();

	/* Restore only what we changed. Do NOT restore width/height/pitch —
	 * a concurrent resize on the draw thread must not be overwritten. */
	_screen.dst_ptr = save_dst;
	_cur_dpi = save_dpi;
	snap.full_redraw = true;

	/* Validate coordinates before publishing to GPU thread. */
	auto t_val0 = std::chrono::steady_clock::now();
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
	auto t_val1 = std::chrono::steady_clock::now();
	if (bad > 0) {
		Debug(driver, 0, "SNAP_VALIDATE: dropped {} commands with out-of-range coords (screen {}x{})", bad, sw, sh);
	}

	this->snapshot_buffer->Publish();
	auto t_snap1 = std::chrono::steady_clock::now();

	auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
	_gles_perf.gameloop_us += us(t_gl0, t_gl1);
	_gles_perf.snap_record_us += us(t_rec0, t_rec1);
	_gles_perf.snap_validate_us += us(t_val0, t_val1);
	_gles_perf.snap_total_us += us(t_snap0, t_snap1);
	_gles_perf.snap_commands += static_cast<int>(snap.commands.size());
	_gles_perf.gameloop_ticks++;
}

void VideoDriver::GameThread()
{
	while (!_exit_game) {
		this->GameLoop();

		auto now = std::chrono::steady_clock::now();
		if (this->next_game_tick > now) {
			std::this_thread::sleep_for(this->next_game_tick - now);
		} else {
			/* Ensure we yield this thread if drawings wants to take a lock on
			 * the game state. This is mainly because most OSes have an
			 * optimization that if you unlock/lock a mutex in the same thread
			 * quickly, it will never context switch even if there is another
			 * thread waiting to take the lock on the same mutex. */
			std::lock_guard<std::mutex> lock(this->game_thread_wait_mutex);
		}
	}
}

/**
 * Pause the game-loop for a bit, releasing the game-state lock. This allows,
 * if the draw-tick requested this, the drawing to happen.
 */
void VideoDriver::GameLoopPause()
{
	/* If we are not called from the game-thread, ignore this request. */
	if (std::this_thread::get_id() != this->game_thread.get_id()) return;

	this->game_state_mutex.unlock();

	{
		/* See GameThread() for more details on this lock. */
		std::lock_guard<std::mutex> lock(this->game_thread_wait_mutex);
	}

	this->game_state_mutex.lock();
}

/* static */ void VideoDriver::GameThreadThunk(VideoDriver *drv)
{
	drv->GameThread();
}

void VideoDriver::StartGameThread()
{
	if (this->is_game_threaded) {
		this->is_game_threaded = StartNewThread(&this->game_thread, "ottd:game", &VideoDriver::GameThreadThunk, this);
	}

	Debug(driver, 1, "using {}thread for game-loop", this->is_game_threaded ? "" : "no ");
}

void VideoDriver::StopGameThread()
{
	if (!this->is_game_threaded) return;

	this->game_thread.join();
}

void VideoDriver::Tick()
{
	if (!this->is_game_threaded && std::chrono::steady_clock::now() >= this->next_game_tick) {
		this->GameLoop();

		/* For things like dedicated server, don't run a separate draw-tick. */
		if (!this->HasGUI()) {
			::InputLoop();
			::UpdateWindows();
			this->next_draw_tick = this->next_game_tick;
		}
	}

	auto now = std::chrono::steady_clock::now();
	if (this->HasGUI() && now >= this->next_draw_tick) {
		this->next_draw_tick += this->GetDrawInterval();
		/* Avoid next_draw_tick getting behind more and more if it cannot keep up. */
		if (this->next_draw_tick < now - ALLOWED_DRIFT * this->GetDrawInterval()) this->next_draw_tick = now;

		auto t_tick0 = std::chrono::steady_clock::now();

		/* Snapshot path: paint from triple buffer, skip mutex wait.
		 * Do NOT call LockVideoBuffer here — it overwrites _screen.dst_ptr,
		 * which races with the game thread's snapshot recording. */
		if (this->snapshot_buffer != nullptr) {
			/* Process SDL events to keep surface/EGL state in sync. */
			this->DrainCommandQueue();
			while (this->PollEvent()) {}

			this->PaintFromSnapshot();

			/* Log triple buffer metrics periodically. */
			auto &tb = *this->snapshot_buffer;
			if (tb.swap_count % 60 == 0 && tb.swap_count > 0) {
				Debug(driver, 0, "TRIPLE_BUFFER: swaps={} cpu_ahead={} gpu_ahead={}",
					tb.swap_count, tb.cpu_ahead_count, tb.gpu_ahead_count);
				tb.ResetMetrics();
			}

			static bool first_draw_tick = true;
			if (first_draw_tick) {
				first_draw_tick = false;
				DriverFactoryBase::MarkVideoDriverOperational();
			}
			return;
		}

		/* Locking video buffer can block (especially with vsync enabled), do it before taking game state lock. */
		this->LockVideoBuffer();

		auto t_lock_video = std::chrono::steady_clock::now();

		/* Block until the game thread releases the lock.
		 * Every frame gets UpdateWindows for smooth vehicle movement. */
		{
			std::lock_guard<std::mutex> lock_wait(this->game_thread_wait_mutex);
			std::lock_guard<std::mutex> lock_state(this->game_state_mutex);

			auto t_mutex = std::chrono::steady_clock::now();

			/* Process deferred atlas clear before UpdateWindows so that
			 * LookupOrUpload never returns stale entries from the old map. */
			if (GLESBackend::Get() != nullptr) {
				GLESBackend::Get()->GetSpriteAtlas().ProcessPendingClear();
			}

			InteractiveRandom();
			this->DrainCommandQueue();
			while (this->PollEvent()) {}
			this->InputLoop();
			::InputLoop();

			auto t_input = std::chrono::steady_clock::now();

			if (_switch_mode == SM_NONE || HasModalProgress()) {
				::UpdateWindows();
			}

			auto t_updwin = std::chrono::steady_clock::now();

			this->PopulateSystemSprites();

			auto t_populate = std::chrono::steady_clock::now();

			auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
			_gles_perf.mutex_wait_us += us(t_lock_video, t_mutex);
			_gles_perf.input_poll_us += us(t_mutex, t_input);
			_gles_perf.populate_us += us(t_updwin, t_populate);
		}

		auto t_pre_palette = std::chrono::steady_clock::now();
		this->CheckPaletteAnim();
		auto t_post_palette = std::chrono::steady_clock::now();
		this->Paint();

		auto t_post_paint = std::chrono::steady_clock::now();

		this->UnlockVideoBuffer();
		auto t_tick_end = std::chrono::steady_clock::now();

		auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
		_gles_perf.lock_video_us += us(t_tick0, t_lock_video);
		_gles_perf.check_palette_us += us(t_pre_palette, t_post_palette);
		_gles_perf.paint_full_us += us(t_post_palette, t_post_paint);
		_gles_perf.unlock_video_us += us(t_post_paint, t_tick_end);
		_gles_perf.tick_total_us += us(t_tick0, t_tick_end);
		_gles_perf.last_frame_us = us(t_tick0, t_tick_end);

		/* Log individual slow frames for stutter diagnosis. */
		if (_gles_perf.last_frame_us > 20000) {
			Debug(driver, 0, "  SLOW frame={}us | lock_video={}us mutex+updwin={}us palette={}us paint={}us unlock={}us",
				_gles_perf.last_frame_us,
				us(t_tick0, t_lock_video),
				us(t_lock_video, t_pre_palette),
				us(t_pre_palette, t_post_palette),
				us(t_post_palette, t_post_paint),
				us(t_post_paint, t_tick_end));
		}

		/* Wait till the first successful drawing tick before marking the driver as operational. */
		static bool first_draw_tick = true;
		if (first_draw_tick) {
			first_draw_tick = false;
			DriverFactoryBase::MarkVideoDriverOperational();
		}
	}
}

void VideoDriver::SleepTillNextTick()
{
	auto next_tick = this->next_draw_tick;
	auto now = std::chrono::steady_clock::now();

	if (!this->is_game_threaded) {
		next_tick = min(next_tick, this->next_game_tick);
	}

	if (next_tick > now) {
		std::this_thread::sleep_for(next_tick - now);
	}
}

/**
 * Get the caption to use for the game's title bar.
 * @return The caption.
 */
/* static */ std::string VideoDriver::GetCaption()
{
	return fmt::format("OpenTTD {}", _openttd_revision);
}
