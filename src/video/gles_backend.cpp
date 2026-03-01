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
#include "../palette_func.h"
#include "../table/gles_shader.h"
#include <GLES2/gl2.h>
#include <algorithm>

#include "../safeguards.h"

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
	if (this->prog_bgra != 0) glDeleteProgram(this->prog_bgra);
	if (this->palette_tex != 0) glDeleteTextures(1, &this->palette_tex);
	if (this->remap_table_tex != 0) glDeleteTextures(1, &this->remap_table_tex);
	if (this->cpu_framebuf_tex != 0) glDeleteTextures(1, &this->cpu_framebuf_tex);
	if (this->fbo_tex != 0) glDeleteTextures(1, &this->fbo_tex);
	if (this->fbo != 0) glDeleteFramebuffers(1, &this->fbo);
	if (this->vbo != 0) glDeleteBuffers(1, &this->vbo);
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
		this->normal_pos_attr = glGetAttribLocation(this->prog_normal, "a_position");
		this->normal_colour_uv_attr = glGetAttribLocation(this->prog_normal, "a_colour_uv");
		this->normal_remap_uv_attr = glGetAttribLocation(this->prog_normal, "a_remap_uv");
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
		this->remap_remap_tex_loc = glGetUniformLocation(this->prog_remap, "remap_tex");
		this->remap_palette_tex_loc = glGetUniformLocation(this->prog_remap, "palette_tex");
		this->remap_table_tex_loc = glGetUniformLocation(this->prog_remap, "remap_table_tex");
		this->remap_pos_attr = glGetAttribLocation(this->prog_remap, "a_position");
		this->remap_colour_uv_attr = glGetAttribLocation(this->prog_remap, "a_colour_uv");
		this->remap_remap_uv_attr = glGetAttribLocation(this->prog_remap, "a_remap_uv");
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
		this->trans_pos_attr = glGetAttribLocation(this->prog_transparent, "a_position");
		this->trans_colour_uv_attr = glGetAttribLocation(this->prog_transparent, "a_colour_uv");
		this->trans_remap_uv_attr = glGetAttribLocation(this->prog_transparent, "a_remap_uv");
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
		this->pal_palette_tex_loc = glGetUniformLocation(this->prog_palette, "palette_tex");
		this->pal_pos_attr = glGetAttribLocation(this->prog_palette, "a_position");
		this->pal_remap_uv_attr = glGetAttribLocation(this->prog_palette, "a_remap_uv");
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
		this->solid_pos_attr = glGetAttribLocation(this->prog_solid, "a_position");
	}

	/* BGRA swizzle fragment shader (for CPU framebuffer upload). */
	{
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, _gles_frag_shader_bgra);
		if (fs == 0) { glDeleteShader(vs); return false; }
		this->prog_bgra = LinkProgram(vs, fs);
		glDeleteShader(fs);
		if (this->prog_bgra == 0) { glDeleteShader(vs); return false; }

		this->bgra_screen_loc = glGetUniformLocation(this->prog_bgra, "screen");
		this->bgra_colour_tex_loc = glGetUniformLocation(this->prog_bgra, "colour_tex");
		this->bgra_pos_attr = glGetAttribLocation(this->prog_bgra, "a_position");
		this->bgra_colour_uv_attr = glGetAttribLocation(this->prog_bgra, "a_colour_uv");
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

	/* Create CPU framebuffer texture for CPU-rendered content. */
	glGenTextures(1, &backend->cpu_framebuf_tex);
	glBindTexture(GL_TEXTURE_2D, backend->cpu_framebuf_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Create 256x1 remap table texture (luminance). */
	glGenTextures(1, &backend->remap_table_tex);
	glBindTexture(GL_TEXTURE_2D, backend->remap_table_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 1, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

	/* Create VBO for batched vertices. */
	glGenBuffers(1, &backend->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, backend->vbo);
	glBufferData(GL_ARRAY_BUFFER, MAX_BATCH_VERTICES * sizeof(GLESVertex), nullptr, GL_DYNAMIC_DRAW);

	backend->sprite_atlas.Init();

	/* Reserve capacity for draw queue and vertex buffer. */
	backend->draw_queue.reserve(4096);
	backend->vertex_buf.reserve(MAX_BATCH_VERTICES);

	GLESBackend::instance = backend;

	Debug(driver, 1, "GLES: Backend initialized successfully");
	return true;
}

void GLESBackend::Destroy()
{
	delete GLESBackend::instance;
	GLESBackend::instance = nullptr;
}

void GLESBackend::Resize(int w, int h)
{
	this->screen_width = w;
	this->screen_height = h;

	/* (Re)create the persistent FBO at the new size. */
	if (this->fbo_tex != 0) glDeleteTextures(1, &this->fbo_tex);
	if (this->fbo != 0) glDeleteFramebuffers(1, &this->fbo);

	glGenTextures(1, &this->fbo_tex);
	glBindTexture(GL_TEXTURE_2D, this->fbo_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	glGenFramebuffers(1, &this->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->fbo_tex, 0);

	/* Clear the new FBO to black. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

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
		rgba[i * 4 + 3] = pal[first + i].a;
	}

	glBindTexture(GL_TEXTURE_2D, this->palette_tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, first, 0, length, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void GLESBackend::UploadVideoBuffer(const void *buffer, int w, int h)
{
	if (buffer == nullptr || w <= 0 || h <= 0) return;

	glBindTexture(GL_TEXTURE_2D, this->cpu_framebuf_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
}

void GLESBackend::QueueDraw(const GLESDrawCommand &cmd)
{
	this->draw_queue.push_back(cmd);

	/* Stamp palette_only from the sprite entry for sort/batch routing. */
	const GLESSpriteEntry *entry = this->sprite_atlas.Lookup(cmd.sprite_key);
	this->draw_queue.back().palette_only = (entry != nullptr && entry->palette_only);
}

void GLESBackend::FlushBatch(GLuint program, GLint screen_loc, GLint colour_tex_loc,
                              GLint pos_attr, GLint colour_uv_attr, GLint remap_uv_attr,
                              GLuint colour_atlas, GLuint remap_atlas,
                              const GLESVertex *vertices, size_t count)
{
	if (count == 0) return;

	glUseProgram(program);
	glUniform2f(screen_loc, static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));

	/* Bind colour atlas to texture unit 0. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, colour_atlas);
	glUniform1i(colour_tex_loc, 0);

	/* Upload vertex data. */
	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(GLESVertex), vertices);

	/* Set up vertex attributes. */
	glEnableVertexAttribArray(pos_attr);
	glVertexAttribPointer(pos_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));

	if (colour_uv_attr >= 0) {
		glEnableVertexAttribArray(colour_uv_attr);
		glVertexAttribPointer(colour_uv_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
	}

	if (remap_uv_attr >= 0) {
		glEnableVertexAttribArray(remap_uv_attr);
		glVertexAttribPointer(remap_uv_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, ru)));
	}

	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));

	glDisableVertexAttribArray(pos_attr);
	if (colour_uv_attr >= 0) glDisableVertexAttribArray(colour_uv_attr);
	if (remap_uv_attr >= 0) glDisableVertexAttribArray(remap_uv_attr);
}

