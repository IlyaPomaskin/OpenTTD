/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_poi.h POI scanner and camera rotation for wallpaper/intro mode. */

#ifndef GLES_POI_H
#define GLES_POI_H

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

/**
 * A scored point of interest found by scanning the current map.
 * Built dynamically from stations and towns; sorted by score descending.
 */
struct GlesPOI {
	float    map_fx;     ///< Fractional X across map width  (0..1)
	float    map_fy;     ///< Fractional Y across map height (0..1)
	int      score;      ///< Aggregate interest score (higher = more interesting)
	int      zoom;       ///< ZoomLevel value: 0 = In4x, 1 = In2x
	uint32_t delay_ms;   ///< Time to hold position before moving to next POI
	std::string reason;  ///< Why this POI was selected
	std::vector<std::pair<float, float>> influences; ///< Map positions of objects that contributed to score
};

/**
 * Advance the camera to the next scored POI on the current map.
 *
 * Scans all stations and towns on first call (or when the map changes),
 * scores them by transport facilities and nearby population, keeps the
 * top 10, and cycles through them in score order.
 *
 * Safe to call from any thread that holds the game state (i.e. the main
 * game / GL thread).  No-op when the map has no POIs.
 */
void PrepareBackground();
void NavigatePOI(int delta);
void InvalidatePOIs();
void DrawPOIMarkers(const struct Viewport &vp);

#endif /* GLES_POI_H */
