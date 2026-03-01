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
#include "../blitter/factory.hpp"
#include "../debug.h"
#include "../framerate_type.h"
#include "sdl2_gles_v.h"
#include "gles_backend.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include "../safeguards.h"

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

	/* Log EGL state after context creation. */
	EGLDisplay egl_dpy = eglGetCurrentDisplay();
	EGLSurface egl_surf = eglGetCurrentSurface(EGL_DRAW);
	EGLContext egl_ctx = eglGetCurrentContext();
	Debug(driver, 0, "GLES: EGL after CreateContext: display={} surface={} context={} err=0x{:04X}",
		(void *)egl_dpy, (void *)egl_surf, (void *)egl_ctx, eglGetError());

	/* Log the SDL window WM info. */
	SDL_SysWMinfo wminfo;
	SDL_VERSION(&wminfo.version);
	if (SDL_GetWindowWMInfo(this->sdl_window, &wminfo)) {
		Debug(driver, 0, "GLES: SDL WM subsystem={}", (int)wminfo.subsystem);
#ifdef SDL_VIDEO_DRIVER_ANDROID
		Debug(driver, 0, "GLES: ANativeWindow={} EGLSurface={}",
			(void *)wminfo.info.android.window, (void *)wminfo.info.android.surface);
#endif
	}

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

void VideoDriver_SDL_GLES::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

	static int paint_count = 0;
	static int fps_frames = 0;
	static auto fps_last = std::chrono::steady_clock::now();
	fps_frames++;
	auto fps_now = std::chrono::steady_clock::now();
	if (fps_now - fps_last >= std::chrono::seconds(1)) {
		Debug(driver, 0, "FPS: {}", fps_frames);
		fps_frames = 0;
		fps_last = fps_now;
	}
	if (paint_count < 10) {
		Debug(driver, 0, "GLES Paint #{}: backend={} cpu_tex={} fbo={} screen={}x{}",
			paint_count, (void *)GLESBackend::Get(),
			GLESBackend::Get() ? GLESBackend::Get()->GetScreenWidth() : -1,
			GLESBackend::Get() ? GLESBackend::Get()->GetScreenHeight() : -1,
			_screen.width, _screen.height);
	}
	paint_count++;

	if (this->local_palette.count_dirty != 0) {
		GLESBackend::Get()->UpdatePalette(this->local_palette.palette,
			this->local_palette.first_dirty, this->local_palette.count_dirty);
		this->local_palette.count_dirty = 0;
	}

	/* Upload CPU-rendered content as background texture.
	 * When GPU sprites are enabled, this still uploads so UI (text, windows)
	 * renders correctly. GPU sprites draw on top of this layer. */
	GLESBackend::Get()->UploadVideoBuffer(this->video_buffer.data(),
		_screen.width, _screen.height);

	GLESBackend::Get()->Paint();

	SDL_GL_SwapWindow(this->sdl_window);
}
