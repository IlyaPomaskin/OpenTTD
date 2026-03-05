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
#include "../town_map.h"
#include "../tile_map.h"
#include "../object_map.h"
#include "../object_type.h"
#include "../openttd.h"
#include "../landscape.h"
#include "../gfx_func.h"
#include "../palette_func.h"
#include "gles_poi.h"
#include "gles_waypoints.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "../safeguards.h"

/**
 * Count buildings (houses) from each town within a radius around a tile.
 * Returns the best town (most buildings) and its building count.
 * @param center Tile to search around.
 * @param radius Manhattan distance radius.
 * @param best_town Output: town with the most buildings (or nullptr).
 * @return Number of buildings from the best town.
 */
static int CountTownBuildings(TileIndex center, uint radius, Town **best_town)
{
	std::map<TownID, int> counts; // town index → building count
	uint cx = TileX(center), cy = TileY(center);
	uint x0 = cx > radius ? cx - radius : 0;
	uint y0 = cy > radius ? cy - radius : 0;
	uint x1 = std::min(cx + radius, Map::SizeX() - 1);
	uint y1 = std::min(cy + radius, Map::SizeY() - 1);

	for (uint dy = y0; dy <= y1; dy++) {
		for (uint dx = x0; dx <= x1; dx++) {
			TileIndex t = TileXY(dx, dy);
			if (IsTileType(t, TileType::House)) {
				TownID idx = GetTownIndex(t);
				counts[idx]++;
			}
		}
	}

	int best_count = 0;
	TownID best_idx{};
	for (const auto &[idx, cnt] : counts) {
		if (cnt > best_count) {
			best_count = cnt;
			best_idx = idx;
		}
	}

	if (best_town != nullptr) {
		*best_town = (best_count > 0) ? Town::Get(best_idx) : nullptr;
	}
	return best_count;
}

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
				/* Small station: only include if surrounded by at least 4 buildings
				 * from a town AND (another train station nearby OR different rail type nearby). */
				bool town_near = CountTownBuildings(st->xy, 3, nullptr) >= 4;
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
				TileIndex ap_tile = st->airport.tile != INVALID_TILE ? st->airport.GetCenterTile() : st->xy;
				if (CountTownBuildings(ap_tile, 5, nullptr) >= 10) {
					score += 3; transport_facilities++; reason += "airport+3 ";
				}
			}
		}
		if (st->facilities.Test(StationFacility::Dock)) {
			TileIndex dock_tile = st->docking_station.tile != INVALID_TILE ? st->docking_station.GetCenterTile() : st->xy;
			/* Check for adjacent road tile. */
			bool road_adj = false;
			for (int d = 0; d < 4 && !road_adj; d++) {
				static const int dx[] = {0, 1, 0, -1};
				static const int dy[] = {-1, 0, 1, 0};
				uint nx = TileX(dock_tile) + dx[d], ny = TileY(dock_tile) + dy[d];
				if (nx < Map::SizeX() && ny < Map::SizeY() && IsTileType(TileXY(nx, ny), TileType::Road)) road_adj = true;
			}
			/* Check for train station within 3 tiles. */
			bool train_near = false;
			for (Station *other : Station::Iterate()) {
				if (other->xy == INVALID_TILE) continue;
				if (other->facilities.Test(StationFacility::Train) && DistanceManhattan(dock_tile, other->xy) <= 3) {
					train_near = true;
					break;
				}
			}
			if ((road_adj && CountTownBuildings(dock_tile, 3, nullptr) > 5) || train_near) {
				score += 2; transport_facilities++; reason += "dock+2 ";
			}
		}
		if (st->facilities.Test(StationFacility::BusStop))   { score += 1; reason += "bus+1 "; }
		if (st->facilities.Test(StationFacility::TruckStop) && transport_facilities >= 2) { score += 1; reason += "truck+1 "; }
		if (score == 0) continue;

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
		if (has_train && st->train_station.tile != INVALID_TILE) {
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

		/* Bonus from town with at least 4 buildings within 5 tiles of poi_tile. */
		Town *bonus_town = nullptr;
		int building_count = CountTownBuildings(poi_tile, 5, &bonus_town);
		if (building_count >= 4 && bonus_town != nullptr) {
			int town_bonus = std::min(5, (int)(bonus_town->cache.population / 500));
			score += town_bonus;
			if (town_bonus > 0) reason += fmt::format("town(pop={},bld={})+" "{} ", bonus_town->cache.population, building_count, town_bonus);
		}

		float fx = (float)TileX(poi_tile) / Map::SizeX();
		float fy = (float)TileY(poi_tile) / Map::SizeY();
		int   zoom = cluster ? 1 : 0; // cluster → In2x, otherwise In4x
		if (cluster) { score += 3; reason += "cluster+3 "; }

		/* Collect influence points — facilities that contributed to the score. */
		std::vector<std::pair<float, float>> infl;
		auto tile_to_fxy = [](TileIndex t) -> std::pair<float, float> {
			return {(float)TileX(t) / Map::SizeX(), (float)TileY(t) / Map::SizeY()};
		};
		if (has_train && st->train_station.tile != INVALID_TILE)
			infl.push_back(tile_to_fxy(st->train_station.GetCenterTile()));
		if (st->facilities.Test(StationFacility::Airport) && st->airport.tile != INVALID_TILE)
			infl.push_back(tile_to_fxy(st->airport.GetCenterTile()));
		if (st->facilities.Test(StationFacility::Dock) && st->docking_station.tile != INVALID_TILE)
			infl.push_back(tile_to_fxy(st->docking_station.GetCenterTile()));
		if (st->facilities.Test(StationFacility::BusStop) && st->bus_station.tile != INVALID_TILE)
			infl.push_back(tile_to_fxy(st->bus_station.GetCenterTile()));
		if (st->facilities.Test(StationFacility::TruckStop) && st->truck_station.tile != INVALID_TILE)
			infl.push_back(tile_to_fxy(st->truck_station.GetCenterTile()));
		/* Town that gave bonus. */
		if (bonus_town != nullptr && building_count >= 4) {
			infl.push_back(tile_to_fxy(bonus_town->xy));
		}
		/* Cluster partner. */
		if (cluster) {
			for (Station *other : Station::Iterate()) {
				if (other == st || other->xy == INVALID_TILE) continue;
				if (DistanceManhattan(st->xy, other->xy) > 5) continue;
				if (other->facilities.Test(StationFacility::Train) ||
						(other->facilities.Test(StationFacility::Airport) &&
						 other->airport.type != AT_HELIPORT && other->airport.type != AT_HELIDEPOT && other->airport.type != AT_HELISTATION)) {
					infl.push_back(tile_to_fxy(other->xy));
					break;
				}
			}
		}

		candidates.push_back({fx, fy, score, zoom, 5000, fmt::format("station: {}", reason), std::move(infl)});
	}

	/* --- Lighthouses (5% chance each) ------------------------------------ */
	for (uint y = 1; y < Map::SizeY() - 1; y++) {
		for (uint x = 1; x < Map::SizeX() - 1; x++) {
			TileIndex tile = TileXY(x, y);
			if (!IsObjectTypeTile(tile, OBJECT_LIGHTHOUSE)) continue;
			if ((std::rand() % 100) >= 5) continue;
			float fx = (float)x / Map::SizeX();
			float fy = (float)y / Map::SizeY();
			candidates.push_back({fx, fy, 100, 0, 5000, "lighthouse", {}});
		}
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

	/* BFS clustering: from each junction, expand through neighbours within 5 tiles,
	 * up to 5 hops. Only keep clusters with 5+ junctions. */
	std::vector<bool> visited(junctions.size(), false);
	for (size_t i = 0; i < junctions.size(); i++) {
		if (visited[i]) continue;

		/* BFS with depth limit of 5. */
		struct BFSEntry { size_t idx; int depth; };
		std::vector<BFSEntry> queue;
		std::vector<size_t> cluster;

		visited[i] = true;
		queue.push_back({i, 0});
		cluster.push_back(i);

		for (size_t qi = 0; qi < queue.size(); qi++) {
			auto [cur, depth] = queue[qi];
			if (depth >= 5) continue;

			for (size_t j = 0; j < junctions.size(); j++) {
				if (visited[j]) continue;
				uint dx = (junctions[j].x > junctions[cur].x) ? junctions[j].x - junctions[cur].x : junctions[cur].x - junctions[j].x;
				uint dy = (junctions[j].y > junctions[cur].y) ? junctions[j].y - junctions[cur].y : junctions[cur].y - junctions[j].y;
				if (dx + dy <= 5) {
					visited[j] = true;
					queue.push_back({j, depth + 1});
					cluster.push_back(j);
				}
			}
		}

		if ((int)cluster.size() < 5) continue;

		uint sum_x = 0, sum_y = 0;
		std::vector<std::pair<float, float>> infl;
		for (size_t idx : cluster) {
			sum_x += junctions[idx].x;
			sum_y += junctions[idx].y;
			infl.push_back({(float)junctions[idx].x / Map::SizeX(), (float)junctions[idx].y / Map::SizeY()});
		}
		int count = (int)cluster.size();
		int score = (count >= 8) ? 8 : 5;
		float fx = (float)(sum_x / count) / Map::SizeX();
		float fy = (float)(sum_y / count) / Map::SizeY();
		int zoom = (count >= 8) ? 1 : 0; // large cluster → In2x, otherwise In4x
		candidates.push_back({fx, fy, score, zoom, 5000,
			fmt::format("rail junction: {} junctions in cluster, score={}", count, score), std::move(infl)});
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
		Debug(driver, 0, "  POI[{}] score={} fx={:.2f} fy={:.2f} zoom={} — {}", i, p.score, p.map_fx, p.map_fy, p.zoom, p.reason);
	}
}

