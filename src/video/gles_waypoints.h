/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_waypoints.h Camera waypoints for GLES wallpaper mode. */

#ifndef GLES_WAYPOINTS_H
#define GLES_WAYPOINTS_H

#include <cstdint>
#include <array>

/**
 * A camera waypoint for the wallpaper camera tour.
 *
 * Positions are expressed as fractions of map dimensions (0.0..1.0) so they
 * work with any map size. Convert to world coordinates at runtime:
 *   world_x = map_fx * Map::SizeX() * TILE_SIZE
 *   world_y = map_fy * Map::SizeY() * TILE_SIZE
 */
struct GlesWaypoint {
	float map_fx;        ///< Fractional X position across map width  (0.0 = left,  1.0 = right).
	float map_fy;        ///< Fractional Y position across map height (0.0 = top,   1.0 = bottom).
	int   zoom_adjust;   ///< Zoom delta relative to default GUI zoom (negative = zoom in, positive = zoom out).
	uint32_t delay_ms;   ///< Time to hold this position before moving to the next waypoint (ms).
	bool  smooth_pan;    ///< If true, camera eases smoothly from the previous waypoint.
};

/**
 * Default camera waypoints for the intro/wallpaper tour.
 *
 * Covers corners, edges, and interior points so the tour samples the whole map.
 * Adjust zoom_adjust and delay_ms to taste.
 */
static constexpr std::array<GlesWaypoint, 12> kDefaultWaypoints = {{
	/* Centre of map — establishing shot */
	{ 0.50f, 0.50f,  0, 6000, false },

	/* North-west quadrant */
	{ 0.25f, 0.25f, -1, 5000, true  },

	/* North edge, centre */
	{ 0.50f, 0.15f,  0, 4000, true  },

	/* North-east quadrant */
	{ 0.75f, 0.25f, -1, 5000, true  },

	/* East edge, centre */
	{ 0.85f, 0.50f,  0, 4000, true  },

	/* South-east quadrant */
	{ 0.75f, 0.75f, -1, 5000, true  },

	/* South edge, centre */
	{ 0.50f, 0.85f,  0, 4000, true  },

	/* South-west quadrant */
	{ 0.25f, 0.75f, -1, 5000, true  },

	/* West edge, centre */
	{ 0.15f, 0.50f,  0, 4000, true  },

	/* Centre again, zoomed out for overview */
	{ 0.50f, 0.50f,  1, 7000, true  },

	/* Off-centre interior points */
	{ 0.35f, 0.40f, -1, 5000, true  },
	{ 0.65f, 0.60f, -1, 5000, true  },
}};

/**
 * A scored point of interest found by scanning the current map.
 * Built dynamically from stations and towns; sorted by score descending.
 */
struct GlesPOI {
	float    map_fx;     ///< Fractional X across map width  (0..1)
	float    map_fy;     ///< Fractional Y across map height (0..1)
	int      score;      ///< Aggregate interest score (higher = more interesting)
	int      zoom_adjust;///< Zoom delta relative to GUI zoom
	uint32_t delay_ms;   ///< Time to hold position before moving to next POI
};

#endif /* GLES_WAYPOINTS_H */
