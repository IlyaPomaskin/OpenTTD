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
	const uint8_t *remap = nullptr; ///< Remap table for ColourRemap mode (256 bytes).
	bool palette_only;            ///< True if sprite has only M channel (no RGB data).
	uint16_t sort_atlas;          ///< Atlas page index for sorting (set before sort).
};

/** Vertex for batched sprite rendering: position + colour UV + remap UV + page indices. */
struct GLESVertex {
	float x, y;     ///< Screen position in pixels.
	float u, v;     ///< Colour atlas UV.
	float ru, rv;   ///< Remap atlas UV.
	float cpage;    ///< Colour atlas page index (0.0 or 1.0).
	float rpage;    ///< Remap atlas page index (0.0 or 1.0).
};

/** OpenGL ES backend singleton managing shaders, textures, and batched rendering. */
class GLESBackend {
private:
	static GLESBackend *instance;

	GLuint prog_normal = 0;      ///< Shader program for normal sprites.
	GLuint prog_remap = 0;       ///< Shader program for colour-remapped sprites.
	GLuint prog_transparent = 0; ///< Shader program for transparent sprites.
	GLuint prog_palette = 0;     ///< Shader program for palette-only sprites (M → palette lookup).
	GLuint prog_solid = 0;       ///< Shader program for debug solid colour.

	/* Normal program uniforms. */
	GLint normal_screen_loc = -1;
	GLint normal_colour_tex_loc = -1;
	GLint normal_colour_tex1_loc = -1;

	/* Remap program uniforms. */
	GLint remap_screen_loc = -1;
	GLint remap_colour_tex_loc = -1;
	GLint remap_colour_tex1_loc = -1;
	GLint remap_remap_tex_loc = -1;
	GLint remap_remap_tex1_loc = -1;
	GLint remap_palette_tex_loc = -1;
	GLint remap_table_tex_loc = -1;

	/* Transparent program uniforms. */
	GLint trans_screen_loc = -1;
	GLint trans_colour_tex_loc = -1;
	GLint trans_colour_tex1_loc = -1;

	/* Palette program uniforms. */
	GLint pal_screen_loc = -1;
	GLint pal_remap_tex_loc = -1;
	GLint pal_remap_tex1_loc = -1;
	GLint pal_palette_tex_loc = -1;

	/* Solid debug program uniforms. */
	GLint solid_screen_loc = -1;
	GLint solid_colour_loc = -1;

	GLuint palette_tex = 0;      ///< 256x1 RGBA palette texture.
	GLuint remap_table_tex[2] = {0, 0}; ///< Double-buffered 256x1 remap table textures.
	int remap_table_idx = 0;            ///< Current remap table texture index (0 or 1).
	const uint8_t *last_remap_ptr = nullptr; ///< Last uploaded remap table pointer (cache).
	GLuint vbo = 0;              ///< Vertex buffer for batched quads.

	GLuint fbo = 0;              ///< Persistent framebuffer object for accumulation.
	GLuint fbo_tex = 0;          ///< Colour attachment for the FBO.

	int screen_width = 0;
	int screen_height = 0;

	GLESSpriteAtlas sprite_atlas; ///< Sprite atlas manager.

	std::vector<GLESDrawCommand> draw_queue; ///< Pending draw commands.
	std::vector<GLESVertex> vertex_buf;      ///< Temporary vertex assembly buffer.
	std::vector<Rect> dirty_rects;           ///< Dirty regions to clear in FBO before drawing.

	GLESBackend();
	~GLESBackend();

	bool InitShaders();
	GLuint CompileShader(GLenum type, const char *source);
	GLuint LinkProgram(GLuint vert, GLuint frag);

public:
	static GLESBackend *Get() { return instance; }
	static bool Create();
	static void Destroy();

	void Resize(int w, int h);
	void UpdatePalette(const Colour *pal, uint first, uint length);

	/** Get the sprite atlas for encoding sprites. */
	GLESSpriteAtlas &GetSpriteAtlas() { return sprite_atlas; }

	/** Add a draw command to the queue. Called from the GLES blitter's Draw(). */
	void QueueDraw(const GLESDrawCommand &cmd);

	/** Add a dirty rectangle that needs clearing in the FBO before drawing. */
	void AddDirtyRect(int left, int top, int right, int bottom);

	/** Flush all queued draw commands as batched GL draw calls. */
	void Paint();

	/** Recover all GPU state after EGL context loss (SDL_RENDER_DEVICE_RESET).
	 *  Old GL handles are silently abandoned (freed by OS when context is destroyed).
	 *  New objects are created in the replacement context. Sprites are reloaded
	 *  from scratch via a map reload triggered by _switch_mode. */
	void RecoverGPUState();

	/** Clear the draw queue without rendering. */
	void ClearQueue() { draw_queue.clear(); }

	/** Get the current draw queue size. */
	size_t GetDrawQueueSize() const { return draw_queue.size(); }

	int GetScreenWidth() const { return screen_width; }
	int GetScreenHeight() const { return screen_height; }
};

#endif /* VIDEO_GLES_BACKEND_H */
