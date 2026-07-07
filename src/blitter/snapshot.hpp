/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file snapshot.hpp Recording blitter that captures draw commands into a snapshot buffer. */

#ifndef BLITTER_SNAPSHOT_HPP
#define BLITTER_SNAPSHOT_HPP

#include "factory.hpp"
#include "../video/draw_snapshot.h"

/**
 * Recording blitter for two-thread snapshot rendering.
 *
 * Set as the active blitter for the entire session in snapshot mode.
 * Draw() records DrawCommands into the snapshot buffer.
 * Encode() caches sprite metadata into the GPU atlas.
 * No pixels are written, no GL calls are made.
 * Safe to use from the game thread without a GL context.
 */
class Blitter_Snapshot : public Blitter {
	void *recording_buffer = nullptr; ///< Stable pointer to the dummy buffer (avoids race with _screen.dst_ptr).
	int recording_pitch = 0;          ///< Pitch at recording start.
public:
	/** Set the recording buffer pointer and pitch. Must be called before StartRecording. */
	void SetRecordingBuffer(void *buf, int pitch) { this->recording_buffer = buf; this->recording_pitch = pitch; }
	uint8_t GetScreenDepth() override { return 32; }
	bool Is32BppSupported() override { return true; }
	uint GetSpriteAlignment() override { return 1; }

	void Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom) override;
	Sprite *Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator) override;

	void DrawColourMappingRect(void *, int, int, PaletteID) override {}
	void *MoveTo(void *video, int x, int y) override;
	void SetPixel(void *, int, int, PixelColour) override {}
	void DrawRect(void *, int, int, PixelColour) override {}
	void DrawLine(void *, int, int, int, int, int, int, PixelColour, int, int) override {}
	void CopyFromBuffer(void *, const void *, int, int) override {}
	void CopyToBuffer(const void *, void *, int, int) override {}
	void CopyImageToBuffer(const void *, void *, int, int, int) override {}
	void ScrollBuffer(void *, int &, int &, int &, int &, int, int) override {}
	size_t BufferSize(uint w, uint h) override { return static_cast<size_t>(w) * h * 4; }
	void PaletteAnimate(const Palette &) override {}
	Blitter::PaletteAnimation UsePaletteAnimation() override { return Blitter::PaletteAnimation::Blitter; }

	std::string_view GetName() override { return "snapshot"; }
};

/** Factory for the snapshot blitter. */
class FBlitter_Snapshot : public BlitterFactory {
public:
	FBlitter_Snapshot() : BlitterFactory("snapshot", "Snapshot Blitter (two-thread recording)") {}
	std::unique_ptr<Blitter> CreateInstance() override { return std::make_unique<Blitter_Snapshot>(); }
};


#endif /* BLITTER_SNAPSHOT_HPP */
