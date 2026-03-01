/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file 32bpp_anim_neon.hpp A NEON 32 bpp blitter with animation support. */

#ifndef BLITTER_32BPP_NEON_ANIM_HPP
#define BLITTER_32BPP_NEON_ANIM_HPP

#include "32bpp_anim.hpp"

/** A 32 bpp blitter with palette animation using ARM NEON. */
class Blitter_32bppNEON_Anim : public Blitter_32bppAnim {
public:
	void PaletteAnimate(const Palette &palette) override;
	std::string_view GetName() override { return "32bpp-neon-anim"; }
};

/** Factory for the NEON 32bpp blitter with animation. */
class FBlitter_32bppNEON_Anim : public BlitterFactory {
public:
	/* NEON is mandatory on AArch64, no CPUID check needed. */
	FBlitter_32bppNEON_Anim() : BlitterFactory("32bpp-neon-anim", "32bpp ARM NEON Animation Blitter (palette animation)") {}
	std::unique_ptr<Blitter> CreateInstance() override { return std::make_unique<Blitter_32bppNEON_Anim>(); }
};

#endif /* BLITTER_32BPP_NEON_ANIM_HPP */
