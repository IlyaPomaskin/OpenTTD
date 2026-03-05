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
#include "../airport.h"
#include "../rail_map.h"
#include "../station_map.h"
#include "../openttd.h"
#include "../landscape.h"
#include "../gfx_func.h"
#include "../palette_func.h"
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
		int transport_facilities = 0;
		std::string reason;
		bool has_train = st->facilities.Test(StationFacility::Train);
		if (has_train) {
			int platform_len = std::max(st->train_station.w, st->train_station.h);
			if (platform_len < 3) {
				/* Small station: only include if town within 3 tiles AND
				 * (another train station nearby OR different rail type nearby). */
				bool town_near = false;
				for (Town *t : Town::Iterate()) {
					if (t->xy != INVALID_TILE && DistanceManhattan(st->xy, t->xy) <= 3) {
						town_near = true;
						break;
					}
				}
				if (!town_near) has_train = false;

				if (has_train) {
					bool rail_interest = false;
					/* Check for another train station within 10 tiles. */
					for (Station *other : Station::Iterate()) {
						if (other == st || other->xy == INVALID_TILE) continue;
						if (other->facilities.Test(StationFacility::Train) && DistanceManhattan(st->xy, other->xy) <= 10) {
							rail_interest = true;
							break;
						}
					}
					/* Check for different rail type within 5 tiles. */
					if (!rail_interest && st->train_station.tile != INVALID_TILE) {
						RailType st_rt = GetRailType(st->train_station.tile);
						uint cx = TileX(st->xy), cy = TileY(st->xy);
						for (uint dy = (cy > 5 ? cy - 5 : 0); !rail_interest && dy <= std::min(cy + 5, Map::SizeY() - 1); dy++) {
							for (uint dx = (cx > 5 ? cx - 5 : 0); dx <= std::min(cx + 5, Map::SizeX() - 1); dx++) {
								TileIndex t = TileXY(dx, dy);
								if (IsPlainRailTile(t) && GetRailType(t) != st_rt) {
									rail_interest = true;
									reason += "mixed-rail ";
									break;
								}
							}
						}
					}
					if (!rail_interest) has_train = false;
				}
			}
		}
		if (has_train) { score += 5; transport_facilities++; reason += "train+5 "; }
		if (st->facilities.Test(StationFacility::Airport)) {
			uint8_t at = st->airport.type;
			if (at != AT_HELIPORT && at != AT_HELIDEPOT && at != AT_HELISTATION) {
				score += 3; transport_facilities++; reason += "airport+3 ";
			}
		}
		if (st->facilities.Test(StationFacility::Dock))      { score += 2; transport_facilities++; reason += "dock+2 "; }
		if (st->facilities.Test(StationFacility::BusStop))   { score += 1; reason += "bus+1 "; }
		if (st->facilities.Test(StationFacility::TruckStop) && transport_facilities >= 2) { score += 1; reason += "truck+1 "; }
		if (score == 0) continue;

		/* Bonus from nearest large town within 50 tiles. */
		for (Town *t : Town::Iterate()) {
			if (t->xy == INVALID_TILE) continue;
			if (DistanceManhattan(st->xy, t->xy) < 50) {
				int town_bonus = std::min(5, (int)(t->cache.population / 500));
				score += town_bonus;
				if (town_bonus > 0) reason += fmt::format("town(pop={})+" "{} ", t->cache.population, town_bonus);
				break;
			}
		}

		/* Cluster: if another train station or real airport within 5 tiles, zoom out. */
		bool cluster = false;
		if (st->facilities.Test(StationFacility::Train)) {
			for (Station *other : Station::Iterate()) {
				if (other == st || other->xy == INVALID_TILE) continue;
				if (DistanceManhattan(st->xy, other->xy) > 5) continue;
				bool other_train = other->facilities.Test(StationFacility::Train);
				bool other_airport = other->facilities.Test(StationFacility::Airport) &&
					other->airport.type != AT_HELIPORT &&
					other->airport.type != AT_HELIDEPOT &&
					other->airport.type != AT_HELISTATION;
				if (other_train || other_airport) { cluster = true; break; }
			}
		}

		/* Pick POI tile from the highest-scoring facility (same priority as scoring). */
		TileIndex poi_tile = st->xy;
		if (st->facilities.Test(StationFacility::Train) && st->train_station.tile != INVALID_TILE) {
			poi_tile = st->train_station.GetCenterTile();
		} else if (st->facilities.Test(StationFacility::Airport) && st->airport.tile != INVALID_TILE) {
			poi_tile = st->airport.GetCenterTile();
		} else if (st->facilities.Test(StationFacility::Dock) && st->docking_station.tile != INVALID_TILE) {
			poi_tile = st->docking_station.GetCenterTile();
		} else if (st->facilities.Test(StationFacility::BusStop) && st->bus_station.tile != INVALID_TILE) {
			poi_tile = st->bus_station.GetCenterTile();
		} else if (st->facilities.Test(StationFacility::TruckStop) && st->truck_station.tile != INVALID_TILE) {
			poi_tile = st->truck_station.GetCenterTile();
		}
		float fx = (float)TileX(poi_tile) / Map::SizeX();
		float fy = (float)TileY(poi_tile) / Map::SizeY();
		int   zoom = cluster ? -1 : ((score >= 8) ? 1 : (score >= 5 ? 0 : -1));
		if (cluster) { score += 3; reason += "cluster+3 "; }
		candidates.push_back({fx, fy, score, zoom, 5000, fmt::format("station: {}", reason)});
	}

	/* --- Rail junctions -------------------------------------------------- */
	/* Find tiles with 3+ track bits (junctions), cluster nearby ones. */
	struct Junction { uint x; uint y; };
	std::vector<Junction> junctions;
	for (uint y = 1; y < Map::SizeY() - 1; y++) {
		for (uint x = 1; x < Map::SizeX() - 1; x++) {
			TileIndex tile = TileXY(x, y);
			if (!IsPlainRailTile(tile)) continue;
			if (CountBits(GetTrackBits(tile)) >= 3) {
				junctions.push_back({x, y});
			}
		}
	}

	/* Cluster nearby junctions (within 5 tiles) and emit center of each cluster. */
	std::vector<bool> visited(junctions.size(), false);
	for (size_t i = 0; i < junctions.size(); i++) {
		if (visited[i]) continue;
		visited[i] = true;

		uint sum_x = junctions[i].x, sum_y = junctions[i].y;
		int count = 1;

		for (size_t j = i + 1; j < junctions.size(); j++) {
			if (visited[j]) continue;
			uint dx = (junctions[j].x > sum_x / count) ? junctions[j].x - sum_x / count : sum_x / count - junctions[j].x;
			uint dy = (junctions[j].y > sum_y / count) ? junctions[j].y - sum_y / count : sum_y / count - junctions[j].y;
			if (dx + dy <= 5) {
				visited[j] = true;
				sum_x += junctions[j].x;
				sum_y += junctions[j].y;
				count++;
			}
		}

		int score = (count >= 3) ? 8 : (count >= 2) ? 5 : 3;
		float fx = (float)(sum_x / count) / Map::SizeX();
		float fy = (float)(sum_y / count) / Map::SizeY();
		int zoom = (count >= 3) ? -1 : 0;
		candidates.push_back({fx, fy, score, zoom, 5000,
			fmt::format("rail junction: {} junctions in cluster, score={}", count, score)});
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
		candidates.push_back({fx, fy, score, 0, 5000,
			fmt::format("town: pop={}, score={}", t->cache.population, score)});
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
		Debug(driver, 0, "  POI[{}] score={} fx={:.2f} fy={:.2f} zoom={} — {}", i, p.score, p.map_fx, p.map_fy, p.zoom_adjust, p.reason);
	}
}

