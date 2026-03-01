/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file 32bpp_anim_neon.cpp Implementation of a NEON 32bpp blitter with animation support. */

#include "../stdafx.h"
#include "../palette_func.h"
#include "../video/video_driver.hpp"
#include "32bpp_anim_neon.hpp"
#include "32bpp_neon_func.hpp"

#include <arm_neon.h>

#include "../safeguards.h"

/** Instantiation of the NEON 32bpp with animation blitter factory. */
static FBlitter_32bppNEON_Anim iFBlitter_32bppNEON_Anim;

template <BlitterMode mode>
void Blitter_32bppNEON_Anim::Draw(const Blitter::BlitterParams *bp, ZoomLevel zoom)
{
	const Blitter_32bppOptimized::SpriteData *src = (const Blitter_32bppOptimized::SpriteData *)bp->sprite;

	const Colour *src_px = reinterpret_cast<const Colour *>(src->data + src->offset[0][zoom]);
	const uint16_t *src_n = reinterpret_cast<const uint16_t *>(src->data + src->offset[1][zoom]);

	for (uint i = bp->skip_top; i != 0; i--) {
		src_px = (const Colour *)((const uint8_t *)src_px + *(const uint32_t *)src_px);
		src_n  = (const uint16_t *)((const uint8_t *)src_n  + *(const uint32_t *)src_n);
	}

	Colour *dst = (Colour *)bp->dst + bp->top * bp->pitch + bp->left;
	uint16_t *anim = this->anim_buf + this->ScreenToAnimOffset((uint32_t *)bp->dst) + bp->top * this->anim_buf_pitch + bp->left;

	const uint8_t *remap = bp->remap;

	for (int y = 0; y < bp->height; y++) {
		Colour *dst_ln = dst + bp->pitch;
		uint16_t *anim_ln = anim + this->anim_buf_pitch;

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

				if (dst > dst_end) anim += dst - dst_end;
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
				anim += n;
				dst += n;
				src_px++;
				src_n++;
				continue;
			}

			draw:;

			switch (mode) {
				case BlitterMode::Normal:
					if (src_px->a == 255) {
						/* Opaque Normal — hottest path. Process 4 pixels at a time with NEON. */
						while (n >= 4) {
							/* Load 4 src_n values and check for animated palette entries. */
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));
							uint16x4_t anim_check = vcge_u16(m_vec, vdup_n_u16(PALETTE_ANIM_START));

							if (vget_lane_u64(vreinterpret_u64_u16(anim_check), 0) == 0) {
								/* No animated pixels — NEON bulk copy. */
								vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
								vst1_u16(anim, n_vec);
							} else {
								/* Animated pixel(s) — scalar fallback for this batch. */
								for (uint i = 0; i < 4; i++) {
									uint m = GB(src_n[i], 0, 8);
									anim[i] = src_n[i];
									dst[i].data = (m >= PALETTE_ANIM_START) ? AdjustBrightness(this->LookupColourInPalette(m), GB(src_n[i], 8, 8)).data : src_px[i].data;
								}
							}
							dst += 4;
							anim += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						/* Scalar tail for remaining 0-3 pixels. */
						while (n != 0) {
							uint m = GB(*src_n, 0, 8);
							*anim++ = *src_n;
							*dst++ = (m >= PALETTE_ANIM_START) ? AdjustBrightness(this->LookupColourInPalette(m), GB(*src_n, 8, 8)) : src_px->data;
							src_px++;
							src_n++;
							n--;
						}
					} else {
						/* Semi-transparent Normal. Process 4 pixels at a time with NEON alpha blend. */
						while (n >= 4) {
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));
							uint16x4_t anim_check = vcge_u16(m_vec, vdup_n_u16(PALETTE_ANIM_START));

							if (vget_lane_u64(vreinterpret_u64_u16(anim_check), 0) == 0) {
								/* No animated pixels — NEON alpha blend. */
								uint8x16_t s = vld1q_u8((const uint8_t *)src_px);
								uint8x16_t d = vld1q_u8((const uint8_t *)dst);
								vst1q_u8((uint8_t *)dst, NeonAlphaBlend4Pixels(s, d));
								vst1_u16(anim, vdup_n_u16(0));
							} else {
								/* Animated pixel(s) — scalar fallback. */
								for (uint i = 0; i < 4; i++) {
									uint m = GB(src_n[i], 0, 8);
									anim[i] = 0;
									if (m >= PALETTE_ANIM_START) {
										dst[i].data = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(m), GB(src_n[i], 8, 8)), src_px[i].a, dst[i]).data;
									} else {
										dst[i].data = ComposeColourRGBANoCheck(src_px[i].r, src_px[i].g, src_px[i].b, src_px[i].a, dst[i]).data;
									}
								}
							}
							dst += 4;
							anim += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						/* Scalar tail. */
						while (n != 0) {
							uint m = GB(*src_n, 0, 8);
							*anim++ = 0;
							if (m >= PALETTE_ANIM_START) {
								*dst = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(m), GB(*src_n, 8, 8)), src_px->a, *dst);
							} else {
								*dst = ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
							}
							dst++;
							src_px++;
							src_n++;
							n--;
						}
					}
					break;

				case BlitterMode::ColourRemap:
					if (src_px->a == 255) {
						/* Opaque ColourRemap. NEON fast path when all m == 0 (no remap). */
						while (n >= 4) {
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));

							if (vget_lane_u64(vreinterpret_u64_u16(m_vec), 0) == 0) {
								/* All m == 0: no remap needed — bulk copy. */
								vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
								vst1_u16(anim, vdup_n_u16(0));
							} else {
								/* Some pixels need remap — scalar fallback. */
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i] = src_px[i].data;
										anim[i] = 0;
									} else {
										uint r = remap[m];
										anim[i] = r | (src_n[i] & 0xFF00);
										if (r != 0) dst[i] = AdjustBrightness(this->LookupColourInPalette(r), GB(src_n[i], 8, 8));
									}
								}
							}
							dst += 4;
							anim += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						/* Scalar tail. */
						while (n != 0) {
							uint m = *src_n & 0xFF;
							if (m == 0) {
								*dst = src_px->data;
								*anim = 0;
							} else {
								uint r = remap[m];
								*anim = r | (*src_n & 0xFF00);
								if (r != 0) *dst = AdjustBrightness(this->LookupColourInPalette(r), GB(*src_n, 8, 8));
							}
							anim++;
							dst++;
							src_px++;
							src_n++;
							n--;
						}
					} else {
						/* Semi-transparent ColourRemap. NEON fast path when all m == 0. */
						while (n >= 4) {
							uint16x4_t n_vec = vld1_u16(src_n);
							uint16x4_t m_vec = vand_u16(n_vec, vdup_n_u16(0xFF));

							if (vget_lane_u64(vreinterpret_u64_u16(m_vec), 0) == 0) {
								/* All m == 0: no remap — NEON alpha blend. */
								uint8x16_t s = vld1q_u8((const uint8_t *)src_px);
								uint8x16_t d = vld1q_u8((const uint8_t *)dst);
								vst1q_u8((uint8_t *)dst, NeonAlphaBlend4Pixels(s, d));
								vst1_u16(anim, vdup_n_u16(0));
							} else {
								/* Some pixels need remap — scalar fallback. */
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i].data = ComposeColourRGBANoCheck(src_px[i].r, src_px[i].g, src_px[i].b, src_px[i].a, dst[i]).data;
										anim[i] = 0;
									} else {
										uint r = remap[m];
										anim[i] = 0;
										if (r != 0) dst[i].data = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(r), GB(src_n[i], 8, 8)), src_px[i].a, dst[i]).data;
									}
								}
							}
							dst += 4;
							anim += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						/* Scalar tail. */
						while (n != 0) {
							uint m = *src_n & 0xFF;
							if (m == 0) {
								*dst = ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
								*anim = 0;
							} else {
								uint r = remap[m];
								*anim = 0;
								if (r != 0) *dst = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(r), GB(*src_n, 8, 8)), src_px->a, *dst);
							}
							anim++;
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

		anim = anim_ln;
		dst = dst_ln;
		src_px = src_px_ln;
		src_n  = src_n_ln;
	}
}