void GLESBackend::FlushPaletteBatch(GLuint remap_atlas,
                                     const GLESVertex *vertices, size_t count)
{
	if (count == 0) return;

	glUseProgram(this->prog_palette);
	glUniform2f(this->pal_screen_loc,
		static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));

	/* Unit 0: remap atlas (M channel). */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, remap_atlas);
	glUniform1i(this->pal_remap_tex_loc, 0);

	/* Unit 1: palette texture. */
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, this->palette_tex);
	glUniform1i(this->pal_palette_tex_loc, 1);

	/* Upload vertices and draw. */
	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(GLESVertex), vertices);

	glEnableVertexAttribArray(this->pal_pos_attr);
	glVertexAttribPointer(this->pal_pos_attr, 2, GL_FLOAT, GL_FALSE,
	                      sizeof(GLESVertex), reinterpret_cast<void *>(offsetof(GLESVertex, x)));
	glEnableVertexAttribArray(this->pal_remap_uv_attr);
	glVertexAttribPointer(this->pal_remap_uv_attr, 2, GL_FLOAT, GL_FALSE,
	                      sizeof(GLESVertex), reinterpret_cast<void *>(offsetof(GLESVertex, ru)));

	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));

	glDisableVertexAttribArray(this->pal_pos_attr);
	glDisableVertexAttribArray(this->pal_remap_uv_attr);
}

