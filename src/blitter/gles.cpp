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
 * Uploads the sprite to the GPU atlas and returns a minimal Sprite struct
 * (dimensions only, no pixel data needed on CPU side).
 */
Sprite *Blitter_GLES::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) {
		/* Fall back to parent encoder if backend not ready yet. */
		return Blitter_32bppOptimized::Encode(sprite_type, sprite, allocator);
	}

	/* Allocate a minimal Sprite struct. */
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));

	/* Use the normal zoom level as the representative. */
	const SpriteLoader::Sprite &root = sprite[ZoomLevel::Normal];
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	/* Upload each available zoom level to the atlas. */
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
 * Override Draw to record a draw command instead of CPU blitting.
 * Falls back to the parent CPU blitter for non-sprite operations.
 */
void Blitter_GLES::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) {
		/* No backend — fall back to CPU. */
		Blitter_32bppOptimized::Draw(bp, mode, zoom);
		return;
	}

	/* For remap modes that require a per-draw remap table upload, fall back to CPU.
	 * Normal and Transparent modes are the primary targets for GPU acceleration
	 * (they cover landscape tiles which dominate the title screen wallpaper). */
	if (mode == BlitterMode::ColourRemap || mode == BlitterMode::TransparentRemap ||
	    mode == BlitterMode::CrashRemap || mode == BlitterMode::BlackRemap) {
		Blitter_32bppOptimized::Draw(bp, mode, zoom);
		return;
	}

	/* Build the sprite key from the sprite data pointer and zoom level. */
	GLESSpriteID key = MakeGLESSpriteKey(bp->sprite, zoom);

	/* Check if this sprite is in the atlas. If not, fall back to CPU. */
	const GLESSpriteEntry *entry = backend->GetSpriteAtlas().Lookup(key);
	if (entry == nullptr) {
		Blitter_32bppOptimized::Draw(bp, mode, zoom);
		return;
	}

	/* Compute absolute screen position.
	 * bp->dst points into the screen buffer, bp->left/top are offsets within that.
	 * We need the pixel offset from _screen.dst_ptr to get absolute screen coords. */
	ptrdiff_t pixel_offset = static_cast<const uint32_t *>(bp->dst) -
	                          static_cast<const uint32_t *>(_screen.dst_ptr);
	int base_x = static_cast<int>(pixel_offset % _screen.pitch);
	int base_y = static_cast<int>(pixel_offset / _screen.pitch);

	/* Record draw command. */
	GLESDrawCommand cmd;
	cmd.sprite_key = key;
	cmd.screen_x = static_cast<int16_t>(base_x + bp->left);
	cmd.screen_y = static_cast<int16_t>(base_y + bp->top);
	cmd.width = static_cast<int16_t>(bp->width);
	cmd.height = static_cast<int16_t>(bp->height);
	cmd.skip_left = static_cast<int16_t>(bp->skip_left);
	cmd.skip_top = static_cast<int16_t>(bp->skip_top);
	cmd.sprite_width = static_cast<int16_t>(UnScaleByZoom(bp->sprite_width, zoom));
	cmd.sprite_height = static_cast<int16_t>(UnScaleByZoom(bp->sprite_height, zoom));
	cmd.zoom = zoom;
	cmd.mode = mode;
	cmd.remap_idx = 0;

	backend->QueueDraw(cmd);
}