/** Whether user is manually browsing POIs (disables auto map rotation). */
static bool _poi_manual_browse = false;

/** Move camera to the current POI. */
static void ShowCurrentPOI()
{
	float fx, fy;
	int zoom_adjust;

	if (!_gles_poi_list.empty()) {
		const GlesPOI &poi = _gles_poi_list[_gles_poi_idx];
		fx          = poi.map_fx;
		fy          = poi.map_fy;
		zoom_adjust = poi.zoom_adjust;
		Debug(driver, 0, "GLES ShowCurrentPOI: POI[{}] score={} fx={:.2f} fy={:.2f} — {}",
			_gles_poi_idx, poi.score, fx, fy, poi.reason);
	} else {
		int idx = std::rand() % (int)kDefaultWaypoints.size();
		fx          = kDefaultWaypoints[idx].map_fx;
		fy          = kDefaultWaypoints[idx].map_fy;
		zoom_adjust = kDefaultWaypoints[idx].zoom_adjust;
	}

	/* Set zoom BEFORE scrolling — ScrollMainWindowTo uses virtual_width/height
	 * to compute the center offset, so zoom must be correct first. */
	FixTitleGameZoom(zoom_adjust);

	Window *w = GetMainWindow();
	if (w != nullptr && w->viewport != nullptr && w->viewport->zoom < ZoomLevel::In4x) {
		ViewportData &vp = *w->viewport;
		vp.zoom = ZoomLevel::In4x;
		vp.virtual_width = ScaleByZoom(vp.width, vp.zoom);
		vp.virtual_height = ScaleByZoom(vp.height, vp.zoom);
	}

	int world_x = (int)(fx * Map::SizeX() * TILE_SIZE);
	int world_y = (int)(fy * Map::SizeY() * TILE_SIZE);
	ScrollMainWindowTo(world_x, world_y, -1, true);

	MarkWholeScreenDirty();
}