void GLESBackend::Paint()
{
	/* === Phase 1: Render into persistent FBO. === */
	glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
	glViewport(0, 0, this->screen_width, this->screen_height);

	/* Clear FBO and draw CPU buffer as the complete base scene.
	 * The CPU blitter always renders all sprites, so the video buffer
	 * contains the full scene. GPU sprites overlay on top for optimization. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (this->cpu_framebuf_tex != 0 && this->screen_width > 0 && this->screen_height > 0) {
		glDisable(GL_BLEND);
		glUseProgram(this->prog_bgra);
		glUniform2f(this->bgra_screen_loc,
			static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, this->cpu_framebuf_tex);
		glUniform1i(this->bgra_colour_tex_loc, 0);

		float bw = static_cast<float>(this->screen_width);
		float bh = static_cast<float>(this->screen_height);
		GLESVertex quad[6] = {
			{0, 0, 0, 0, 0, 0}, {bw, 0, 1, 0, 0, 0}, {0, bh, 0, 1, 0, 0},
			{bw, 0, 1, 0, 0, 0}, {bw, bh, 1, 1, 0, 0}, {0, bh, 0, 1, 0, 0},
		};

		glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

		glEnableVertexAttribArray(this->bgra_pos_attr);
		glVertexAttribPointer(this->bgra_pos_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));
		if (this->bgra_colour_uv_attr >= 0) {
			glEnableVertexAttribArray(this->bgra_colour_uv_attr);
			glVertexAttribPointer(this->bgra_colour_uv_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
			                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
		}
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glDisableVertexAttribArray(this->bgra_pos_attr);
		if (this->bgra_colour_uv_attr >= 0) glDisableVertexAttribArray(this->bgra_colour_uv_attr);
	}

	if (!this->draw_queue.empty()) {

	glEnable(GL_BLEND);

	this->vertex_buf.clear();

	/* Sort draw queue for batching.
	 * Primary: transparent vs non-transparent (different blend func).
	 * Secondary: palette_only vs normal (different shader).
	 * Tertiary: by mode. */
	std::stable_sort(this->draw_queue.begin(), this->draw_queue.end(),
		[](const GLESDrawCommand &a, const GLESDrawCommand &b) {
			bool a_trans = (a.mode == BlitterMode::Transparent || a.mode == BlitterMode::TransparentRemap);
			bool b_trans = (b.mode == BlitterMode::Transparent || b.mode == BlitterMode::TransparentRemap);
			if (a_trans != b_trans) return !a_trans;
			if (!a_trans && a.palette_only != b.palette_only) return !a.palette_only;
			return a.mode < b.mode;
		});

	/* Process draw queue in batches by mode. */
	size_t i = 0;
	while (i < this->draw_queue.size()) {
		BlitterMode batch_mode = this->draw_queue[i].mode;
		this->vertex_buf.clear();

		/* Determine which shader and blend mode to use. */
		bool is_transparent = (batch_mode == BlitterMode::Transparent ||
		                       batch_mode == BlitterMode::TransparentRemap);
		bool is_remap = (batch_mode == BlitterMode::ColourRemap ||
		                 batch_mode == BlitterMode::CrashRemap ||
		                 batch_mode == BlitterMode::BlackRemap);
		bool is_palette = this->draw_queue[i].palette_only && !is_transparent && !is_remap;

		if (is_transparent) {
			glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
		} else {
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}

		/* Accumulate vertices for this batch. */
		GLuint cur_colour_atlas = 0;
		GLuint cur_remap_atlas = 0;
		bool first = true;

		while (i < this->draw_queue.size()) {
			const GLESDrawCommand &cmd = this->draw_queue[i];

			/* Break batch on mode change. */
			bool cmd_trans = (cmd.mode == BlitterMode::Transparent ||
			                  cmd.mode == BlitterMode::TransparentRemap);
			bool cmd_remap = (cmd.mode == BlitterMode::ColourRemap ||
			                  cmd.mode == BlitterMode::CrashRemap ||
			                  cmd.mode == BlitterMode::BlackRemap);
			bool cmd_palette = cmd.palette_only && !cmd_trans && !cmd_remap;
			if (cmd_trans != is_transparent) break;
			if (cmd_remap != is_remap && !is_transparent) break;
			if (cmd_palette != is_palette && !is_transparent) break;

			const GLESSpriteEntry *entry = this->sprite_atlas.Lookup(cmd.sprite_key);
			if (entry == nullptr) { i++; continue; }

			GLuint colour_atlas = this->sprite_atlas.GetColourTexture(entry->colour.atlas_idx);
			GLuint remap_atlas = entry->has_remap ? this->sprite_atlas.GetRemapTexture(entry->remap.atlas_idx) : 0;

			/* Flush if atlas changes or buffer is full. */
			if (!first && (colour_atlas != cur_colour_atlas ||
			    ((is_remap || is_palette) && remap_atlas != cur_remap_atlas) ||
			    this->vertex_buf.size() + 6 > MAX_BATCH_VERTICES)) {

				/* Flush current batch. */
				if (is_transparent) {
					FlushBatch(this->prog_transparent, this->trans_screen_loc,
					           this->trans_colour_tex_loc,
					           this->trans_pos_attr, this->trans_colour_uv_attr,
					           this->trans_remap_uv_attr,
					           cur_colour_atlas, cur_remap_atlas,
					           this->vertex_buf.data(), this->vertex_buf.size());
				} else if (is_remap) {
					glUseProgram(this->prog_remap);
					glActiveTexture(GL_TEXTURE1);
					glBindTexture(GL_TEXTURE_2D, cur_remap_atlas);
					glUniform1i(this->remap_remap_tex_loc, 1);
					glActiveTexture(GL_TEXTURE2);
					glBindTexture(GL_TEXTURE_2D, this->palette_tex);
					glUniform1i(this->remap_palette_tex_loc, 2);
					glActiveTexture(GL_TEXTURE3);
					glBindTexture(GL_TEXTURE_2D, this->remap_table_tex);
					glUniform1i(this->remap_table_tex_loc, 3);

					FlushBatch(this->prog_remap, this->remap_screen_loc,
					           this->remap_colour_tex_loc,
					           this->remap_pos_attr, this->remap_colour_uv_attr,
					           this->remap_remap_uv_attr,
					           cur_colour_atlas, cur_remap_atlas,
					           this->vertex_buf.data(), this->vertex_buf.size());
				} else if (is_palette) {
					FlushPaletteBatch(cur_remap_atlas,
					                  this->vertex_buf.data(), this->vertex_buf.size());
				} else {
					FlushBatch(this->prog_normal, this->normal_screen_loc,
					           this->normal_colour_tex_loc,
					           this->normal_pos_attr, this->normal_colour_uv_attr,
					           this->normal_remap_uv_attr,
					           cur_colour_atlas, cur_remap_atlas,
					           this->vertex_buf.data(), this->vertex_buf.size());
				}
				this->vertex_buf.clear();
			}

			cur_colour_atlas = colour_atlas;
			cur_remap_atlas = remap_atlas;
			first = false;

			/* Compute screen quad coordinates. */
			float x0 = static_cast<float>(cmd.screen_x);
			float y0 = static_cast<float>(cmd.screen_y);
			float x1 = x0 + static_cast<float>(cmd.width);
			float y1 = y0 + static_cast<float>(cmd.height);

			/* Compute UVs with clipping applied.
			 * The sprite region covers the full sprite, but we may only draw
			 * a portion of it due to viewport clipping. */
			float sprite_w = static_cast<float>(cmd.sprite_width);
			float sprite_h = static_cast<float>(cmd.sprite_height);

			/* Fraction of the sprite region that's visible. */
			float uv_skip_l = static_cast<float>(cmd.skip_left) / sprite_w;
			float uv_skip_t = static_cast<float>(cmd.skip_top) / sprite_h;
			float uv_w = static_cast<float>(cmd.width) / sprite_w;
			float uv_h = static_cast<float>(cmd.height) / sprite_h;

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

			/* Two triangles per quad (6 vertices). */
			GLESVertex v;

			/* Triangle 1: top-left, top-right, bottom-left */
			v = {x0, y0, cu0, cv0, ru0, rv0}; this->vertex_buf.push_back(v);
			v = {x1, y0, cu1, cv0, ru1, rv0}; this->vertex_buf.push_back(v);
			v = {x0, y1, cu0, cv1, ru0, rv1}; this->vertex_buf.push_back(v);

			/* Triangle 2: top-right, bottom-right, bottom-left */
			v = {x1, y0, cu1, cv0, ru1, rv0}; this->vertex_buf.push_back(v);
			v = {x1, y1, cu1, cv1, ru1, rv1}; this->vertex_buf.push_back(v);
			v = {x0, y1, cu0, cv1, ru0, rv1}; this->vertex_buf.push_back(v);

			i++;
		}

		/* Flush remaining vertices. */
		if (!this->vertex_buf.empty()) {
			if (is_transparent) {
				FlushBatch(this->prog_transparent, this->trans_screen_loc,
				           this->trans_colour_tex_loc,
				           this->trans_pos_attr, this->trans_colour_uv_attr,
				           this->trans_remap_uv_attr,
				           cur_colour_atlas, cur_remap_atlas,
				           this->vertex_buf.data(), this->vertex_buf.size());
			} else if (is_remap) {
				glUseProgram(this->prog_remap);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, cur_remap_atlas);
				glUniform1i(this->remap_remap_tex_loc, 1);
				glActiveTexture(GL_TEXTURE2);
				glBindTexture(GL_TEXTURE_2D, this->palette_tex);
				glUniform1i(this->remap_palette_tex_loc, 2);
				glActiveTexture(GL_TEXTURE3);
				glBindTexture(GL_TEXTURE_2D, this->remap_table_tex);
				glUniform1i(this->remap_table_tex_loc, 3);

				FlushBatch(this->prog_remap, this->remap_screen_loc,
				           this->remap_colour_tex_loc,
				           this->remap_pos_attr, this->remap_colour_uv_attr,
				           this->remap_remap_uv_attr,
				           cur_colour_atlas, cur_remap_atlas,
				           this->vertex_buf.data(), this->vertex_buf.size());
			} else if (is_palette) {
				FlushPaletteBatch(cur_remap_atlas,
				                  this->vertex_buf.data(), this->vertex_buf.size());
			} else {
				FlushBatch(this->prog_normal, this->normal_screen_loc,
				           this->normal_colour_tex_loc,
				           this->normal_pos_attr, this->normal_colour_uv_attr,
				           this->normal_remap_uv_attr,
				           cur_colour_atlas, cur_remap_atlas,
				           this->vertex_buf.data(), this->vertex_buf.size());
			}
		}
	}

	/* Reset blend mode and clear queue. */
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	this->draw_queue.clear();

	} /* end if (!draw_queue.empty()) */

	/* === Phase 2: Blit FBO to the actual screen. === */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, this->screen_width, this->screen_height);

	/* Use the normal (pass-through) shader, NOT the BGRA swizzle shader.
	 * The FBO already contains correct RGBA from Phase 1 (where BGRA shader
	 * swizzled the CPU buffer). Using BGRA again would double-swizzle.
	 * Flip V coordinates (1→0 instead of 0→1) because the FBO texture is
	 * stored bottom-up in OpenGL but the vertex shader maps pixel Y top-down. */
	glDisable(GL_BLEND);
	glUseProgram(this->prog_normal);
	glUniform2f(this->normal_screen_loc,
		static_cast<float>(this->screen_width), static_cast<float>(this->screen_height));

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, this->fbo_tex);
	glUniform1i(this->normal_colour_tex_loc, 0);

	float w = static_cast<float>(this->screen_width);
	float h = static_cast<float>(this->screen_height);
	GLESVertex quad[6] = {
		{0, 0, 0, 1, 0, 0}, {w, 0, 1, 1, 0, 0}, {0, h, 0, 0, 0, 0},
		{w, 0, 1, 1, 0, 0}, {w, h, 1, 0, 0, 0}, {0, h, 0, 0, 0, 0},
	};

	glBindBuffer(GL_ARRAY_BUFFER, this->vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

	glEnableVertexAttribArray(this->normal_pos_attr);
	glVertexAttribPointer(this->normal_pos_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
	                      reinterpret_cast<void *>(offsetof(GLESVertex, x)));
	if (this->normal_colour_uv_attr >= 0) {
		glEnableVertexAttribArray(this->normal_colour_uv_attr);
		glVertexAttribPointer(this->normal_colour_uv_attr, 2, GL_FLOAT, GL_FALSE, sizeof(GLESVertex),
		                      reinterpret_cast<void *>(offsetof(GLESVertex, u)));
	}
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(this->normal_pos_attr);
	if (this->normal_colour_uv_attr >= 0) glDisableVertexAttribArray(this->normal_colour_uv_attr);
}
