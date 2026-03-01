/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_backend.h OpenGL ES backend for GPU-accelerated sprite rendering. */

#ifndef VIDEO_GLES_BACKEND_H
#define VIDEO_GLES_BACKEND_H

#include "../gfx_type.h"
#include "../blitter/base.hpp"
#include "gles_sprite.h"
#include <GLES2/gl2.h>
#include <vector>

/** A single draw command recorded by the GLES blitter. */
struct GLESDrawCommand {
	GLESSpriteID sprite_key;      ///< Key into the sprite atlas lookup.
	int16_t screen_x, screen_y;   ///< Screen position (pixels).
	int16_t width, height;        ///< Visible dimensions (pixels, after clipping).
	int16_t skip_left, skip_top;  ///< Source skip offsets for clipping.
	int16_t sprite_width;         ///< Full sprite width (for UV computation).
	int16_t sprite_height;        ///< Full sprite height (for UV computation).
	ZoomLevel zoom;               ///< Zoom level.
	BlitterMode mode;             ///< Blitter mode.
	uint8_t remap_idx;            ///< Remap table index (for ColourRemap mode).
};

/** Vertex for batched sprite rendering: position + colour UV + remap UV. */
struct GLESVertex {
	float x, y;     ///< Screen position in pixels.
	float u, v;     ///< Colour atlas UV.
	float ru, rv;   ///< Remap atlas UV.
};

/** OpenGL ES backend singleton managing shaders, textures, and batched rendering. */
class GLESBackend {
private:
	static GLESBackend *instance;

	GLuint prog_normal = 0;      ///< Shader program for normal sprites.
	GLuint prog_remap = 0;       ///< Shader program for colour-remapped sprites.
	GLuint prog_transparent = 0; ///< Shader program for transparent sprites.
	GLuint prog_bgra = 0;        ///< Shader program for CPU framebuffer (BGRA→RGBA swizzle).

	/* Normal program uniforms/attributes. */
	GLint normal_screen_loc = -1;
	GLint normal_colour_tex_loc = -1;
	GLint normal_pos_attr = -1;
	GLint normal_colour_uv_attr = -1;
	GLint normal_remap_uv_attr = -1;

	/* Remap program uniforms/attributes. */
	GLint remap_screen_loc = -1;
	GLint remap_colour_tex_loc = -1;
	GLint remap_remap_tex_loc = -1;
	GLint remap_palette_tex_loc = -1;
	GLint remap_table_tex_loc = -1;
	GLint remap_pos_attr = -1;
	GLint remap_colour_uv_attr = -1;
	GLint remap_remap_uv_attr = -1;

	/* Transparent program uniforms/attributes. */
	GLint trans_screen_loc = -1;
	GLint trans_colour_tex_loc = -1;
	GLint trans_pos_attr = -1;
	GLint trans_colour_uv_attr = -1;
	GLint trans_remap_uv_attr = -1;

	/* BGRA program uniforms/attributes (for CPU framebuffer). */
	GLint bgra_screen_loc = -1;
	GLint bgra_colour_tex_loc = -1;
	GLint bgra_pos_attr = -1;
	GLint bgra_colour_uv_attr = -1;

	GLuint palette_tex = 0;      ///< 256x1 RGBA palette texture.
	GLuint remap_table_tex = 0;  ///< 256x1 remap table texture (current remap).
	GLuint vbo = 0;              ///< Vertex buffer for batched quads.
	GLuint cpu_framebuf_tex = 0; ///< Texture for CPU-rendered content (video buffer upload).

	int screen_width = 0;
	int screen_height = 0;

	GLESSpriteAtlas sprite_atlas; ///< Sprite atlas manager.

	std::vector<GLESDrawCommand> draw_queue; ///< Pending draw commands.
	std::vector<GLESVertex> vertex_buf;      ///< Temporary vertex assembly buffer.

	GLESBackend();
	~GLESBackend();

	bool InitShaders();
	GLuint CompileShader(GLenum type, const char *source);
	GLuint LinkProgram(GLuint vert, GLuint frag);

	void FlushBatch(GLuint program, GLint screen_loc, GLint colour_tex_loc,
	                GLint pos_attr, GLint colour_uv_attr, GLint remap_uv_attr,
	                GLuint colour_atlas, GLuint remap_atlas,
	                const GLESVertex *vertices, size_t count);

public:
	static GLESBackend *Get() { return instance; }
	static bool Create();
	static void Destroy();

	void Resize(int w, int h);
	void UpdatePalette(const Colour *pal, uint first, uint length);

	/** Upload CPU-rendered video buffer as background before GPU sprites. */
	void UploadVideoBuffer(const void *buffer, int w, int h);

	/** Get the sprite atlas for encoding sprites. */
	GLESSpriteAtlas &GetSpriteAtlas() { return sprite_atlas; }

	/** Add a draw command to the queue. Called from the GLES blitter's Draw(). */
	void QueueDraw(const GLESDrawCommand &cmd);

	/** Flush all queued draw commands as batched GL draw calls. */
	void Paint();

	/** Clear the draw queue without rendering. */
	void ClearQueue() { draw_queue.clear(); }

	int GetScreenWidth() const { return screen_width; }
	int GetScreenHeight() const { return screen_height; }
};

#endif /* VIDEO_GLES_BACKEND_H */
