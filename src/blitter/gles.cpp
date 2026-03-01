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
#include "../debug.h"

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

/**
 * Draw override for the GLES blitter.
 * Currently delegates entirely to the CPU blitter. The FBO pipeline in
 * GLESBackend::Paint() handles uploading the CPU buffer to the screen,
 * solving double-buffering / dirty-rect incompatibilities.
 *
 * GPU sprite overlay is disabled for now because it causes artifacts:
 * - Semi-transparent sprites get double-blended (CPU + GPU)
 * - Palette-animated sprites (water) show stale RGBA from encode time
 * The atlas infrastructure is kept for future use when GPU rendering
 * can fully replace CPU rendering for supported sprite types.
 */
void Blitter_GLES::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	Blitter_32bppOptimized::Draw(bp, mode, zoom);
}
