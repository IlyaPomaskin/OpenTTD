/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_shader.h OpenGL ES shader programs for batched sprite rendering. */

/** Vertex shader (GLSL ES 100) for batched sprite rendering.
 *  Each vertex carries position (pixel coords), two sets of UVs
 *  (colour_uv for RGBA atlas, remap_uv for M channel atlas),
 *  and page indices to select which atlas texture to sample. */
static const char *_gles_vertex_shader =
	"precision mediump float;\n"
	"uniform vec2 screen;\n"
	"attribute vec2 a_position;\n"
	"attribute vec2 a_colour_uv;\n"
	"attribute vec2 a_remap_uv;\n"
	"attribute float a_cpage;\n"
	"attribute float a_rpage;\n"
	"varying vec2 v_colour_uv;\n"
	"varying vec2 v_remap_uv;\n"
	"varying float v_cpage;\n"
	"varying float v_rpage;\n"
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
 *  Selects between two atlas pages based on v_cpage. */
static const char *_gles_frag_shader_normal =
	"precision mediump float;\n"
	"uniform sampler2D colour_tex;\n"
	"uniform sampler2D colour_tex1;\n"
	"varying vec2 v_colour_uv;\n"
	"varying float v_cpage;\n"
	"void main() {\n"
	"  if (v_cpage < 0.5)\n"
	"    gl_FragColor = texture2D(colour_tex, v_colour_uv);\n"
	"  else\n"
	"    gl_FragColor = texture2D(colour_tex1, v_colour_uv);\n"
	"}\n";

/** Fragment shader for colour-remapped sprite rendering.
 *  Reads the M channel from the remap atlas, looks up the remap table
 *  to get the final palette index, then looks up the palette texture.
 *  Brightness from the RGBA sprite modulates the final colour.
 *  Selects atlas page based on v_cpage / v_rpage. */
static const char *_gles_frag_shader_remap =
	"precision mediump float;\n"
	"uniform sampler2D colour_tex;\n"
	"uniform sampler2D colour_tex1;\n"
	"uniform sampler2D remap_tex;\n"
	"uniform sampler2D remap_tex1;\n"
	"uniform sampler2D palette_tex;\n"
	"uniform sampler2D remap_table_tex;\n"
	"varying vec2 v_colour_uv;\n"
	"varying vec2 v_remap_uv;\n"
	"varying float v_cpage;\n"
	"varying float v_rpage;\n"
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
	"  float m;\n"
	"  if (v_rpage < 0.5)\n"
	"    m = texture2D(remap_tex, v_remap_uv).r;\n"
	"  else\n"
	"    m = texture2D(remap_tex1, v_remap_uv).r;\n"
	"  float remapped = texture2D(remap_table_tex, vec2(m, 0.5)).r;\n"
	"  vec4 pal_col = texture2D(palette_tex, vec2(remapped, 0.5));\n"
	"  vec4 rgb_col;\n"
	"  if (v_cpage < 0.5)\n"
	"    rgb_col = texture2D(colour_tex, v_colour_uv);\n"
	"  else\n"
	"    rgb_col = texture2D(colour_tex1, v_colour_uv);\n"
	"  if (m > 0.0) {\n"
	"    gl_FragColor.a = pal_col.a;\n"
	"    gl_FragColor.rgb = adj_brightness(pal_col.rgb, max3(rgb_col.rgb));\n"
	"  } else {\n"
	"    gl_FragColor = rgb_col;\n"
	"  }\n"
	"}\n";

/** Fragment shader for palette-only sprite rendering.
 *  Reads the M channel index from the remap atlas, looks up the palette
 *  texture to get the final RGBA colour. Index 0 is transparent (discarded).
 *  Selects atlas page based on v_rpage. */
static const char *_gles_frag_shader_palette =
	"precision mediump float;\n"
	"uniform sampler2D remap_tex;\n"
	"uniform sampler2D remap_tex1;\n"
	"uniform sampler2D palette_tex;\n"
	"varying vec2 v_remap_uv;\n"
	"varying float v_rpage;\n"
	"void main() {\n"
	"  float m;\n"
	"  if (v_rpage < 0.5)\n"
	"    m = texture2D(remap_tex, v_remap_uv).r;\n"
	"  else\n"
	"    m = texture2D(remap_tex1, v_remap_uv).r;\n"
	"  if (m < 0.002) discard;\n"
	"  vec4 col = texture2D(palette_tex, vec2(m, 0.5));\n"
	"  gl_FragColor = vec4(col.rgb, 1.0);\n"
	"}\n";

/** Fragment shader for debug: outputs a solid colour (no texture). */
static const char *_gles_frag_shader_solid =
	"precision mediump float;\n"
	"uniform vec4 u_colour;\n"
	"void main() {\n"
	"  gl_FragColor = u_colour;\n"
	"}\n";

/** Fragment shader for transparent sprite rendering.
 *  Outputs black with alpha, used with glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA).
 *  Selects atlas page based on v_cpage. */
static const char *_gles_frag_shader_transparent =
	"precision mediump float;\n"
	"uniform sampler2D colour_tex;\n"
	"uniform sampler2D colour_tex1;\n"
	"varying vec2 v_colour_uv;\n"
	"varying float v_cpage;\n"
	"void main() {\n"
	"  float a;\n"
	"  if (v_cpage < 0.5)\n"
	"    a = texture2D(colour_tex, v_colour_uv).a;\n"
	"  else\n"
	"    a = texture2D(colour_tex1, v_colour_uv).a;\n"
	"  gl_FragColor = vec4(0.0, 0.0, 0.0, a * 0.5);\n"
	"}\n";
