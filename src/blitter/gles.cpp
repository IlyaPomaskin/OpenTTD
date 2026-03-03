/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles.cpp OpenGL ES blitter implementation. */

#include "../stdafx.h"
#include "gles.hpp"
#include "../video/gles_backend.h"
#include "../video/gles_sprite.h"
#include "../gfx_func.h"
#include "../zoom_func.h"
#include "../palette_func.h"
#include "../debug.h"

#ifdef WITH_NEON
#include "32bpp_neon_func.hpp"
#endif

#include <unordered_set>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "../safeguards.h"

/** Write sprite pixel data as PPM file (RGB, transparent pixels as magenta). */
static void DumpSpritePPM(SpriteID id, int zoom, const SpriteLoader::CommonPixel *pixels,
                          int w, int h, bool has_rgb, bool has_remap)
{
	const char *flags = (has_rgb && has_remap) ? "both" :
	                    has_rgb ? "rgb" :
	                    has_remap ? "pal" : "none";

	/* Write RGB image. */
	{
		std::ostringstream ss;
		ss << "/data/data/org.openttd.android/files/sprites/" << std::setfill('0') << std::setw(6) << id
		   << "_z" << zoom << "_" << w << "x" << h << "_" << flags << ".ppm";
		std::ofstream f(ss.str(), std::ios::binary);
		if (!f) return;
		f << "P6\n" << w << " " << h << "\n255\n";
		for (int i = 0; i < w * h; i++) {
			uint8_t rgb[3];
			if (pixels[i].a > 0) {
				rgb[0] = pixels[i].r;
				rgb[1] = pixels[i].g;
				rgb[2] = pixels[i].b;
			} else {
				rgb[0] = 255; rgb[1] = 0; rgb[2] = 255; /* magenta = transparent */
			}
			f.write(reinterpret_cast<char *>(rgb), 3);
		}
	}

	/* Also write M channel if remap data exists. */
	if (has_remap) {
		std::ostringstream ss;
		ss << "/data/data/org.openttd.android/files/sprites/" << std::setfill('0') << std::setw(6) << id
		   << "_z" << zoom << "_" << w << "x" << h << "_M.ppm";
		std::ofstream f(ss.str(), std::ios::binary);
		if (!f) return;
		f << "P6\n" << w << " " << h << "\n255\n";
		for (int i = 0; i < w * h; i++) {
			uint8_t v = pixels[i].m;
			uint8_t rgb[3] = {v, v, v};
			f.write(reinterpret_cast<char *>(rgb), 3);
		}
	}
}

static FBlitter_GLES iFBlitter_GLES;

/**
 * Encode a sprite for the GLES blitter.
 * Allocates a minimal Sprite (dimensions only) and uploads pixel data to the GPU atlas.
 * No CPU-side RLE encoding — all rendering goes through the GPU.
 */
Sprite *Blitter_GLES::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	_gles_perf.encode_total++;

	const auto &root = sprite.Root();
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) {
		_gles_perf.encode_skipped++;
		return dest_sprite;
	}

	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	int zooms_uploaded = 0;
	for (int z = to_underlying(ZoomLevel::Begin); z < to_underlying(ZoomLevel::End); z++) {
		ZoomLevel zoom = static_cast<ZoomLevel>(z);
		const SpriteLoader::Sprite &src = sprite[zoom];
		if (src.data == nullptr || src.width == 0 || src.height == 0) continue;

		bool has_rgb = src.colours.Test(SpriteComponent::RGB) || src.colours.Test(SpriteComponent::Alpha);
		bool has_remap = src.colours.Test(SpriteComponent::Palette);

		atlas.Stage(_gles_encoding_sprite_id, zoom, src.data,
		            src.width, src.height, has_rgb, has_remap);
		zooms_uploaded++;
	}
	if (zooms_uploaded > 0) _gles_perf.encode_uploaded++;

	return dest_sprite;
}

/**
 * Encode with CPU fallback.
 * Not used. May be needed in the future for CPU-side font glyph rendering.
 * Calls parent 32bpp RLE encoder (for CPU Draw path) + GPU atlas upload.
 */
