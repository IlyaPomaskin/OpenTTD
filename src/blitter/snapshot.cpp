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
#include "../video/gles_sprite.h"

#include "../safeguards.h"

/** Register the snapshot blitter factory so it can be selected by name. */
static FBlitter_Snapshot iFBlitter_Snapshot;

/** Early staging buffer for sprites encoded before GLESBackend is ready. */
static std::vector<GLESUploadRequest> &GetEarlyStaged()
{
	static std::vector<GLESUploadRequest> buf;
	return buf;
}

void FlushEarlyStaged()
{
	auto &early = GetEarlyStaged();
	if (early.empty()) return;

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return;

	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	for (auto &req : early) {
		SpriteID sprite_id = static_cast<SpriteID>(req.key >> 4);
		ZoomLevel zoom = static_cast<ZoomLevel>(req.key & 0xF);
		atlas.Enqueue(sprite_id, zoom, req.pixels.data(),
		              req.width, req.height, req.has_rgb, req.has_remap);
	}
	early.clear();
}

Sprite *Blitter_Snapshot::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	_gles_perf.encode_total++;

	const auto &root = sprite.Root();
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	/* Skip font glyphs — no valid this->encoding_sprite_id_. */
	if (sprite_type == SpriteType::Font) return dest_sprite;

	GLESBackend *backend = GLESBackend::Get();

	/* Cache root dimensions for ReadSprite fast-path. */
	if (backend != nullptr) {
		backend->GetSpriteAtlas().CacheMeta(this->encoding_sprite_id_,
			root.width, root.height, root.x_offs, root.y_offs);
	}

	/* Find the single best (largest) zoom variant for GPU scaling.
	 * Prefer base zoom, then search toward more detail,
	 * then toward less detail. */
	const SpriteLoader::Sprite *best = nullptr;

	for (int z = to_underlying(kGPUScaleBaseZoom); z >= to_underlying(ZoomLevel::Begin); z--) {
		const auto &s = sprite[static_cast<ZoomLevel>(z)];
		if (s.data != nullptr && s.width > 0 && s.height > 0) { best = &s; break; }
	}
	if (best == nullptr) {
		for (int z = to_underlying(kGPUScaleBaseZoom) + 1; z < to_underlying(ZoomLevel::End); z++) {
			const auto &s = sprite[static_cast<ZoomLevel>(z)];
			if (s.data != nullptr && s.width > 0 && s.height > 0) { best = &s; break; }
		}
	}
	if (best != nullptr) {
		bool has_rgb = best->colours.Test(SpriteComponent::RGB) || best->colours.Test(SpriteComponent::Alpha);
		bool has_remap = best->colours.Test(SpriteComponent::Palette);

		if (backend != nullptr) {
			backend->GetSpriteAtlas().Enqueue(this->encoding_sprite_id_, kGPUScaleBaseZoom, best->data,
			                                  best->width, best->height, has_rgb, has_remap);
		} else {
			/* Backend not ready yet — save to early staging buffer. */
			GLESUploadRequest req;
			size_t count = static_cast<size_t>(best->width) * best->height;
			req.key = MakeGLESSpriteKey(this->encoding_sprite_id_, kGPUScaleBaseZoom);
			req.pixels.assign(best->data, best->data + count);
			req.width = best->width;
			req.height = best->height;
			req.has_rgb = has_rgb;
			req.has_remap = has_remap;
			GetEarlyStaged().push_back(std::move(req));
		}
		_gles_perf.encode_uploaded++;
	}

	return dest_sprite;
}

void Blitter_Snapshot::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	if (!IsRecording()) return;

	DrawCommand cmd;
	cmd.type = (mode == BlitterMode::ColourRemap) ? DrawCommand::RECOLOUR : DrawCommand::SPRITE;
	cmd.mode = mode;
	cmd.sprite = bp->sprite_id;
	cmd.palette = 0;

	/* Compute screen position from dst pointer offset into recording buffer. */
	const uint32_t *screen_start = static_cast<const uint32_t *>(this->recording_buffer);
	const uint32_t *dst = static_cast<const uint32_t *>(bp->dst);
	ptrdiff_t pixel_offset = dst - screen_start;
	int abs_x = static_cast<int>(pixel_offset % this->recording_pitch) + bp->left;
	int abs_y = static_cast<int>(pixel_offset / this->recording_pitch) + bp->top;

	cmd.x = static_cast<int16_t>(abs_x);
	cmd.y = static_cast<int16_t>(abs_y);
	cmd.width = static_cast<int16_t>(bp->width);
	cmd.height = static_cast<int16_t>(bp->height);
	cmd.skip_left = static_cast<int16_t>(bp->skip_left);
	cmd.skip_top = static_cast<int16_t>(bp->skip_top);
	cmd.sprite_width = static_cast<int16_t>(UnScaleByZoom(bp->sprite_width, zoom));
	cmd.sprite_height = static_cast<int16_t>(UnScaleByZoom(bp->sprite_height, zoom));
	cmd.zoom = zoom;
	cmd.remap = bp->remap;

	RecordCommand(cmd);
}

void *Blitter_Snapshot::MoveTo(void *video, int x, int y)
{
	int pitch = this->recording_pitch > 0 ? this->recording_pitch : _screen.pitch;
	return static_cast<uint32_t *>(video) + x + y * pitch;
}
