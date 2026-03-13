/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_shader.h OpenGL ES 3.0 shader programs for batched sprite rendering with MRT. */

/** Vertex shader (GLSL ES 300) for batched sprite rendering.
 *  Each vertex carries position (pixel coords), two sets of UVs
 *  (colour_uv for RGBA atlas, remap_uv for M channel atlas),
 *  and page indices to select which atlas texture to sample. */
static const char *_gles_vertex_shader =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform vec2 screen;\n"
	"in vec2 a_position;\n"
	"in vec2 a_colour_uv;\n"
	"in vec2 a_remap_uv;\n"
	"in float a_cpage;\n"
	"in float a_rpage;\n"
	"out vec2 v_colour_uv;\n"
	"out vec2 v_remap_uv;\n"
	"out float v_cpage;\n"
	"out float v_rpage;\n"
	"void main() {\n"
	"  vec2 ndc = (a_position / screen) * 2.0 - 1.0;\n"
	"  ndc.y = -ndc.y;\n"
	"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
	"  v_colour_uv = a_colour_uv;\n"
	"  v_remap_uv = a_remap_uv;\n"
	"  v_cpage = a_cpage;\n"
	"  v_rpage = a_rpage;\n"
	"}\n";

/** Fragment shader for normal (RGBA) sprite rendering.
 *  Samples colour from a 2D array texture using v_cpage as layer index.
 *  Writes palette index 0 (not palette-dependent). */
static const char *_gles_frag_shader_normal =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray colour_tex;\n"
	"in vec2 v_colour_uv;\n"
	"in float v_cpage;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"layout(location = 1) out vec4 o_index;\n"
	"void main() {\n"
	"  o_colour = texture(colour_tex, vec3(v_colour_uv, v_cpage));\n"
	"  o_index = vec4(0.0);\n"
	"}\n";

/** Fragment shader for colour-remapped sprite rendering.
 *  Reads the M channel from the remap array texture, looks up the remap table
 *  to get the final palette index, then looks up the palette texture.
 *  Brightness from the RGBA sprite modulates the final colour.
 *  Uses v_cpage / v_rpage as array layer indices.
 *  Writes the remapped palette index to attachment 1 for deferred resolve. */
static const char *_gles_frag_shader_remap =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray colour_tex;\n"
	"uniform highp sampler2DArray remap_tex;\n"
	"uniform highp sampler2D palette_tex;\n"
	"uniform highp sampler2D remap_table_tex;\n"
	"in vec2 v_colour_uv;\n"
	"in vec2 v_remap_uv;\n"
	"in float v_cpage;\n"
	"in float v_rpage;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"layout(location = 1) out vec4 o_index;\n"
	"\n"
	"float max3(vec3 v) {\n"
	"  return max(max(v.x, v.y), v.z);\n"
	"}\n"
	"\n"
	"vec3 adj_brightness(vec3 colour, float brightness) {\n"
	"  vec3 adj = colour * (brightness > 0.0 ? brightness / 0.5 : 1.0);\n"
	"  vec3 ob_vec = clamp(adj - 1.0, 0.0, 1.0);\n"
	"  float ob = (ob_vec.r + ob_vec.g + ob_vec.b) / 2.0;\n"
	"  return clamp(adj + ob * (1.0 - adj), 0.0, 1.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  float m = texture(remap_tex, vec3(v_remap_uv, v_rpage)).r;\n"
	"  float remapped = texture(remap_table_tex, vec2((m * 255.0 + 0.5) / 256.0, 0.5)).r;\n"
	"  vec4 pal_col = texture(palette_tex, vec2((remapped * 255.0 + 0.5) / 256.0, 0.5));\n"
	"  vec4 rgb_col = texture(colour_tex, vec3(v_colour_uv, v_cpage));\n"
	"  if (m > 0.0) {\n"
	"    o_colour.a = pal_col.a;\n"
	"    o_colour.rgb = adj_brightness(pal_col.rgb, max3(rgb_col.rgb));\n"
	"    o_index = vec4(remapped, 0.0, 0.0, 0.0);\n"
	"  } else {\n"
	"    o_colour = rgb_col;\n"
	"    o_index = vec4(0.0);\n"
	"  }\n"
	"}\n";

/** Fragment shader for palette-only sprite rendering.
 *  Reads the M channel index from the remap array texture, looks up the palette
 *  texture to get the final RGBA colour. Index 0 is transparent (discarded).
 *  Uses v_rpage as array layer index.
 *  Writes the palette index to attachment 1 for deferred resolve. */
static const char *_gles_frag_shader_palette =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray remap_tex;\n"
	"uniform highp sampler2D palette_tex;\n"
	"in vec2 v_remap_uv;\n"
	"in float v_rpage;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"layout(location = 1) out vec4 o_index;\n"
	"void main() {\n"
	"  float m = texture(remap_tex, vec3(v_remap_uv, v_rpage)).r;\n"
	"  if (m < 0.002) discard;\n"
	"  vec4 col = texture(palette_tex, vec2((m * 255.0 + 0.5) / 256.0, 0.5));\n"
	"  o_colour = vec4(col.rgb, 1.0);\n"
	"  o_index = vec4(m, 0.0, 0.0, 0.0);\n"
	"}\n";

/** Fragment shader for debug: outputs a solid colour (no texture).
 *  Writes palette index 0. */
static const char *_gles_frag_shader_solid =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform vec4 u_colour;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"layout(location = 1) out vec4 o_index;\n"
	"void main() {\n"
	"  o_colour = u_colour;\n"
	"  o_index = vec4(0.0);\n"
	"}\n";

/** Fragment shader for transparent sprite rendering.
 *  Outputs black with alpha, used with glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA).
 *  Uses v_cpage as array layer index.
 *  Writes palette index 0 (not palette-dependent). */
static const char *_gles_frag_shader_transparent =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray colour_tex;\n"
	"in vec2 v_colour_uv;\n"
	"in float v_cpage;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"layout(location = 1) out vec4 o_index;\n"
	"void main() {\n"
	"  float a = texture(colour_tex, vec3(v_colour_uv, v_cpage)).a;\n"
	"  o_colour = vec4(0.0, 0.0, 0.0, a * 0.5);\n"
	"  o_index = vec4(0.0);\n"
	"}\n";

/** Simple FBO blit fragment shader — single output, no MRT. */
static const char *_gles_frag_shader_blit =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_tex;\n"
	"in vec2 v_colour_uv;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"void main() {\n"
	"  o_colour = texture(u_tex, v_colour_uv);\n"
	"}\n";

/** Fragment shader for palette resolve pass.
 *  Reads the palette index from the FBO index attachment, looks up the
 *  current palette texture, and writes the resolved colour to attachment 0.
 *  Non-palette pixels (index < 0.002) are discarded to preserve existing colour. */
static const char *_gles_frag_shader_resolve =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2D idx_tex;\n"
	"uniform highp sampler2D palette_tex;\n"
	"in vec2 v_colour_uv;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"void main() {\n"
	"  float idx = texture(idx_tex, v_colour_uv).r;\n"
	"  if (idx < 0.002) discard;\n"
	"  vec4 col = texture(palette_tex, vec2((idx * 255.0 + 0.5) / 256.0, 0.5));\n"
	"  o_colour = vec4(col.rgb, 1.0);\n"
	"}\n";