Sprite *Blitter_GLES::EncodeCpuFallback(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	Sprite *dest_sprite = Blitter_32bppOptimized::Encode(sprite_type, sprite, allocator);

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return dest_sprite;

	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	for (int z = to_underlying(ZoomLevel::Begin); z < to_underlying(ZoomLevel::End); z++) {
		ZoomLevel zoom = static_cast<ZoomLevel>(z);
		const SpriteLoader::Sprite &src = sprite[zoom];
		if (src.data == nullptr || src.width == 0 || src.height == 0) continue;

		bool has_rgb = src.colours.Test(SpriteComponent::RGB) || src.colours.Test(SpriteComponent::Alpha);
		bool has_remap = src.colours.Test(SpriteComponent::Palette);

		atlas.Upload(_gles_encoding_sprite_id, zoom, src.data,
		             src.width, src.height, has_rgb, has_remap);
	}

	return dest_sprite;
}

#ifdef WITH_NEON
/**
 * NEON-optimized Draw for Normal and ColourRemap modes.
 * Mirrors Blitter_32bppOptimized::Draw<mode, false> with NEON inner loops.
 */
template <BlitterMode mode>
static void DrawNeon(Blitter::BlitterParams *bp, ZoomLevel zoom)
{
	const Blitter_32bppOptimized::SpriteData *src = (const Blitter_32bppOptimized::SpriteData *)bp->sprite;

	const Colour *src_px = reinterpret_cast<const Colour *>(src->data + src->offset[0][zoom]);
	const uint16_t *src_n = reinterpret_cast<const uint16_t *>(src->data + src->offset[1][zoom]);

	for (uint i = bp->skip_top; i != 0; i--) {
		src_px = (const Colour *)((const uint8_t *)src_px + *(const uint32_t *)src_px);
		src_n  = (const uint16_t *)((const uint8_t *)src_n  + *(const uint32_t *)src_n);
	}

	Colour *dst = (Colour *)bp->dst + bp->top * bp->pitch + bp->left;
	const uint8_t *remap = bp->remap;

	for (int y = 0; y < bp->height; y++) {
		Colour *dst_ln = dst + bp->pitch;

		const Colour *src_px_ln = (const Colour *)((const uint8_t *)src_px + *(const uint32_t *)src_px);
		src_px++;

		const uint16_t *src_n_ln = (const uint16_t *)((const uint8_t *)src_n + *(const uint32_t *)src_n);
		src_n += 2;

		Colour *dst_end = dst + bp->skip_left;

		uint n;

		while (dst < dst_end) {
			n = *src_n++;

			if (src_px->a == 0) {
				dst += n;
				src_px++;
				src_n++;
			} else {
				if (dst + n > dst_end) {
					uint d = dst_end - dst;
					src_px += d;
					src_n += d;

					dst = dst_end - bp->skip_left;
					dst_end = dst + bp->width;

					n = std::min(n - d, (uint)bp->width);
					goto draw;
				}
				dst += n;
				src_px += n;
				src_n += n;
			}
		}

		dst -= bp->skip_left;
		dst_end -= bp->skip_left;

		dst_end += bp->width;

		while (dst < dst_end) {
			n = std::min<uint>(*src_n++, dst_end - dst);

			if (src_px->a == 0) {
				dst += n;
				src_px++;
				src_n++;
				continue;
			}

			draw:;

			switch (mode) {
				case BlitterMode::Normal:
					if (src_px->a == 255) {
						/* Opaque Normal — unconditional NEON bulk copy. */
						while (n >= 4) {
							vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
							dst += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						while (n != 0) {
							*dst++ = src_px->data;
							src_px++;
							src_n++;
							n--;
						}
					} else {
						/* Semi-transparent Normal — NEON alpha blend. */
						while (n >= 4) {
							uint8x16_t s = vld1q_u8((const uint8_t *)src_px);
							uint8x16_t d = vld1q_u8((const uint8_t *)dst);
							vst1q_u8((uint8_t *)dst, NeonAlphaBlend4Pixels(s, d));
							dst += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						while (n != 0) {
							*dst = Blitter_32bppBase::ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
							dst++;
							src_px++;
							src_n++;
							n--;
						}
					}
					break;

				case BlitterMode::ColourRemap:
					if (src_px->a == 255) {
						/* Opaque ColourRemap — NEON fast path when all m == 0. */
						while (n >= 4) {
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));

							if (vget_lane_u64(vreinterpret_u64_u16(m_vec), 0) == 0) {
								vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
							} else {
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i] = src_px[i].data;
									} else {
										uint r = remap[m];
										if (r != 0) dst[i] = AdjustBrightness(Blitter_32bppBase::LookupColourInPalette(r), GB(src_n[i], 8, 8));
									}
								}
							}
							dst += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						while (n != 0) {
							uint m = *src_n & 0xFF;
							if (m == 0) {
								*dst = src_px->data;
							} else {
								uint r = remap[m];
								if (r != 0) *dst = AdjustBrightness(Blitter_32bppBase::LookupColourInPalette(r), GB(*src_n, 8, 8));
							}
							dst++;
							src_px++;
							src_n++;
							n--;
						}
					} else {
						/* Semi-transparent ColourRemap — NEON fast path when all m == 0. */
						while (n >= 4) {
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));

							if (vget_lane_u64(vreinterpret_u64_u16(m_vec), 0) == 0) {
								uint8x16_t s = vld1q_u8((const uint8_t *)src_px);
								uint8x16_t d = vld1q_u8((const uint8_t *)dst);
								vst1q_u8((uint8_t *)dst, NeonAlphaBlend4Pixels(s, d));
							} else {
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i].data = Blitter_32bppBase::ComposeColourRGBANoCheck(src_px[i].r, src_px[i].g, src_px[i].b, src_px[i].a, dst[i]).data;
									} else {
										uint r = remap[m];
										if (r != 0) dst[i].data = Blitter_32bppBase::ComposeColourPANoCheck(AdjustBrightness(Blitter_32bppBase::LookupColourInPalette(r), GB(src_n[i], 8, 8)), src_px[i].a, dst[i]).data;
									}
								}
							}
							dst += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						while (n != 0) {
							uint m = *src_n & 0xFF;
							if (m == 0) {
								*dst = Blitter_32bppBase::ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
							} else {
								uint r = remap[m];
								if (r != 0) *dst = Blitter_32bppBase::ComposeColourPANoCheck(AdjustBrightness(Blitter_32bppBase::LookupColourInPalette(r), GB(*src_n, 8, 8)), src_px->a, *dst);
							}
							dst++;
							src_px++;
							src_n++;
							n--;
						}
					}
					break;

				default:
					NOT_REACHED();
			}
		}

		dst = dst_ln;
		src_px = src_px_ln;
		src_n  = src_n_ln;
	}
}

