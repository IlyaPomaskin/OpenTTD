/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles.hpp OpenGL ES blitter that records draw commands for GPU rendering. */

#ifndef BLITTER_GLES_HPP
#define BLITTER_GLES_HPP

#include "32bpp_optimized.hpp"
#include "../video/video_driver.hpp"

/** The OpenGL ES blitter: records sprite draw commands instead of CPU-blitting. */
class Blitter_GLES : public Blitter_32bppOptimized {
public:
	void Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom) override;
	void DrawRect(void *video, int width, int height, PixelColour colour) override;
	Sprite *Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator) override;

	std::string_view GetName() override { return "gles"; }

	bool Is32BppSupported() override { return true; }
	uint GetSpriteAlignment() override { return 1; }
	Blitter::PaletteAnimation UsePaletteAnimation() override { return Blitter::PaletteAnimation::Blitter; }
};

/** Factory for the OpenGL ES blitter. */
class FBlitter_GLES : public BlitterFactory {
public:
	FBlitter_GLES() : BlitterFactory("gles", "OpenGL ES Blitter (GPU sprite rendering)") {}
	std::unique_ptr<Blitter> CreateInstance() override { return std::make_unique<Blitter_GLES>(); }
};

#endif /* BLITTER_GLES_HPP */
