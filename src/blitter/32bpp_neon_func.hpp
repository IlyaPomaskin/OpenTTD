/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file 32bpp_neon_func.hpp NEON helper functions shared by NEON-accelerated blitters. */

#ifndef BLITTER_32BPP_NEON_FUNC_HPP
#define BLITTER_32BPP_NEON_FUNC_HPP

#include <arm_neon.h>

/**
 * Blend 4 RGBA pixels using per-pixel alpha from src.
 * Computes: result[ch] = ((src[ch] - dst[ch]) * alpha) / 256 + dst[ch]
 * Uses vqdmulhq_s16 to avoid widening to 32-bit.
 * Forces output alpha to 0xFF.
 *
 * Colour layout is ColourBGRA on LE: memory [b,g,r,a] per pixel.
 * Alpha is at byte offsets 3, 7, 11, 15 in a 16-byte vector of 4 pixels.
 */
static inline uint8x16_t NeonAlphaBlend4Pixels(uint8x16_t src, uint8x16_t dst)
{
	/* Broadcast alpha byte to all channels within each pixel. */
	static const uint8_t alpha_shuffle_data[] = {3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15};
	const uint8x16_t alpha_shuffle = vld1q_u8(alpha_shuffle_data);
	uint8x16_t alpha = vqtbl1q_u8(src, alpha_shuffle);

	/* Process low half: pixels 0-1 (8 bytes -> 8 x int16). */
	int16x8_t src_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(src)));
	int16x8_t dst_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(dst)));
	int16x8_t alpha_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(alpha)));
	int16x8_t diff_lo = vsubq_s16(src_lo, dst_lo);
	int16x8_t alpha_shifted_lo = vshlq_n_s16(alpha_lo, 7);
	/* vqdmulhq_s16: sat((2 * a * b) >> 16) = (diff * alpha) / 256 */
	int16x8_t blend_lo = vaddq_s16(vqdmulhq_s16(diff_lo, alpha_shifted_lo), dst_lo);

	/* Process high half: pixels 2-3. */
	int16x8_t src_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(src)));
	int16x8_t dst_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(dst)));
	int16x8_t alpha_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(alpha)));
	int16x8_t diff_hi = vsubq_s16(src_hi, dst_hi);
	int16x8_t alpha_shifted_hi = vshlq_n_s16(alpha_hi, 7);
	int16x8_t blend_hi = vaddq_s16(vqdmulhq_s16(diff_hi, alpha_shifted_hi), dst_hi);

	/* Narrow with unsigned saturation and combine. */
	uint8x16_t result = vcombine_u8(vqmovun_s16(blend_lo), vqmovun_s16(blend_hi));

	/* Force alpha channel to 0xFF. */
	static const uint8_t alpha_mask_data[] = {0,0,0,0xFF, 0,0,0,0xFF, 0,0,0,0xFF, 0,0,0,0xFF};
	result = vorrq_u8(result, vld1q_u8(alpha_mask_data));

	return result;
}

#endif /* BLITTER_32BPP_NEON_FUNC_HPP */