template void DrawNeon<BlitterMode::Normal>(Blitter::BlitterParams *, ZoomLevel);
template void DrawNeon<BlitterMode::ColourRemap>(Blitter::BlitterParams *, ZoomLevel);
#endif /* WITH_NEON */

void Blitter_GLES::DrawRect(void *video, int width, int height, PixelColour colour)
{
	/* Always fill rectangles into the CPU buffer.  Even with GPU sprites,
	 * the CPU buffer is uploaded as the background layer in the FBO —
	 * it provides toolbar, window, and viewport backgrounds that GPU
	 * sprites render on top of. */
	Blitter_32bppBase::DrawRect(video, width, height, colour);
}

/**
 * Draw override for the GLES blitter.
 * When GPU sprites are enabled, queues draw commands for the GPU batch renderer.
 * Otherwise uses NEON-optimized CPU inner loops (ARM64) or scalar fallback.
 */
void Blitter_GLES::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	if (_gles_gpu_sprites) {
		GLESBackend *backend = GLESBackend::Get();
		if (backend == nullptr) return;

		/* Only intercept draws to the actual screen buffer, not screenshots. */
		const uint32_t *screen_start = static_cast<const uint32_t *>(_screen.dst_ptr);
		const uint32_t *screen_end = screen_start + _screen.pitch * _screen.height;
		const uint32_t *dst = static_cast<const uint32_t *>(bp->dst);

		if (dst < screen_start || dst >= screen_end) {
			_gles_perf.gpu_skip_offscreen++;
			return;
		}

		GLESSpriteID key = MakeGLESSpriteKey(bp->sprite_id, zoom);
		GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
		const GLESSpriteEntry *entry = atlas.LookupOrUpload(key);

		int zi = static_cast<int>(zoom);
		if (zi >= 0 && zi < 8) _gles_perf.gpu_zoom_counts[zi]++;

		if (entry == nullptr) {
			_gles_perf.gpu_sprites_missing++;
			static int miss_log_count = 0;
			if (miss_log_count < 50) {
				miss_log_count++;
				Debug(driver, 0, "GLES: Draw miss sprite_id={} zoom={} key={:#x} mode={} skip=({},{}) vis=({},{}) spr=({},{})",
				      bp->sprite_id, static_cast<int>(zoom), key, static_cast<int>(mode),
				      bp->skip_left, bp->skip_top, bp->width, bp->height,
				      bp->sprite_width, bp->sprite_height);
			}
			return;
		}

		/* Log suspicious atlas entries that could render as empty. */
		{
			static int bad_log_count = 0;
			int bad_type = 0; /* 0=ok, 1=zero-dim, 2=zero-vis, 3=cpage>1, 4=rpage>1, 5=clipped */
			int spr_w = static_cast<int>(UnScaleByZoom(bp->sprite_width, zoom));
			int spr_h = static_cast<int>(UnScaleByZoom(bp->sprite_height, zoom));

			if (entry->colour.w == 0 || entry->colour.h == 0) {
				bad_type = 1;
			} else if (bp->width <= 0 || bp->height <= 0) {
				bad_type = 2;
			} else if (entry->colour.atlas_idx > 1) {
				bad_type = 3;
			} else if (entry->has_remap && entry->remap.atlas_idx > 1) {
				bad_type = 4;
			} else if (bp->skip_left >= spr_w || bp->skip_top >= spr_h) {
				bad_type = 5;
			}

			if (bad_type > 0 && bad_log_count < 50) {
				bad_log_count++;
				Debug(driver, 0, "GLES: BAD-DRAW type={} sid={} zoom={} mode={} skip=({},{}) vis=({},{}) spr=({},{}) atlas_c=({},{} p{}) atlas_r=({},{} p{}) remap={} palonly={}",
				      bad_type, bp->sprite_id, static_cast<int>(zoom), static_cast<int>(mode),
				      bp->skip_left, bp->skip_top, bp->width, bp->height,
				      spr_w, spr_h,
				      static_cast<int>(entry->colour.w), static_cast<int>(entry->colour.h), static_cast<int>(entry->colour.atlas_idx),
				      static_cast<int>(entry->remap.w), static_cast<int>(entry->remap.h), static_cast<int>(entry->remap.atlas_idx),
				      static_cast<int>(entry->has_remap), static_cast<int>(entry->palette_only));
			}
		}

		/* Convert buffer-relative coords to absolute screen coords. */
		ptrdiff_t pixel_offset = dst - screen_start;
		int abs_x = static_cast<int>(pixel_offset % _screen.pitch) + bp->left;
		int abs_y = static_cast<int>(pixel_offset / _screen.pitch) + bp->top;

		GLESDrawCommand cmd;
		cmd.sprite_key = key;
		cmd.screen_x = static_cast<int16_t>(abs_x);
		cmd.screen_y = static_cast<int16_t>(abs_y);
		cmd.width = static_cast<int16_t>(bp->width);
		cmd.height = static_cast<int16_t>(bp->height);
		cmd.skip_left = static_cast<int16_t>(bp->skip_left);
		cmd.skip_top = static_cast<int16_t>(bp->skip_top);
		/* Use zoom-adjusted dimensions so UV fractions are correct. */
		cmd.sprite_width = static_cast<int16_t>(UnScaleByZoom(bp->sprite_width, zoom));
		cmd.sprite_height = static_cast<int16_t>(UnScaleByZoom(bp->sprite_height, zoom));
		cmd.zoom = zoom;
		cmd.mode = mode;
		cmd.remap_idx = 0;
		cmd.palette_only = entry->palette_only;
		if (mode == BlitterMode::ColourRemap || mode == BlitterMode::CrashRemap ||
		    mode == BlitterMode::BlackRemap) {
			cmd.remap = bp->remap;
		}
		backend->QueueDraw(cmd);
		return;
	}

	/* CPU fallback (only when _gles_gpu_sprites is off). */
#ifdef WITH_NEON
	switch (mode) {
		case BlitterMode::Normal:      DrawNeon<BlitterMode::Normal>(bp, zoom); return;
		case BlitterMode::ColourRemap: DrawNeon<BlitterMode::ColourRemap>(bp, zoom); return;
		default: break;
	}
#endif
	Blitter_32bppOptimized::Draw(bp, mode, zoom);
}
