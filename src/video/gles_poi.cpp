/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_poi.cpp POI scanner and camera rotation for wallpaper/intro mode. */

#include "../stdafx.h"
#include "../debug.h"
#include "../map_func.h"
#include "../tile_type.h"
#include "../viewport_func.h"
#include "../viewport_type.h"
#include "../zoom_func.h"
#include "../window_func.h"
#include "../station_base.h"
#include "../town.h"
#include "gles_poi.h"
#include "gles_waypoints.h"
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "../safeguards.h"

/** Scanned POI list (top 10 by score), rebuilt when map changes. */
static std::vector<GlesPOI> _gles_poi_list;
static int _gles_poi_idx = 0;
static uint _gles_poi_map_tiles = 0; ///< Map::SizeX()*SizeY() at last scan; triggers rescan on change.

/**
 * Score and collect all interesting locations on the current map.
 *
 * Scoring:
 *  Airport          +4
 *  Rail station     +3
 *  Dock             +2
 *  Bus/truck stop   +1 each
 *  Nearby town pop  +1 per 500 people (up to +5, within 50 tiles)
 *
 * Stations dominate; large uncovered towns are added as fallback.
 * Results are sorted by score descending, top 10 kept.
 */
static void ScanMapPOIs()
{
	_gles_poi_list.clear();
	_gles_poi_map_tiles = Map::SizeX() * Map::SizeY();

	std::vector<GlesPOI> candidates;
	candidates.reserve(64);

	/* --- Stations -------------------------------------------------------- */
	for (Station *st : Station::Iterate()) {
		if (st->xy == INVALID_TILE) continue;

		int score = 0;
		if (st->facilities.Test(StationFacility::Airport))   score += 4;
		if (st->facilities.Test(StationFacility::Train))     score += 3;
		if (st->facilities.Test(StationFacility::Dock))      score += 2;
		if (st->facilities.Test(StationFacility::BusStop))   score += 1;
		if (st->facilities.Test(StationFacility::TruckStop)) score += 1;
		if (score == 0) continue;

		/* Bonus from nearest large town within 50 tiles. */
		for (Town *t : Town::Iterate()) {
			if (t->xy == INVALID_TILE) continue;
			if (DistanceManhattan(st->xy, t->xy) < 50) {
				score += std::min(5, (int)(t->cache.population / 500));
				break; /* one bonus per station */
			}
		}

		float fx = (float)TileX(st->xy) / Map::SizeX();
		float fy = (float)TileY(st->xy) / Map::SizeY();
		int   zoom = (score >= 8) ? 1 : (score >= 5 ? 0 : -1);
		candidates.push_back({fx, fy, score, zoom, 5000});
	}

	/* --- Towns not already covered by a nearby station ------------------- */
	for (Town *t : Town::Iterate()) {
		if (t->xy == INVALID_TILE || t->cache.population < 500) continue;

		/* Skip if a station POI already represents this town (within 30 tiles). */
		bool covered = false;
		for (const GlesPOI &poi : candidates) {
			TileIndex poi_tile = TileXY(
				(uint)(poi.map_fx * Map::SizeX()),
				(uint)(poi.map_fy * Map::SizeY()));
			if (DistanceManhattan(t->xy, poi_tile) < 30) { covered = true; break; }
		}
		if (covered) continue;

		int score = std::min(5, (int)(t->cache.population / 500));
		float fx  = (float)TileX(t->xy) / Map::SizeX();
		float fy  = (float)TileY(t->xy) / Map::SizeY();
		candidates.push_back({fx, fy, score, 0, 5000});
	}

	/* Sort by score descending, keep top 10. */
	std::sort(candidates.begin(), candidates.end(),
		[](const GlesPOI &a, const GlesPOI &b) { return a.score > b.score; });

	int n = std::min(10, (int)candidates.size());
	_gles_poi_list.assign(candidates.begin(), candidates.begin() + n);
	_gles_poi_idx = 0;

	Debug(driver, 0, "GLES ScanMapPOIs: {} POIs (map {}x{})",
		(int)_gles_poi_list.size(), Map::SizeX(), Map::SizeY());
	for (int i = 0; i < (int)_gles_poi_list.size(); i++) {
		const GlesPOI &p = _gles_poi_list[i];
		Debug(driver, 0, "  POI[{}] score={} fx={:.2f} fy={:.2f} zoom={}", i, p.score, p.map_fx, p.map_fy, p.zoom_adjust);
	}
}

void PrepareBackground()
{
	if (Map::SizeX() == 0 || Map::SizeY() == 0) return;

	/* Rescan if map changed or not yet scanned. */
	if (_gles_poi_list.empty() || Map::SizeX() * Map::SizeY() != _gles_poi_map_tiles) {
		ScanMapPOIs();
	}

	float fx, fy;
	int zoom_adjust;

	if (!_gles_poi_list.empty()) {
		/* Cycle through POIs in score order. */
		const GlesPOI &poi = _gles_poi_list[_gles_poi_idx];
		fx          = poi.map_fx;
		fy          = poi.map_fy;
		zoom_adjust = poi.zoom_adjust;
		_gles_poi_idx = (_gles_poi_idx + 1) % (int)_gles_poi_list.size();
		Debug(driver, 0, "GLES PrepareBackground: POI[{}] score={} fx={:.2f} fy={:.2f}",
			_gles_poi_idx == 0 ? (int)_gles_poi_list.size() - 1 : _gles_poi_idx - 1,
			_gles_poi_list[_gles_poi_idx == 0 ? (int)_gles_poi_list.size() - 1 : _gles_poi_idx - 1].score,
			fx, fy);
	} else {
		/* Fallback: random static waypoint. */
		int idx = std::rand() % (int)kDefaultWaypoints.size();
		fx          = kDefaultWaypoints[idx].map_fx;
		fy          = kDefaultWaypoints[idx].map_fy;
		zoom_adjust = kDefaultWaypoints[idx].zoom_adjust;
	}

	int world_x = (int)(fx * Map::SizeX() * TILE_SIZE);
	int world_y = (int)(fy * Map::SizeY() * TILE_SIZE);
	ScrollMainWindowTo(world_x, world_y, -1, true);
	FixTitleGameZoom(zoom_adjust);

	/* Ensure zoom stays within Normal..Out2x range for GPU scaling. */
	Window *w = GetMainWindow();
	if (w != nullptr && w->viewport != nullptr && w->viewport->zoom < ZoomLevel::In4x) {
		ViewportData &vp = *w->viewport;
		vp.virtual_width = ScaleByZoom(vp.width, ZoomLevel::In4x);
		vp.virtual_height = ScaleByZoom(vp.height, ZoomLevel::In4x);
		vp.zoom = ZoomLevel::In4x;
	}

	MarkWholeScreenDirty();
}
