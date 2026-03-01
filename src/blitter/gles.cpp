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

#include "../safeguards.h"

static FBlitter_GLES iFBlitter_GLES;

/**
 * Encode a sprite for the GLES blitter.
 * Calls the parent encoder to keep full CPU pixel data (for fallback),
 * then also uploads to the GPU atlas for accelerated rendering.
 */
Sprite *Blitter_GLES::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	/* Always call parent encoder first to get full CPU sprite data.
	 * This ensures we can fall back to CPU rendering for unsupported modes. */
	Sprite *dest_sprite = Blitter_32bppOptimized::Encode(sprite_type, sprite, allocator);

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return dest_sprite;

	/* Also upload each available zoom level to the GPU atlas. */
	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	for (int z = to_underlying(ZoomLevel::Begin); z < to_underlying(ZoomLevel::End); z++) {
		ZoomLevel zoom = static_cast<ZoomLevel>(z);
		const SpriteLoader::Sprite &src = sprite[zoom];
		if (src.data == nullptr || src.width == 0 || src.height == 0) continue;

		bool has_rgb = src.colours.Test(SpriteComponent::RGB) || src.colours.Test(SpriteComponent::Alpha);
		bool has_remap = src.colours.Test(SpriteComponent::Palette);

		atlas.Upload(dest_sprite->data, zoom, src.data,
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

/**
 * Draw override for the GLES blitter.
 * Uses NEON-optimized inner loops for Normal and ColourRemap on ARM64.
 * Falls back to CPU blitter for other modes.
 */
void Blitter_GLES::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
#ifdef WITH_NEON
	switch (mode) {
		case BlitterMode::Normal:      DrawNeon<BlitterMode::Normal>(bp, zoom); return;
		case BlitterMode::ColourRemap: DrawNeon<BlitterMode::ColourRemap>(bp, zoom); return;
		default: break;
	}
#endif
	Blitter_32bppOptimized::Draw(bp, mode, zoom);
}
