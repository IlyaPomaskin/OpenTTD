/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_backend.cpp OpenGL ES backend for GPU-accelerated sprite rendering. */

#include "../stdafx.h"
#include "gles_backend.h"
#include "../debug.h"
#include "../gfx_func.h"
#include "../palette_func.h"
#include "../table/gles_shader.h"
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

#include "../safeguards.h"

/* GL_EXT_disjoint_timer_query function pointers. */
using PFNGLGENQUERIESEXTPROC = void (*)(GLsizei n, GLuint *ids);
using PFNGLDELETEQUERIESEXTPROC = void (*)(GLsizei n, const GLuint *ids);
using PFNGLBEGINQUERYEXTPROC = void (*)(GLenum target, GLuint id);
using PFNGLENDQUERYEXTPROC = void (*)(GLenum target);
using PFNGLGETQUERYOBJECTUI64VEXTPROC = void (*)(GLuint id, GLenum pname, GLuint64 *params);
using PFNGLGETQUERYOBJECTIVEXTPROC = void (*)(GLuint id, GLenum pname, GLint *params);

static PFNGLGENQUERIESEXTPROC _glGenQueriesEXT = nullptr;
static PFNGLDELETEQUERIESEXTPROC _glDeleteQueriesEXT = nullptr;
static PFNGLBEGINQUERYEXTPROC _glBeginQueryEXT = nullptr;
static PFNGLENDQUERYEXTPROC _glEndQueryEXT = nullptr;
static PFNGLGETQUERYOBJECTUI64VEXTPROC _glGetQueryObjectui64vEXT = nullptr;
static PFNGLGETQUERYOBJECTIVEXTPROC _glGetQueryObjectivEXT = nullptr;

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

GLESBackend *GLESBackend::instance = nullptr;

/** Maximum number of sprites per batch (64K vertices / 6 vertices per sprite). */
static const size_t MAX_BATCH_SPRITES = 10922;
static const size_t MAX_BATCH_VERTICES = MAX_BATCH_SPRITES * 6;

GLESBackend::GLESBackend() {}

GLESBackend::~GLESBackend()
{
	if (this->prog_normal != 0) glDeleteProgram(this->prog_normal);
	if (this->prog_remap != 0) glDeleteProgram(this->prog_remap);
	if (this->prog_transparent != 0) glDeleteProgram(this->prog_transparent);
	if (this->prog_palette != 0) glDeleteProgram(this->prog_palette);
	if (this->prog_solid != 0) glDeleteProgram(this->prog_solid);
	if (this->prog_resolve != 0) glDeleteProgram(this->prog_resolve);
	if (this->palette_tex != 0) glDeleteTextures(1, &this->palette_tex);
	glDeleteTextures(2, this->remap_table_tex);
	if (this->fbo_tex != 0) glDeleteTextures(1, &this->fbo_tex);
	if (this->fbo_idx_tex != 0) glDeleteTextures(1, &this->fbo_idx_tex);
	if (this->fbo != 0) glDeleteFramebuffers(1, &this->fbo);
	if (this->vbo != 0) glDeleteBuffers(1, &this->vbo);
	if (this->has_timer_query && _glDeleteQueriesEXT) _glDeleteQueriesEXT(2, this->gpu_query);
	this->sprite_atlas.Destroy();
}

