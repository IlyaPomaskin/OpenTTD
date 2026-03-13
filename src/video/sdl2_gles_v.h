/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sdl2_gles_v.h OpenGL ES backend of the SDL2 video driver. */

#ifndef VIDEO_SDL2_GLES_V_H
#define VIDEO_SDL2_GLES_V_H

#include "sdl2_v.h"
#include "../zoom_type.h"
#include <chrono>

/** The OpenGL ES video driver for Android. */
class VideoDriver_SDL_GLES : public VideoDriver_SDL_Base {
public:
	VideoDriver_SDL_GLES() : VideoDriver_SDL_Base(true) {}

	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;

	bool HasEfficient8Bpp() const override { return false; }
	bool UseSystemCursor() override { return false; }

	void ToggleVsync(bool vsync) override;

	std::string_view GetName() const override { return "sdl-gles"; }

	void MakeDirty(int left, int top, int width, int height) override;

protected:
	bool AllocateBackingStore(int w, int h, bool force = false) override;
	void *GetVideoPointer() override;
	void ReleaseVideoPointer() override {}
	void Paint() override;
	bool PaintFromSnapshot() override;
	void CheckPaletteAnim() override;
	bool CreateMainWindow(uint w, uint h, uint flags) override;

private:
	void *gl_context = nullptr;
	std::vector<uint32_t> video_buffer; ///< CPU-side video buffer for non-GPU drawing (text, UI).
	std::vector<Rect> gles_dirty_rects; ///< Individual dirty rects for GLES (not coalesced).

	/* Camera interpolation state (GPU blit thread).
	 * Interpolate between prev and curr scrollpos over one tick period.
	 * At t=0 we show prev (= end of previous interval), at t=1 we show curr. */
	int snap_prev_scrollpos_x = 0;
	int snap_prev_scrollpos_y = 0;
	int snap_curr_scrollpos_x = 0;
	int snap_curr_scrollpos_y = 0;
	ZoomLevel snap_scroll_zoom{};
	std::chrono::steady_clock::time_point snap_time{};

	std::optional<std::string_view> AllocateContext();
	void DestroyContext();
};

/** The factory for SDL's OpenGL ES video driver. */
class FVideoDriver_SDL_GLES : public DriverFactoryBase {
public:
	FVideoDriver_SDL_GLES() : DriverFactoryBase(Driver::Type::Video, 10, "sdl-gles", "SDL OpenGL ES Video Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<VideoDriver_SDL_GLES>(); }

protected:
	bool UsesHardwareAcceleration() const override { return true; }
};

#endif /* VIDEO_SDL2_GLES_V_H */
