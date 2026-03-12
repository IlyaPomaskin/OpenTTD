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
#include "../debug.h"
#include "../driver.h"
#include "../fontcache.h"
#include "../gfx_func.h"
#include "../gfxinit.h"
#include "../progress.h"
#include "../rev.h"
#include "../thread.h"
#include "../window_func.h"
#include "../openttd.h"
#include "video_driver.hpp"
#include "draw_snapshot.h"
#include "gles_backend.h"
#include "../palette_func.h"

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

		::GameLoop();
	}
}

void VideoDriver::GameThread()
{
	while (!_exit_game) {
		this->GameLoop();

		/* Snapshot path: record draw commands after game state update. */
		if (this->snapshot_buffer != nullptr) {
			DrawSnapshot &snap = this->snapshot_buffer->GetWriteBuffer();
			snap.Clear();

			/* Copy palette. */
			for (int i = 0; i < 256; i++) {
				snap.palette[i] = _cur_palette.palette[i].data;
			}

			/* Record draw commands via GfxBlitter intercept. */
			StartRecording(snap);
			::UpdateWindows();
			StopRecording();

			snap.full_redraw = true;
			this->snapshot_buffer->Publish();
		}

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

		/* Locking video buffer can block (especially with vsync enabled), do it before taking game state lock. */
		this->LockVideoBuffer();

		/* Snapshot path: paint from triple buffer, skip mutex wait. */
		if (this->snapshot_buffer != nullptr) {
			this->PaintFromSnapshot();

			/* Log triple buffer metrics periodically. */
			auto &tb = *this->snapshot_buffer;
			if (tb.swap_count % 60 == 0 && tb.swap_count > 0) {
				Debug(driver, 0, "TRIPLE_BUFFER: swaps={} cpu_ahead={} gpu_ahead={}",
					tb.swap_count, tb.cpu_ahead_count, tb.gpu_ahead_count);
				tb.ResetMetrics();
			}

			this->UnlockVideoBuffer();

			static bool first_draw_tick = true;
			if (first_draw_tick) {
				first_draw_tick = false;
				DriverFactoryBase::MarkVideoDriverOperational();
			}
			return;
		}

		auto t_lock_video = std::chrono::steady_clock::now();

		/* Block until the game thread releases the lock.
		 * Every frame gets UpdateWindows for smooth vehicle movement. */
		{
			std::lock_guard<std::mutex> lock_wait(this->game_thread_wait_mutex);
			std::lock_guard<std::mutex> lock_state(this->game_state_mutex);

			auto t_mutex = std::chrono::steady_clock::now();

			/* Process deferred atlas clear before UpdateWindows so that
			 * LookupOrUpload never returns stale entries from the old map. */
			if (_gles_gpu_sprites && GLESBackend::Get() != nullptr) {
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