/**
 * NEON-optimized Draw for the non-animation path (_screen_disable_anim == true).
 * Equivalent to Blitter_32bppOptimized::Draw<mode, false> but with NEON inner loops.
 * No anim buffer, no palette-to-RGB conversion (Tpal_to_rgb=false).
 */
template <BlitterMode mode>
void Blitter_32bppNEON_Anim::DrawNoAnim(const Blitter::BlitterParams *bp, ZoomLevel zoom)
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
						/* Opaque Normal — unconditional NEON bulk copy (Tpal_to_rgb=false). */
						while (n >= 4) {
							vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
							dst += 4;
							src_px += 4;
							src_n += 4;
							n -= 4;
						}
						while (n != 0) {
							*dst++ = src_px->data;
							dst[-1].a = 0xFF; /* ensure opaque output */
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
							*dst = ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
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
								/* All m == 0: no remap — bulk copy. */
								vst1q_u32((uint32_t *)dst, vld1q_u32((const uint32_t *)src_px));
							} else {
								/* Some pixels need remap — scalar. */
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i] = src_px[i].data;
									} else {
										uint r = remap[m];
										if (r != 0) dst[i] = AdjustBrightness(this->LookupColourInPalette(r), GB(src_n[i], 8, 8));
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
								if (r != 0) *dst = AdjustBrightness(this->LookupColourInPalette(r), GB(*src_n, 8, 8));
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
								/* All m == 0: no remap — NEON alpha blend. */
								uint8x16_t s = vld1q_u8((const uint8_t *)src_px);
								uint8x16_t d = vld1q_u8((const uint8_t *)dst);
								vst1q_u8((uint8_t *)dst, NeonAlphaBlend4Pixels(s, d));
							} else {
								/* Some pixels need remap — scalar. */
								for (uint i = 0; i < 4; i++) {
									uint m = src_n[i] & 0xFF;
									if (m == 0) {
										dst[i].data = ComposeColourRGBANoCheck(src_px[i].r, src_px[i].g, src_px[i].b, src_px[i].a, dst[i]).data;
									} else {
										uint r = remap[m];
										if (r != 0) dst[i].data = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(r), GB(src_n[i], 8, 8)), src_px[i].a, dst[i]).data;
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
								*dst = ComposeColourRGBANoCheck(src_px->r, src_px->g, src_px->b, src_px->a, *dst);
							} else {
								uint r = remap[m];
								if (r != 0) *dst = ComposeColourPANoCheck(AdjustBrightness(this->LookupColourInPalette(r), GB(*src_n, 8, 8)), src_px->a, *dst);
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

void Blitter_32bppNEON_Anim::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	if (_screen_disable_anim) {
		switch (mode) {
			case BlitterMode::Normal:      DrawNoAnim<BlitterMode::Normal>(bp, zoom); return;
			case BlitterMode::ColourRemap: DrawNoAnim<BlitterMode::ColourRemap>(bp, zoom); return;
			default: Blitter_32bppOptimized::Draw(bp, mode, zoom); return;
		}
	}

	switch (mode) {
		case BlitterMode::Normal:      Draw<BlitterMode::Normal>(bp, zoom); return;
		case BlitterMode::ColourRemap: Draw<BlitterMode::ColourRemap>(bp, zoom); return;
		default: Blitter_32bppAnim::Draw(bp, mode, zoom); return;
	}
}

