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

#include "../safeguards.h"

static FBlitter_GLES iFBlitter_GLES;

/** Early staging buffer for sprites encoded before GLESBackend is ready. */
static std::unordered_map<GLESSpriteID, GLESStagedPixels> &GetEarlyStaged()
{
	static std::unordered_map<GLESSpriteID, GLESStagedPixels> buf;
	return buf;
}

void FlushEarlyStaged()
{
	auto &early = GetEarlyStaged();
	if (early.empty()) return;

	GLESBackend *backend = GLESBackend::Get();
	if (backend == nullptr) return;

	GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
	for (auto &[key, sp] : early) {
		SpriteID sprite_id = static_cast<SpriteID>(key >> 4);
		ZoomLevel zoom = static_cast<ZoomLevel>(key & 0xF);
		atlas.Stage(sprite_id, zoom, reinterpret_cast<const SpriteLoader::CommonPixel *>(sp.pixels.data()),
		            sp.width, sp.height, sp.has_rgb, sp.has_remap);
	}
	early.clear();
}

/**
 * Encode a sprite for the GLES blitter.
 * Allocates a minimal Sprite (dimensions only) and uploads pixel data to the GPU atlas.
 * No CPU-side RLE encoding — all rendering goes through the GPU.
 */
Sprite *Blitter_GLES::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	_gles_perf.encode_total++;

	const auto &root = sprite.Root();
	Sprite *dest_sprite = allocator.Allocate<Sprite>(sizeof(Sprite));
	dest_sprite->height = root.height;
	dest_sprite->width = root.width;
	dest_sprite->x_offs = root.x_offs;
	dest_sprite->y_offs = root.y_offs;

	/* Find the single best (largest) zoom variant for GPU scaling. */
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

		GLESBackend *backend = GLESBackend::Get();
		if (backend != nullptr) {
			backend->GetSpriteAtlas().Stage(_gles_encoding_sprite_id, kGPUScaleBaseZoom, best->data,
			                               best->width, best->height, has_rgb, has_remap);
		} else {
			/* Backend not ready yet — save to early staging buffer. */
			GLESStagedPixels sp;
			size_t count = static_cast<size_t>(best->width) * best->height;
			sp.pixels.assign(best->data, best->data + count);
			sp.width = best->width;
			sp.height = best->height;
			sp.has_rgb = has_rgb;
			sp.has_remap = has_remap;
			GLESSpriteID key = MakeGLESSpriteKey(_gles_encoding_sprite_id, kGPUScaleBaseZoom);
			GetEarlyStaged()[key] = std::move(sp);
		}
		_gles_perf.encode_uploaded++;
	}

	return dest_sprite;
}

void Blitter_GLES::DrawRect(void *video, int width, int height, PixelColour colour)
{
	/* Always fill rectangles into the CPU buffer.  Even with GPU sprites,
	 * the CPU buffer is uploaded as the background layer in the FBO —
	 * it provides toolbar, window, and viewport backgrounds that GPU
	 * sprites render on top of. */
	Blitter_32bppBase::DrawRect(video, width, height, colour);
}

/**
 * Draw override for the GLES blitter.
 * When GPU sprites are enabled, queues draw commands for the GPU batch renderer.
 * Otherwise falls back to CPU rendering via Blitter_32bppOptimized::Draw.
 */
void Blitter_GLES::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	if (_gles_gpu_sprites) {
		GLESBackend *backend = GLESBackend::Get();
		if (backend == nullptr) return;

		/* Only intercept draws to the actual screen buffer, not screenshots. */
		const uint32_t *screen_start = static_cast<const uint32_t *>(_screen.dst_ptr);
		const uint32_t *screen_end = screen_start + _screen.pitch * _screen.height;
		const uint32_t *dst = static_cast<const uint32_t *>(bp->dst);

		if (dst < screen_start || dst >= screen_end) {
			_gles_perf.gpu_skip_offscreen++;
			return;
		}

		GLESSpriteID key = MakeGLESSpriteKey(bp->sprite_id, zoom);
		GLESSpriteAtlas &atlas = backend->GetSpriteAtlas();
		const GLESSpriteEntry *entry = atlas.LookupOrUpload(key);

		int zi = static_cast<int>(zoom);
		if (zi >= 0 && zi < 8) _gles_perf.gpu_zoom_counts[zi]++;

		if (entry == nullptr) {
			_gles_perf.gpu_sprites_missing++;
			return;
		}

		/* Convert buffer-relative coords to absolute screen coords. */
		ptrdiff_t pixel_offset = dst - screen_start;
		int abs_x = static_cast<int>(pixel_offset % _screen.pitch) + bp->left;
		int abs_y = static_cast<int>(pixel_offset / _screen.pitch) + bp->top;

		GLESDrawCommand cmd;
		cmd.sprite_key = key;
		cmd.screen_x = static_cast<int16_t>(abs_x);
		cmd.screen_y = static_cast<int16_t>(abs_y);
		cmd.width = static_cast<int16_t>(bp->width);
		cmd.height = static_cast<int16_t>(bp->height);
		cmd.skip_left = static_cast<int16_t>(bp->skip_left);
		cmd.skip_top = static_cast<int16_t>(bp->skip_top);
		/* Use zoom-adjusted dimensions so UV fractions are correct. */
		cmd.sprite_width = static_cast<int16_t>(UnScaleByZoom(bp->sprite_width, zoom));
		cmd.sprite_height = static_cast<int16_t>(UnScaleByZoom(bp->sprite_height, zoom));
		cmd.zoom = zoom;
		cmd.mode = mode;
		cmd.remap_idx = 0;
		cmd.palette_only = entry->palette_only;
		if (mode == BlitterMode::ColourRemap || mode == BlitterMode::CrashRemap ||
		    mode == BlitterMode::BlackRemap) {
			cmd.remap = bp->remap;
		}
		backend->QueueDraw(cmd);
		return;
	}

	/* CPU fallback (only when _gles_gpu_sprites is off). */
	Blitter_32bppOptimized::Draw(bp, mode, zoom);
}