GLuint GLESBackend::CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		GLchar log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		Debug(driver, 0, "GLES: Shader compile error: {}", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint GLESBackend::LinkProgram(GLuint vert, GLuint frag)
{
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);

	/* Force consistent attribute locations across all programs.
	 * This allows Paint() to set up vertex attribs once instead of per-batch. */
	glBindAttribLocation(prog, 0, "a_position");
	glBindAttribLocation(prog, 1, "a_colour_uv");
	glBindAttribLocation(prog, 2, "a_remap_uv");
	glBindAttribLocation(prog, 3, "a_cpage");
	glBindAttribLocation(prog, 4, "a_rpage");

	glLinkProgram(prog);

	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLchar log[512];
		glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
		Debug(driver, 0, "GLES: Program link error: {}", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

bool GLESBackend::InitShaders()
{
	/* Compile vertex shader (shared by all programs). */
	GLuint vs = CompileShader(GL_VERTEX_SHADER, _gles_vertex_shader);
	if (vs == 0) return false;

	/* Normal fragment shader. */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_normal);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_normal = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_normal == 0) { glDeleteShader(vs); return false; }

		this->normal_screen_loc = glGetUniformLocation(this->prog_normal, "screen");
		this->normal_colour_tex_loc = glGetUniformLocation(this->prog_normal, "colour_tex");
		this->normal_colour_tex1_loc = glGetUniformLocation(this->prog_normal, "colour_tex1");
	}

	/* Remap fragment shader. */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_remap);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_remap = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_remap == 0) { glDeleteShader(vs); return false; }

		this->remap_screen_loc = glGetUniformLocation(this->prog_remap, "screen");
		this->remap_colour_tex_loc = glGetUniformLocation(this->prog_remap, "colour_tex");
		this->remap_colour_tex1_loc = glGetUniformLocation(this->prog_remap, "colour_tex1");
		this->remap_remap_tex_loc = glGetUniformLocation(this->prog_remap, "remap_tex");
		this->remap_remap_tex1_loc = glGetUniformLocation(this->prog_remap, "remap_tex1");
		this->remap_palette_tex_loc = glGetUniformLocation(this->prog_remap, "palette_tex");
		this->remap_table_tex_loc = glGetUniformLocation(this->prog_remap, "remap_table_tex");
	}

	/* Transparent fragment shader. */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_transparent);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_transparent = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_transparent == 0) { glDeleteShader(vs); return false; }

		this->trans_screen_loc = glGetUniformLocation(this->prog_transparent, "screen");
		this->trans_colour_tex_loc = glGetUniformLocation(this->prog_transparent, "colour_tex");
		this->trans_colour_tex1_loc = glGetUniformLocation(this->prog_transparent, "colour_tex1");
	}

	/* Palette-only fragment shader (M channel → palette lookup). */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_palette);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_palette = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_palette == 0) { glDeleteShader(vs); return false; }

		this->pal_screen_loc = glGetUniformLocation(this->prog_palette, "screen");
		this->pal_remap_tex_loc = glGetUniformLocation(this->prog_palette, "remap_tex");
		this->pal_remap_tex1_loc = glGetUniformLocation(this->prog_palette, "remap_tex1");
		this->pal_palette_tex_loc = glGetUniformLocation(this->prog_palette, "palette_tex");
	}

	/* Solid debug fragment shader. */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_solid);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_solid = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_solid == 0) { glDeleteShader(vs); return false; }

		this->solid_screen_loc = glGetUniformLocation(this->prog_solid, "screen");
		this->solid_colour_loc = glGetUniformLocation(this->prog_solid, "u_colour");
	}

	/* Palette resolve fragment shader (index attachment -> palette lookup). */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_resolve);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_resolve = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_resolve == 0) { glDeleteShader(vs); return false; }

		this->resolve_screen_loc = glGetUniformLocation(this->prog_resolve, "screen");
		this->resolve_idx_tex_loc = glGetUniformLocation(this->prog_resolve, "idx_tex");
		this->resolve_palette_tex_loc = glGetUniformLocation(this->prog_resolve, "palette_tex");
	}

	glDeleteShader(vs);

	Debug(driver, 1, "GLES: All shaders compiled and linked successfully");
	return true;
}

bool GLESBackend::Create()
{
	assert(GLESBackend::instance == nullptr);

	GLESBackend *backend = new GLESBackend();

	if (!backend->InitShaders()) {
		delete backend;
		return false;
	}

	/* Create 256x1 RGBA palette texture. */
	glGenTextures(1, &backend->palette_tex);
	glBindTexture(GL_TEXTURE_2D, backend->palette_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	/* Create double-buffered 256x1 remap table textures (R8). */
	glGenTextures(2, backend->remap_table_tex);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, backend->remap_table_tex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
	}

	/* Create VBO for batched vertices. */
	glGenBuffers(1, &backend->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, backend->vbo);
	glBufferData(GL_ARRAY_BUFFER, MAX_BATCH_VERTICES * sizeof(GLESVertex), nullptr, GL_DYNAMIC_DRAW);

	backend->sprite_atlas.Init();

	/* Reserve capacity for draw queue and vertex buffer. */
	backend->draw_queue.reserve(4096);
	backend->vertex_buf.reserve(MAX_BATCH_VERTICES);

	/* Probe GL_EXT_disjoint_timer_query for GPU timing. */
	const char *exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
	if (exts != nullptr && std::strstr(exts, "GL_EXT_disjoint_timer_query") != nullptr) {
		_glGenQueriesEXT = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(eglGetProcAddress("glGenQueriesEXT"));
		_glDeleteQueriesEXT = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(eglGetProcAddress("glDeleteQueriesEXT"));
		_glBeginQueryEXT = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(eglGetProcAddress("glBeginQueryEXT"));
		_glEndQueryEXT = reinterpret_cast<PFNGLENDQUERYEXTPROC>(eglGetProcAddress("glEndQueryEXT"));
		_glGetQueryObjectui64vEXT = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
		_glGetQueryObjectivEXT = reinterpret_cast<PFNGLGETQUERYOBJECTIVEXTPROC>(eglGetProcAddress("glGetQueryObjectivEXT"));

		if (_glGenQueriesEXT && _glBeginQueryEXT && _glEndQueryEXT && _glGetQueryObjectui64vEXT && _glGetQueryObjectivEXT) {
			_glGenQueriesEXT(2, backend->gpu_query);
			backend->has_timer_query = true;
			Debug(driver, 1, "GLES: GL_EXT_disjoint_timer_query available, GPU timing enabled");
		}
	}
	if (!backend->has_timer_query) {
		Debug(driver, 1, "GLES: GL_EXT_disjoint_timer_query not available");
	}

	GLESBackend::instance = backend;

	Debug(driver, 1, "GLES: Backend initialized successfully");
	return true;
}

