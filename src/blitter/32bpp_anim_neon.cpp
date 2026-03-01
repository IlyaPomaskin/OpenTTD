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

#include <arm_neon.h>

#include "../safeguards.h"

/** Instantiation of the NEON 32bpp with animation blitter factory. */
static FBlitter_32bppNEON_Anim iFBlitter_32bppNEON_Anim;

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