void NavigatePOI(int delta)
{
	if (Map::SizeX() == 0 || Map::SizeY() == 0) return;

	if (_gles_poi_list.empty() || Map::SizeX() * Map::SizeY() != _gles_poi_map_tiles) {
		ScanMapPOIs();
	}
	if (_gles_poi_list.empty()) return;

	_poi_manual_browse = true;
	int n = (int)_gles_poi_list.size();
	_gles_poi_idx = ((_gles_poi_idx + delta) % n + n) % n;
	ShowCurrentPOI();
}

void PrepareBackground()
{
	if (Map::SizeX() == 0 || Map::SizeY() == 0) return;

	/* Rescan if map changed or not yet scanned. */
	bool fresh_scan = false;
	if (_gles_poi_list.empty() || Map::SizeX() * Map::SizeY() != _gles_poi_map_tiles) {
		ScanMapPOIs();
		fresh_scan = true;
	}

	if (fresh_scan) {
		/* First call after loading a new map — show POI[0] immediately. */
		ShowCurrentPOI();
		return;
	}

	/* After cycling through all POIs on this map, rotate to next title map.
	 * Skip rotation if user is manually browsing with hotkeys. */
	if (!_poi_manual_browse && _gles_poi_idx == 0 && !_gles_poi_list.empty() &&
			_switch_mode == SM_NONE && CanRotateTitleMap()) {
		Debug(driver, 0, "GLES PrepareBackground: all POIs shown, rotating to next title map");
		RequestNextTitleMap();
		return;
	}
	_poi_manual_browse = false;

	if (!_gles_poi_list.empty()) {
		_gles_poi_idx = (_gles_poi_idx + 1) % (int)_gles_poi_list.size();
	}
	ShowCurrentPOI();
}

void DrawPOIMarkers(const Viewport &vp)
{
	if (_gles_poi_list.empty()) return;

	const DrawPixelInfo *dpi = _cur_dpi;
	int half = 5;

	for (int i = 0; i < (int)_gles_poi_list.size(); i++) {
		const GlesPOI &poi = _gles_poi_list[i];

		/* Same world coords as ShowCurrentPOI / ScrollMainWindowTo uses. */
		int wx = (int)(poi.map_fx * Map::SizeX() * TILE_SIZE);
		int wy = (int)(poi.map_fy * Map::SizeY() * TILE_SIZE);
		int wz = GetSlopePixelZ(
			Clamp(wx, 0, (int)Map::SizeX() * TILE_SIZE - 1),
			Clamp(wy, 0, (int)Map::SizeY() * TILE_SIZE - 1));
		Point p = RemapCoords(wx, wy, wz);

		int sx = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
		int sy = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;

		/* Clip to dpi bounds. */
		if (sx + half < dpi->left || sx - half >= dpi->left + dpi->width) continue;
		if (sy + half < dpi->top  || sy - half >= dpi->top  + dpi->height) continue;

		int h2 = half + 2;
		GfxFillRect(sx - h2, sy - h2, sx + h2, sy + h2, PC_RED);
		if (i == _gles_poi_idx) {
			GfxFillRect(sx - half, sy - half, sx + half, sy + half, PC_WHITE);
		}
	}
}