void GLESBackend::Destroy()
{
	delete GLESBackend::instance;
	GLESBackend::instance = nullptr;
}

void GLESBackend::RecoverGPUState()
{
	Debug(driver, 0, "GLES: RecoverGPUState: rebuilding GPU objects after context loss");

	/* All old GL handles belong to the dead EGL context.  Zero them out so that
	 * subsequent glDelete* calls inside Resize() / Destroy() are no-ops, and
	 * do NOT call glDelete* on them — that would inject errors into the new context. */
	this->prog_normal = 0; this->prog_remap = 0; this->prog_transparent = 0;
	this->prog_palette = 0; this->prog_solid = 0; this->prog_resolve = 0;
	this->palette_tex = 0;
	this->remap_table_tex[0] = 0; this->remap_table_tex[1] = 0;
	this->vbo = 0;
	this->fbo = 0; this->fbo_tex = 0; this->fbo_idx_tex = 0;
	this->palette_dirty = false;
	this->fbo_has_content = false;
	this->last_remap_ptr = nullptr;
	this->remap_table_idx = 0;
	this->gpu_query[0] = 0; this->gpu_query[1] = 0;
	this->gpu_query_idx = 0;
	this->gpu_query_active = false;
	/* has_timer_query stays true if extension was found; re-create query objects below. */

	/* Recompile and link all shader programs in the new context. */
	if (!this->InitShaders()) {
		Debug(driver, 0, "GLES: RecoverGPUState: FAILED to reinitialize shaders!");
		return;
	}

	/* Create palette texture. */
	glGenTextures(1, &this->palette_tex);
	glBindTexture(GL_TEXTURE_2D, this->palette_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	/* Create remap table textures. */
	glGenTextures(2, this->remap_table_tex);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, this->remap_table_tex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
	}

	/* Create VBO. */
	glGenBuffers(1, &this->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferData(GL_ARRAY_BUFFER, MAX_BATCH_VERTICES * sizeof(GLESVertex), nullptr, GL_DYNAMIC_DRAW);

	/* Re-initialize atlas (GL settings for the new context). */
	this->sprite_atlas.Init();

	/* Request deferred atlas clear — old textures belong to dead context. */
	this->sprite_atlas.RequestClear();

	/* Recreate FBO and cpu_framebuf_tex via Resize() (handles are already 0). */
	if (this->screen_width > 0 && this->screen_height > 0) {
		this->Resize(this->screen_width, this->screen_height);
	}

	/* Discard stale queued state from before context loss. */
	this->draw_queue.clear();
	this->dirty_rects.clear();

	/* Re-create timer query objects if extension was available. */
	if (this->has_timer_query && _glGenQueriesEXT) {
		_glGenQueriesEXT(2, this->gpu_query);
	}

	Debug(driver, 0, "GLES: RecoverGPUState: done, triggering map reload");
}

void GLESBackend::Resize(int w, int h)
{
	this->screen_width = w;
	this->screen_height = h;

	/* (Re)create the persistent MRT FBO at the new size. */
	if (this->fbo_tex != 0) glDeleteTextures(1, &this->fbo_tex);
	if (this->fbo_idx_tex != 0) glDeleteTextures(1, &this->fbo_idx_tex);
	if (this->fbo != 0) glDeleteFramebuffers(1, &this->fbo);

	/* Attachment 0: RGBA colour. */
	glGenTextures(1, &this->fbo_tex);
	glBindTexture(GL_TEXTURE_2D, this->fbo_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	/* Attachment 1: R8 palette index. */
	glGenTextures(1, &this->fbo_idx_tex);
	glBindTexture(GL_TEXTURE_2D, this->fbo_idx_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

	glGenFramebuffers(1, &this->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->fbo_tex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, this->fbo_idx_tex, 0);

	/* Enable MRT: draw to both attachments. */
	GLenum draw_bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, draw_bufs);

	/* Clear both attachments (colour to black, index to 0). */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	this->palette_dirty = false;
	this->fbo_has_content = false;

	/* Bind back to default framebuffer. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, w, h);
}

void GLESBackend::UpdatePalette(const Colour *pal, uint first, uint length)
{
	/* Convert palette entries to RGBA bytes and upload to palette texture. */
	std::vector<uint8_t> rgba(length * 4);
	for (uint i = 0; i < length; i++) {
		rgba[i * 4 + 0] = pal[first + i].r;
		rgba[i * 4 + 1] = pal[first + i].g;
		rgba[i * 4 + 2] = pal[first + i].b;
		/* Palette entries are loaded as 24-bit RGB with alpha=0.
		 * Force alpha=255 for all entries except index 0 (transparent). */
		rgba[i * 4 + 3] = (first + i == 0) ? 0 : 255;
	}

	glBindTexture(GL_TEXTURE_2D, this->palette_tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, first, 0, length, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void GLESBackend::AddDirtyRect(int left, int top, int right, int bottom)
{
	Rect r = {left, top, right, bottom};
	this->dirty_rects.push_back(r);
}

static bool _gles_debug_dirty_overlay = false;

void GLESBackend::DrawDebugDirtyOverlay(const std::vector<Rect> &rects)
{
	if (!_gles_debug_dirty_overlay || rects.empty()) return;

	GLenum buf0 = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &buf0);

	glUseProgram(this->prog_solid);
	glUniform2f(this->solid_screen_loc, static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	static int colour_idx = 0;
	const float colours[][4] = {
		{1.0f, 0.0f, 0.0f, 0.25f},
		{0.0f, 1.0f, 0.0f, 0.25f},
		{0.0f, 0.0f, 1.0f, 0.25f},
		{1.0f, 1.0f, 0.0f, 0.25f},
	};

	for (const Rect &r : rects) {
		const float *c = colours[colour_idx++ % 4];
		float verts[] = {
			(float)r.left, (float)r.top,   (float)r.right, (float)r.top,   (float)r.left, (float)r.bottom,
			(float)r.right, (float)r.top,  (float)r.right, (float)r.bottom, (float)r.left, (float)r.bottom,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
		glUniform4f(this->solid_colour_loc, c[0], c[1], c[2], c[3]);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glDisableVertexAttribArray(0);
	}

	glDisable(GL_BLEND);

	GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
}

void GLESBackend::QueueDraw(const GLESDrawCommand &cmd)
{
	this->draw_queue.push_back(cmd);

	/* Stamp palette_only from the sprite entry for sort/batch routing. */
	const GLESSpriteEntry *entry = this->sprite_atlas.LookupOrUpload(cmd.sprite_key);
	this->draw_queue.back().palette_only = (entry != nullptr && entry->palette_only);
}

bool GLESBackend::Paint()
{
	this->last_remap_ptr = nullptr;

	/* GPU timer query: read previous frame's result (non-blocking). */
	if (this->has_timer_query && this->gpu_query_active) {
		int prev = this->gpu_query_idx ^ 1;
		GLint available = 0;
		_glGetQueryObjectivEXT(this->gpu_query[prev], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
		if (available) {
			/* Check for disjoint operation (GPU clock reset). */
			GLint disjoint = 0;
			glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
			if (!disjoint) {
				GLuint64 elapsed_ns = 0;
				_glGetQueryObjectui64vEXT(this->gpu_query[prev], GL_QUERY_RESULT_EXT, &elapsed_ns);
				_gles_perf.gpu_time_us = static_cast<int64_t>(elapsed_ns / 1000);
			}
		}
	}

	/* GPU timer query: begin new query for this frame. */
	if (this->has_timer_query) {
		_glBeginQueryEXT(GL_TIME_ELAPSED_EXT, this->gpu_query[this->gpu_query_idx]);
	}

	/* === Phase 1: Render into persistent FBO. === */
	glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
	glViewport(0, 0, this->screen_width, this->screen_height);

	/* Orphan the VBO to avoid GPU sync stalls.  Tells the driver we don't
	 * need the old contents, so it can give us fresh storage while the GPU
	 * finishes reading the previous frame's data. */
	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferData(GL_ARRAY_BUFFER, MAX_BATCH_VERTICES * sizeof(GLESVertex), nullptr, GL_DYNAMIC_DRAW);

	/* Clear dirty regions in the FBO before drawing. */
	std::vector<Rect> frame_dirty_rects;
	if (!this->dirty_rects.empty()) {
		frame_dirty_rects = this->dirty_rects;
		glEnable(GL_SCISSOR_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		for (const Rect &r : this->dirty_rects) {
			int gl_y = this->screen_height - r.bottom;
			glScissor(r.left, gl_y, r.right - r.left, r.bottom - r.top);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		glDisable(GL_SCISSOR_TEST);
		this->dirty_rects.clear();
	}

	bool did_full_render = !this->draw_queue.empty();

	if (did_full_render) {

	/* === Build all vertices and record batch boundaries ===
	 * Preserve original Z-order from viewport (no sorting).
	 * Atlas page selection is per-vertex (cpage/rpage), so batches
	 * only break on shader type changes — not atlas changes. */

	enum BatchType : uint8_t { BT_NORMAL, BT_REMAP, BT_TRANSPARENT, BT_PALETTE };
	struct BatchRange {
		size_t start, count;
		BatchType type;
		const uint8_t *remap = nullptr; ///< Remap table pointer for BT_REMAP batches.
	};

	this->vertex_buf.clear();
	std::vector<BatchRange> batches;
	batches.reserve(16);

	BatchType cur_type = BT_NORMAL;
	const uint8_t *cur_remap = nullptr;
	size_t batch_start = 0;
	bool first = true;

	/* Track unique sprite IDs and zoom for GPU scaling verification. */
	static ZoomLevel last_logged_zoom = ZoomLevel::End;
	std::unordered_set<SpriteID> unique_base_sprites;
	std::unordered_set<GLESSpriteID> unique_keys_used;
	ZoomLevel frame_zoom = ZoomLevel::End;

	for (const GLESDrawCommand &cmd : this->draw_queue) {
		/* All sprites are stored at base zoom; cmd.sprite_key already uses kGPUScaleBaseZoom. */
		const GLESSpriteEntry *entry = this->sprite_atlas.LookupOrUpload(cmd.sprite_key);
		if (entry == nullptr) continue;
		bool gpu_scaled = (cmd.zoom != kGPUScaleBaseZoom);

		if (gpu_scaled) {
			_gles_perf.gpu_scaled_hits++;
		} else {
			_gles_perf.gpu_scaled_fallbacks++;
		}

		SpriteID sid = static_cast<SpriteID>(cmd.sprite_key >> 4);
		unique_base_sprites.insert(sid);
		unique_keys_used.insert(cmd.sprite_key);
		if (frame_zoom == ZoomLevel::End) frame_zoom = cmd.zoom;

		/* Track dimension mismatches for diagnostics. */
		if (cmd.sprite_width != entry->colour.w || cmd.sprite_height != entry->colour.h) {
			_gles_perf.gpu_dim_mismatches++;
			/* Log when atlas is SMALLER than expected — UVs would overflow. */
			static int dimmis_log = 0;
			if ((entry->colour.w < cmd.sprite_width || entry->colour.h < cmd.sprite_height) && dimmis_log < 30) {
				dimmis_log++;
				Debug(driver, 3, "GLES: DIM-SMALL key={:#x} cmd=({},{}) atlas=({},{}) skip=({},{}) vis=({},{}) mode={}",
				      cmd.sprite_key, cmd.sprite_width, cmd.sprite_height,
				      static_cast<int>(entry->colour.w), static_cast<int>(entry->colour.h),
				      cmd.skip_left, cmd.skip_top, cmd.width, cmd.height,
				      static_cast<int>(cmd.mode));
			}
		}

		BatchType type;
		if (cmd.mode == BlitterMode::Transparent || cmd.mode == BlitterMode::TransparentRemap) {
			type = BT_TRANSPARENT;
		} else if ((cmd.mode == BlitterMode::ColourRemap || cmd.mode == BlitterMode::CrashRemap ||
		            cmd.mode == BlitterMode::BlackRemap) && entry->has_remap) {
			/* Only use remap shader when sprite actually has M channel data.
			 * Without remap data, the remap UVs point to random atlas locations,
			 * causing garbage M values and invisible/corrupted pixels. */
			type = BT_REMAP;
		} else if (entry->palette_only) {
			type = BT_PALETTE;
		} else {
			type = BT_NORMAL;
		}

		/* Batch break on shader type change or remap table change. */
		bool need_break = !first && type != cur_type;
		if (!need_break && !first && type == BT_REMAP && cmd.remap != cur_remap) {
			need_break = true;
		}
		if (need_break) {
			size_t cnt = this->vertex_buf.size() - batch_start;
			if (cnt > 0) batches.push_back({batch_start, cnt, cur_type, cur_remap});
			batch_start = this->vertex_buf.size();
		}

		cur_type = type;
		if (type == BT_REMAP) cur_remap = cmd.remap;
		first = false;

		/* Per-vertex atlas page indices (0.0 or 1.0). */
		float cp = static_cast<float>(entry->colour.atlas_idx);
		float rp = entry->has_remap ? static_cast<float>(entry->remap.atlas_idx) : 0.0f;

		/* Generate 6 vertices (2 triangles) for this sprite.
		 * When GPU-scaling from a lower-resolution atlas sprite, expand quads
		 * by half a pixel in each direction to eliminate seams between adjacent
		 * tiles caused by coarser diamond edges in the base-zoom texture. */
		float pad = 0.0f;
		float x0 = static_cast<float>(cmd.screen_x) - pad;
		float y0 = static_cast<float>(cmd.screen_y) - pad;
		float x1 = static_cast<float>(cmd.screen_x) + static_cast<float>(cmd.width) + pad;
		float y1 = static_cast<float>(cmd.screen_y) + static_cast<float>(cmd.height) + pad;

		/* Use actual atlas entry dimensions for UV computation.
		 * cmd.sprite_width is derived from root sprite width via integer
		 * UnScaleByZoom, which may differ from the real pixel dimensions
		 * stored in the atlas (rounding).  Using atlas truth prevents UV
		 * errors that sample into neighbouring sprites. */
		float sprite_w = static_cast<float>(entry->colour.w);
		float sprite_h = static_cast<float>(entry->colour.h);

		float uv_skip_l, uv_skip_t, uv_w, uv_h;

		bool full_sprite = (cmd.skip_left == 0 && cmd.skip_top == 0 &&
		                    cmd.width == cmd.sprite_width && cmd.height == cmd.sprite_height);

		if (full_sprite) {
			/* Full sprite — exact UV avoids integer-rounding gaps between tiles. */
			uv_skip_l = 0.0f;
			uv_skip_t = 0.0f;
			uv_w = 1.0f;
			uv_h = 1.0f;
		} else {
			/* Clipped sprite — scale screen coords to atlas coords via zoom ratio. */
			float zoom_scale = gpu_scaled
				? static_cast<float>(1 << to_underlying(cmd.zoom)) /
				  static_cast<float>(1 << to_underlying(kGPUScaleBaseZoom))
				: 1.0f;

			uv_skip_l = (static_cast<float>(cmd.skip_left) * zoom_scale) / sprite_w;
			uv_skip_t = (static_cast<float>(cmd.skip_top)  * zoom_scale) / sprite_h;
			uv_w      = (static_cast<float>(cmd.width)     * zoom_scale) / sprite_w;
			uv_h      = (static_cast<float>(cmd.height)    * zoom_scale) / sprite_h;

			/* Clamp UV to [0,1] — integer rounding across zoom levels can overshoot. */
			if (uv_skip_l + uv_w > 1.0f) uv_w = 1.0f - uv_skip_l;
			if (uv_skip_t + uv_h > 1.0f) uv_h = 1.0f - uv_skip_t;
		}

		/* Detect UV overflow: skip + visible exceeds atlas sprite dimensions. */
		{
			static int uv_log_count = 0;
			float total_u = uv_skip_l + uv_w;
			float total_v = uv_skip_t + uv_h;
			if ((total_u > 1.01f || total_v > 1.01f) && uv_log_count < 30) {
				uv_log_count++;
				Debug(driver, 3, "GLES: UV overflow key={:#x} skip=({},{}) vis=({},{}) atlas=({},{}) cmd_sprite=({},{}) total_uv=({:.3f},{:.3f})",
				      cmd.sprite_key, cmd.skip_left, cmd.skip_top,
				      cmd.width, cmd.height,
				      entry->colour.w, entry->colour.h,
				      cmd.sprite_width, cmd.sprite_height,
				      total_u, total_v);
			}
		}

		float cu0 = entry->colour.u0 + uv_skip_l * (entry->colour.u1 - entry->colour.u0);
		float cv0 = entry->colour.v0 + uv_skip_t * (entry->colour.v1 - entry->colour.v0);
		float cu1 = cu0 + uv_w * (entry->colour.u1 - entry->colour.u0);
		float cv1 = cv0 + uv_h * (entry->colour.v1 - entry->colour.v0);

		float ru0 = cu0, rv0 = cv0, ru1 = cu1, rv1 = cv1;
		if (entry->has_remap) {
			ru0 = entry->remap.u0 + uv_skip_l * (entry->remap.u1 - entry->remap.u0);
			rv0 = entry->remap.v0 + uv_skip_t * (entry->remap.v1 - entry->remap.v0);
			ru1 = ru0 + uv_w * (entry->remap.u1 - entry->remap.u0);
			rv1 = rv0 + uv_h * (entry->remap.v1 - entry->remap.v0);
		}

		GLESVertex v;
		v = {x0, y0, cu0, cv0, ru0, rv0, cp, rp}; this->vertex_buf.push_back(v);
		v = {x1, y0, cu1, cv0, ru1, rv0, cp, rp}; this->vertex_buf.push_back(v);
		v = {x0, y1, cu0, cv1, ru0, rv1, cp, rp}; this->vertex_buf.push_back(v);
		v = {x1, y0, cu1, cv0, ru1, rv0, cp, rp}; this->vertex_buf.push_back(v);
		v = {x1, y1, cu1, cv1, ru1, rv1, cp, rp}; this->vertex_buf.push_back(v);
		v = {x0, y1, cu0, cv1, ru0, rv1, cp, rp}; this->vertex_buf.push_back(v);
	}

	/* Record final batch. */
	if (this->vertex_buf.size() > batch_start) {
		batches.push_back({batch_start, this->vertex_buf.size() - batch_start, cur_type, cur_remap});
	}

	/* Log unique sprite set on zoom change to verify GPU scaling reuses same atlas entries. */
	if (frame_zoom != ZoomLevel::End && frame_zoom != last_logged_zoom) {
		Debug(driver, 3, "GLES SCALE: zoom={} cmds={} unique_sprites={} unique_keys={} scaled={} fallback={}",
		      to_underlying(frame_zoom), this->draw_queue.size(),
		      unique_base_sprites.size(), unique_keys_used.size(),
		      _gles_perf.gpu_scaled_hits, _gles_perf.gpu_scaled_fallbacks);
		last_logged_zoom = frame_zoom;
	}

	/* === Single upload of ALL vertices === */
	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0,
		this->vertex_buf.size() * sizeof(GLESVertex), this->vertex_buf.data());

	/* Set up vertex attributes once (locations 0-4 forced via glBindAttribLocation). */
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, ru)));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, cpage)));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, rpage)));

	/* === Bind ALL atlas textures once ===
	 * Unit 0: colour page 0    Unit 1: colour page 1
	 * Unit 2: remap page 0     Unit 3: remap page 1
	 * Unit 4: palette           Unit 5: remap table */
	GLuint cp0 = this->sprite_atlas.GetColourPageCount() > 0 ? this->sprite_atlas.GetColourTexture(0) : 0;
	GLuint cp1 = this->sprite_atlas.GetColourPageCount() > 1 ? this->sprite_atlas.GetColourTexture(1) : cp0;
	GLuint rp0 = this->sprite_atlas.GetRemapPageCount() > 0 ? this->sprite_atlas.GetRemapTexture(0) : 0;
	GLuint rp1 = this->sprite_atlas.GetRemapPageCount() > 1 ? this->sprite_atlas.GetRemapTexture(1) : rp0;

	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, cp0);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, cp1);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, rp0);
	glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, rp1);
	glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, this->palette_tex);
	glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, this->remap_table_tex[this->remap_table_idx]);

	/* === Draw batches — only break on shader type change === */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	float sw = static_cast<float>(this->screen_width);
	float sh = static_cast<float>(this->screen_height);
	BatchType prev_type = BT_NORMAL;
	bool first_batch = true;

	for (const BatchRange &b : batches) {
		_gles_perf.gpu_batches++;

		if (first_batch || b.type != prev_type) {
			if (!first_batch && prev_type == BT_TRANSPARENT) {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}

			switch (b.type) {
			case BT_NORMAL:
				glUseProgram(this->prog_normal);
				glUniform2f(this->normal_screen_loc, sw, sh);
				glUniform1i(this->normal_colour_tex_loc, 0);
				glUniform1i(this->normal_colour_tex1_loc, 1);
				break;

			case BT_TRANSPARENT:
				glUseProgram(this->prog_transparent);
				glUniform2f(this->trans_screen_loc, sw, sh);
				glUniform1i(this->trans_colour_tex_loc, 0);
				glUniform1i(this->trans_colour_tex1_loc, 1);
				glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
				break;

			case BT_REMAP:
				glUseProgram(this->prog_remap);
				glUniform2f(this->remap_screen_loc, sw, sh);
				glUniform1i(this->remap_colour_tex_loc, 0);
				glUniform1i(this->remap_colour_tex1_loc, 1);
				glUniform1i(this->remap_remap_tex_loc, 2);
				glUniform1i(this->remap_remap_tex1_loc, 3);
				glUniform1i(this->remap_palette_tex_loc, 4);
				glUniform1i(this->remap_table_tex_loc, 5);
				break;

			case BT_PALETTE:
				glUseProgram(this->prog_palette);
				glUniform2f(this->pal_screen_loc, sw, sh);
				glUniform1i(this->pal_remap_tex_loc, 2);
				glUniform1i(this->pal_remap_tex1_loc, 3);
				glUniform1i(this->pal_palette_tex_loc, 4);
				break;
			}
		}

		/* Upload remap table for remap batches, skip if same pointer as last time. */
		if (b.type == BT_REMAP && b.remap != nullptr && b.remap != this->last_remap_ptr) {
			/* Swap to the other remap texture to avoid GPU ghost on the one still in use. */
			this->remap_table_idx ^= 1;
			glActiveTexture(GL_TEXTURE5);
			glBindTexture(GL_TEXTURE_2D, this->remap_table_tex[this->remap_table_idx]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
			                GL_RED, GL_UNSIGNED_BYTE, b.remap);
			this->last_remap_ptr = b.remap;
		}

		prev_type = b.type;
		first_batch = false;

		glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.start), static_cast<GLsizei>(b.count));
	}

	/* Clean up sprite rendering state. */
	if (prev_type == BT_TRANSPARENT) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	for (int i = 0; i < 5; i++) glDisableVertexAttribArray(i);
	this->draw_queue.clear();

	} /* end if (did_full_render) */

	if (did_full_render) {
		this->fbo_has_content = true;
		this->palette_dirty = false;
		_gles_perf.full_renders++;
	} else if (this->palette_dirty && this->fbo_has_content) {
		/* === Phase 1.5: Palette resolve pass ===
		 * Camera is static, only palette changed. Read palette index from
		 * attachment 1, look up updated palette, write to attachment 0. */
		auto t_resolve0 = std::chrono::steady_clock::now();
		glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
		glViewport(0, 0, this->screen_width, this->screen_height);

		/* Detach idx texture from FBO to avoid feedback loop, then
		 * switch to single-attachment output (attachment 0 only). */
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
		GLenum single_buf = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &single_buf);

		/* Draw fullscreen quad with resolve shader. */
		glUseProgram(this->prog_resolve);
		float sw = static_cast<float>(this->screen_width);
		float sh = static_cast<float>(this->screen_height);
		glUniform2f(this->resolve_screen_loc, sw, sh);

		/* Bind idx texture to unit 0, palette to unit 1. */
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, this->fbo_idx_tex);
		glUniform1i(this->resolve_idx_tex_loc, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, this->palette_tex);
		glUniform1i(this->resolve_palette_tex_loc, 1);

		/* Blending: discard overwrites palette pixels, keeps non-palette. */
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		GLESVertex resolve_quad[6] = {
			{0, 0, 0, 1, 0, 0, 0, 0}, {sw, 0, 1, 1, 0, 0, 0, 0}, {0, sh, 0, 0, 0, 0, 0, 0},
			{sw, 0, 1, 1, 0, 0, 0, 0}, {sw, sh, 1, 0, 0, 0, 0, 0}, {0, sh, 0, 0, 0, 0, 0, 0},
		};

		glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(resolve_quad), resolve_quad);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
		glVertexAttrib1f(3, 0.0f);

		glDrawArrays(GL_TRIANGLES, 0, 6);

		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glDisable(GL_BLEND);

		/* Reattach idx texture and restore MRT for future full renders. */
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, this->fbo_idx_tex, 0);
		GLenum mrt_bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, mrt_bufs);

		this->palette_dirty = false;
		_gles_perf.gpu_batches++;
		_gles_perf.resolve_passes++;
		_gles_perf.resolve_us += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t_resolve0).count();
	} else {
		_gles_perf.idle_blits++;
		/* Nothing changed — skip FBO blit and swap entirely. */
		if (this->has_timer_query) {
			_glEndQueryEXT(GL_TIME_ELAPSED_EXT);
			this->gpu_query_idx ^= 1;
			this->gpu_query_active = true;
		}
		return false;
	}

	this->DrawDebugDirtyOverlay(frame_dirty_rects);

	/* === Phase 2: Blit FBO to the actual screen. === */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, this->screen_width, this->screen_height);

	glDisable(GL_BLEND);
	glUseProgram(this->prog_normal);
	glUniform2f(this->normal_screen_loc,
		static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));

	/* Bind FBO texture to unit 0, set both samplers to unit 0 (cpage=0). */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, this->fbo_tex);
	glUniform1i(this->normal_colour_tex_loc, 0);
	glUniform1i(this->normal_colour_tex1_loc, 0);

	float w = static_cast<float>(this->screen_width);
	float h = static_cast<float>(this->screen_height);
	GLESVertex quad[6] = {
		{0, 0, 0, 1, 0, 0, 0, 0}, {w, 0, 1, 1, 0, 0, 0, 0}, {0, h, 0, 0, 0, 0, 0, 0},
		{w, 0, 1, 1, 0, 0, 0, 0}, {w, h, 1, 0, 0, 0, 0, 0}, {0, h, 0, 0, 0, 0, 0, 0},
	};

	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
	glVertexAttrib1f(3, 0.0f); /* cpage = 0 for FBO blit */
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	/* GPU timer query: end this frame's query. */
	if (this->has_timer_query) {
		_glEndQueryEXT(GL_TIME_ELAPSED_EXT);
		this->gpu_query_idx ^= 1;
		this->gpu_query_active = true;
	}
	return true;
}
