/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sprite_file_type.hpp Random Access File specialised for accessing sprites. */

#ifndef SPRITE_FILE_TYPE_HPP
#define SPRITE_FILE_TYPE_HPP

#include "../random_access_file_type.h"
#include <vector>

/**
 * RandomAccessFile with some extra information specific for sprite files.
 * It automatically detects and stores the container version upload opening the file.
 */
class SpriteFile : public RandomAccessFile {
	bool palette_remap;     ///< Whether or not a remap of the palette is required for this file.
	uint8_t container_version; ///< Container format of the sprite file.
	size_t content_begin;   ///< The begin of the content of the sprite file, i.e. after the container metadata.
	std::vector<uint8_t> mem_buffer; ///< Owned memory buffer (when loaded into memory).
public:
	SpriteFile(const std::string &filename, Subdirectory subdir, bool palette_remap);
	/** Construct from in-memory buffer (for GL thread). */
	SpriteFile(const uint8_t *data, size_t size, std::string_view filename,
	           bool palette_remap, uint8_t container_version, size_t content_begin,
	           size_t base_offset);
	SpriteFile(const SpriteFile&) = delete;
	void operator=(const SpriteFile&) = delete;

	void LoadIntoMemory();
	const uint8_t *GetMemoryData() const { return this->mem_buffer.empty() ? nullptr : this->mem_buffer.data(); }
	size_t GetMemorySize() const { return this->mem_buffer.size(); }
	void SetMemoryBuffer(std::vector<uint8_t> &&buf) { this->mem_buffer = std::move(buf); }
	std::vector<uint8_t> TakeMemoryBuffer() { return std::move(this->mem_buffer); }

	/**
	 * Whether a palette remap is needed when loading sprites from this file.
	 * @return True when needed, otherwise false.
	 */
	bool NeedsPaletteRemap() const { return this->palette_remap; }

	/**
	 * Get the version number of container type used by the file.
	 * @return The version.
	 */
	uint8_t GetContainerVersion() const { return this->container_version; }

	/**
	 * Seek to the begin of the content, i.e. the position just after the container version has been determined.
	 */
	void SeekToBegin() { this->SeekTo(this->content_begin, SEEK_SET); }

	/** Get the content begin offset (after container header). */
	size_t GetContentBegin() const { return this->content_begin; }
};

#endif /* SPRITE_FILE_TYPE_HPP */
