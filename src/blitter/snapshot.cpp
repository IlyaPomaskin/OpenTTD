/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file snapshot.cpp Recording blitter implementation. */

#include "../stdafx.h"
#include "snapshot.hpp"
#include "../gfx_func.h"
#include "../zoom_func.h"
#include "../video/gles_backend.h"
#include "../video/gles_perf.h"
#include "../table/sprites.h"

#include "../safeguards.h"

/** Register the snapshot blitter factory so it can be selected by name. */
static FBlitter_Snapshot iFBlitter_Snapshot;

Sprite *Blitter_Snapshot::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	GLES_PERF_COUNT(_gles_perf.encode_total++);

	const auto &root = sprite.Root();
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	/* Skip font glyphs — no valid this->encoding_sprite_id_. */
	if (sprite_type == SpriteType::Font) return dest_sprite;

	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->GetSpriteAtlas().CacheMeta(this->encoding_sprite_id_,
			root.width, root.height, root.x_offs, root.y_offs);
	}
	return dest_sprite;
}

void Blitter_Snapshot::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	if (!IsRecording()) return;

	DrawCommand cmd;
	cmd.mode = mode;
	cmd.sprite = bp->sprite_id;
	cmd.x = static_cast<int16_t>(bp->sprite_x);
	cmd.y = static_cast<int16_t>(bp->sprite_y);
	cmd.width = static_cast<int16_t>(bp->width);
	cmd.height = static_cast<int16_t>(bp->height);
	cmd.skip_left = static_cast<int16_t>(bp->skip_left);
	cmd.skip_top = static_cast<int16_t>(bp->skip_top);
	cmd.sprite_width = static_cast<int16_t>(UnScaleByZoom(bp->sprite_width, zoom));
	cmd.sprite_height = static_cast<int16_t>(UnScaleByZoom(bp->sprite_height, zoom));
	cmd.zoom = zoom;
	cmd.palette = (mode == BlitterMode::ColourRemap || mode == BlitterMode::TransparentRemap) ? bp->pal : PAL_NONE;

	RecordCommand(cmd);
}