/** Force POI rescan on next PrepareBackground() call. */
void InvalidatePOIs()
{
	_gles_poi_list.clear();
	_gles_poi_idx = 0;
	_gles_poi_map_tiles = 0;
}

/** Whether user is manually browsing POIs (disables auto map rotation). */
static bool _poi_manual_browse = false;

/** Move camera to the current POI. */
static void ShowCurrentPOI()
{
	float fx, fy;
	ZoomLevel zoom = ZoomLevel::In4x;

	if (!_gles_poi_list.empty()) {
		const GlesPOI &poi = _gles_poi_list[_gles_poi_idx];
		fx   = poi.map_fx;
		fy   = poi.map_fy;
		zoom = static_cast<ZoomLevel>(poi.zoom);
		Debug(driver, 0, "GLES ShowCurrentPOI: POI[{}] score={} fx={:.2f} fy={:.2f} zoom={} — {}",
			_gles_poi_idx, poi.score, fx, fy, poi.zoom, poi.reason);
	} else {
		int idx = std::rand() % (int)kDefaultWaypoints.size();
		fx = kDefaultWaypoints[idx].map_fx;
		fy = kDefaultWaypoints[idx].map_fy;
	}

	/* Set zoom BEFORE scrolling — ScrollMainWindowTo uses virtual_width/height
	 * to compute the center offset, so zoom must be correct first. */
	Window *w = GetMainWindow();
	if (w != nullptr && w->viewport != nullptr) {
		ViewportData &vp = *w->viewport;
		vp.zoom = zoom;
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

/** Convert fractional map position to screen coords for a given viewport. */
static Point MapFracToScreen(float map_fx, float map_fy, const Viewport &vp)
{
	int wx = (int)(map_fx * Map::SizeX() * TILE_SIZE);
	int wy = (int)(map_fy * Map::SizeY() * TILE_SIZE);
	int wz = GetSlopePixelZ(
		Clamp(wx, 0, (int)Map::SizeX() * TILE_SIZE - 1),
		Clamp(wy, 0, (int)Map::SizeY() * TILE_SIZE - 1));
	Point p = RemapCoords(wx, wy, wz);
	p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
	p.y = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;
	return p;
}

void DrawPOIMarkers(const Viewport &vp)
{
	if (_gles_poi_list.empty()) return;

	const DrawPixelInfo *dpi = _cur_dpi;
	int half = 5;

	for (int i = 0; i < (int)_gles_poi_list.size(); i++) {
		const GlesPOI &poi = _gles_poi_list[i];
		Point sp = MapFracToScreen(poi.map_fx, poi.map_fy, vp);

		/* Broad clip — skip POIs far outside viewport. */
		if (sp.x + half < dpi->left || sp.x - half >= dpi->left + dpi->width) continue;
		if (sp.y + half < dpi->top  || sp.y - half >= dpi->top  + dpi->height) continue;

		/* Draw influence lines from POI center to each contributing object. */
		for (const auto &[ifx, ify] : poi.influences) {
			Point ip = MapFracToScreen(ifx, ify, vp);
			GfxDrawLine(sp.x, sp.y, ip.x, ip.y, PC_YELLOW, 1, 0);
		}

		int h2 = half + 2;
		GfxFillRect(sp.x - h2, sp.y - h2, sp.x + h2, sp.y + h2, PC_RED);
		if (i == _gles_poi_idx) {
			GfxFillRect(sp.x - half, sp.y - half, sp.x + half, sp.y + half, PC_WHITE);
		}
	}
}