template void Blitter_32bppNEON_Anim::Draw<BlitterMode::Normal>(const Blitter::BlitterParams *, ZoomLevel);
template void Blitter_32bppNEON_Anim::Draw<BlitterMode::ColourRemap>(const Blitter::BlitterParams *, ZoomLevel);
template void Blitter_32bppNEON_Anim::DrawNoAnim<BlitterMode::Normal>(const Blitter::BlitterParams *, ZoomLevel);
template void Blitter_32bppNEON_Anim::DrawNoAnim<BlitterMode::ColourRemap>(const Blitter::BlitterParams *, ZoomLevel);

void Blitter_32bppNEON_Anim::PaletteAnimate(const Palette &palette)
{
	assert(!_screen_disable_anim);

	this->palette = palette;
	/* If first_dirty is 0, it is for 8bpp indication to send the new
	 *  palette. However, only the animation colours might possibly change.
	 *  Especially when going between toyland and non-toyland. */
	assert(this->palette.first_dirty == PALETTE_ANIM_START || this->palette.first_dirty == 0);

	const uint16_t *anim = this->anim_buf;
	Colour *dst = (Colour *)_screen.dst_ptr;

	bool screen_dirty = false;

	const int width = this->anim_buf_width;
	const int screen_pitch = _screen.pitch;
	const int anim_pitch = this->anim_buf_pitch;
	const uint16x8_t anim_cmp = vdupq_n_u16(PALETTE_ANIM_START - 1);
	const uint16x8_t brightness_cmp = vdupq_n_u16(DEFAULT_BRIGHTNESS);
	const uint16x8_t colour_mask = vdupq_n_u16(0xFF);

	for (int y = this->anim_buf_height; y != 0; y--) {
		Colour *next_dst_ln = dst + screen_pitch;
		const uint16_t *next_anim_ln = anim + anim_pitch;
		int x = width;
		while (x > 0) {
			uint16x8_t data = vld1q_u16(anim);

			/* Low bytes only — the palette colour index. */
			uint16x8_t colour_data = vandq_u16(data, colour_mask);

			/* Test if any colour >= PALETTE_ANIM_START (unsigned compare: colour > PALETTE_ANIM_START - 1). */
			uint16x8_t cmp_result = vcgtq_u16(colour_data, anim_cmp);
			if (vmaxvq_u16(cmp_result) != 0) {
				/* At least one pixel is animated. */
				if (x < 8 || vminvq_u16(cmp_result) == 0 ||
						vminvq_u16(vceqq_u16(vshrq_n_u16(data, 8), brightness_cmp)) == 0) {
					/* Slow path: < 8 pixels left, not all animated, or unexpected brightnesses. */
					for (int z = std::min<int>(x, 8); z != 0; z--) {
						uint16_t value = vgetq_lane_u16(data, 0);
						uint8_t colour = GB(value, 0, 8);
						if (colour >= PALETTE_ANIM_START) {
							*dst = AdjustBrightness(LookupColourInPalette(colour), GB(value, 8, 8));
							screen_dirty = true;
						}
						data = vextq_u16(data, vdupq_n_u16(0), 1);
						dst++;
					}
				} else {
					/* Medium path: all 8 pixels animated with default brightness. */
					for (int z = 0; z < 8; z++) {
						*dst = LookupColourInPalette(vgetq_lane_u16(colour_data, 0));
						colour_data = vextq_u16(colour_data, vdupq_n_u16(0), 1);
						dst++;
					}
					screen_dirty = true;
				}
			} else {
				/* Fast path, no animation. */
				dst += 8;
			}
			anim += 8;
			x -= 8;
		}
		dst = next_dst_ln;
		anim = next_anim_ln;
	}

	if (screen_dirty) {
		/* Make sure the backend redraws the whole screen. */
		VideoDriver::GetInstance()->MakeDirty(0, 0, _screen.width, _screen.height);
	}
}
