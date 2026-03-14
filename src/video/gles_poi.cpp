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
#include "../vehicle_base.h"
#include "../aircraft.h"
#include "../openttd.h"
#include "../wallpaper.h"
#include "../landscape.h"
#include "../gfx_func.h"
#include "../palette_func.h"
#include "gles_poi.h"
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

/** Scanned POI list (top 20 by score), rebuilt when map changes. */
static std::vector<GlesPOI> _gles_poi_list;
static int _gles_poi_idx = 0;
static uint _gles_poi_map_tiles = 0; ///< Map::SizeX()*SizeY() at last scan; triggers rescan on change.

/** Convert a tile to fractional map coordinates. */
static std::pair<float, float> TileToFxy(TileIndex t)
{
	return {(float)TileX(t) / Map::SizeX(), (float)TileY(t) / Map::SizeY()};
}

/** Check if a non-heliport airport exists at this station. */
static bool IsRealAirport(const Station *st)
{
	if (!st->facilities.Test(StationFacility::Airport)) return false;
	uint8_t at = st->airport.type;
	return at != AT_HELIPORT && at != AT_HELIDEPOT && at != AT_HELISTATION;
}

/** Check if a small train station (<3 tiles) is interesting enough to include. */
static bool IsSmallStationInteresting(const Station *st, std::string &reason)
{
	if (CountTownBuildings(st->xy, 3, nullptr) < 4) return false;

	/* Another train station within 10 tiles. */
	for (Station *other : Station::Iterate()) {
		if (other == st || other->xy == INVALID_TILE) continue;
		if (other->facilities.Test(StationFacility::Train) && DistanceManhattan(st->xy, other->xy) <= 10) {
			return true;
		}
	}

	/* Different rail type within 5 tiles. */
	if (st->train_station.tile != INVALID_TILE) {
		RailType st_rt = GetRailType(st->train_station.tile);
		uint cx = TileX(st->xy), cy = TileY(st->xy);
		for (uint dy = (cy > 5 ? cy - 5 : 0); dy <= std::min(cy + 5, Map::SizeY() - 1); dy++) {
			for (uint dx = (cx > 5 ? cx - 5 : 0); dx <= std::min(cx + 5, Map::SizeX() - 1); dx++) {
				TileIndex t = TileXY(dx, dy);
				if (IsPlainRailTile(t) && GetRailType(t) != st_rt) {
					reason += "mixed-rail ";
					return true;
				}
			}
		}
	}
	return false;
}

/** Check if dock is interesting: adjacent road + 5+ buildings, or train station within 3 tiles. */
static bool IsDockInteresting(TileIndex dock_tile)
{
	/* Adjacent road tile. */
	bool road_adj = false;
	static const int ddx[] = {0, 1, 0, -1};
	static const int ddy[] = {-1, 0, 1, 0};
	for (int d = 0; d < 4 && !road_adj; d++) {
		uint nx = TileX(dock_tile) + ddx[d], ny = TileY(dock_tile) + ddy[d];
		if (nx < Map::SizeX() && ny < Map::SizeY() && IsTileType(TileXY(nx, ny), TileType::Road)) road_adj = true;
	}

	/* Train station within 3 tiles. */
	for (Station *other : Station::Iterate()) {
		if (other->xy == INVALID_TILE) continue;
		if (other->facilities.Test(StationFacility::Train) && DistanceManhattan(dock_tile, other->xy) <= 3) {
			return true;
		}
	}

	return road_adj && CountTownBuildings(dock_tile, 3, nullptr) > 5;
}

/** Check if station forms a cluster with another train station or airport within 5 tiles. */
static bool IsStationCluster(const Station *st)
{
	if (!st->facilities.Test(StationFacility::Train)) return false;
	for (Station *other : Station::Iterate()) {
		if (other == st || other->xy == INVALID_TILE) continue;
		if (DistanceManhattan(st->xy, other->xy) > 5) continue;
		if (other->facilities.Test(StationFacility::Train) || IsRealAirport(other)) return true;
	}
	return false;
}

/** Pick the best tile to represent this station as a POI. */
static TileIndex PickStationPOITile(const Station *st, bool has_train)
{
	if (has_train && st->train_station.tile != INVALID_TILE) return st->train_station.GetCenterTile();
	if (st->facilities.Test(StationFacility::Airport) && st->airport.tile != INVALID_TILE) return st->airport.GetCenterTile();
	if (st->facilities.Test(StationFacility::Dock) && st->docking_station.tile != INVALID_TILE) return st->docking_station.GetCenterTile();
	if (st->facilities.Test(StationFacility::BusStop) && st->bus_station.tile != INVALID_TILE) return st->bus_station.GetCenterTile();
	if (st->facilities.Test(StationFacility::TruckStop) && st->truck_station.tile != INVALID_TILE) return st->truck_station.GetCenterTile();
	return st->xy;
}

/** Collect influence points from all scored facilities of a station. */
static std::vector<std::pair<float, float>> CollectStationInfluences(
	const Station *st, bool has_train, bool cluster, Town *bonus_town, int building_count)
{
	std::vector<std::pair<float, float>> infl;
	if (has_train && st->train_station.tile != INVALID_TILE)
		infl.push_back(TileToFxy(st->train_station.GetCenterTile()));
	if (st->facilities.Test(StationFacility::Airport) && st->airport.tile != INVALID_TILE)
		infl.push_back(TileToFxy(st->airport.GetCenterTile()));
	if (st->facilities.Test(StationFacility::Dock) && st->docking_station.tile != INVALID_TILE)
		infl.push_back(TileToFxy(st->docking_station.GetCenterTile()));
	if (st->facilities.Test(StationFacility::BusStop) && st->bus_station.tile != INVALID_TILE)
		infl.push_back(TileToFxy(st->bus_station.GetCenterTile()));
	if (st->facilities.Test(StationFacility::TruckStop) && st->truck_station.tile != INVALID_TILE)
		infl.push_back(TileToFxy(st->truck_station.GetCenterTile()));
	if (bonus_town != nullptr && building_count >= 4)
		infl.push_back(TileToFxy(bonus_town->xy));
	if (cluster) {
		for (Station *other : Station::Iterate()) {
			if (other == st || other->xy == INVALID_TILE) continue;
			if (DistanceManhattan(st->xy, other->xy) > 5) continue;
			if (other->facilities.Test(StationFacility::Train) || IsRealAirport(other)) {
				infl.push_back(TileToFxy(other->xy));
				break;
			}
		}
	}
	return infl;
}

/** Scan stations and emit POI candidates. */
static void ScanStationPOIs(std::vector<GlesPOI> &candidates)
{
	for (Station *st : Station::Iterate()) {
		if (st->xy == INVALID_TILE) continue;

		int score = 0;
		int transport_facilities = 0;
		std::string reason;

		bool has_train = st->facilities.Test(StationFacility::Train);
		if (has_train) {
			int platform_len = std::max(st->train_station.w, st->train_station.h);
			if (platform_len < 3 && !IsSmallStationInteresting(st, reason)) {
				has_train = false;
			}
		}
		if (has_train) { score += 5; transport_facilities++; reason += "train+5 "; }

		if (IsRealAirport(st)) {
			TileIndex ap_tile = st->airport.tile != INVALID_TILE ? st->airport.GetCenterTile() : st->xy;
			if (CountTownBuildings(ap_tile, 5, nullptr) >= 10) {
				score += 3; transport_facilities++; reason += "airport+3 ";
			}
		}

		if (st->facilities.Test(StationFacility::Dock)) {
			TileIndex dock_tile = st->docking_station.tile != INVALID_TILE ? st->docking_station.GetCenterTile() : st->xy;
			if (IsDockInteresting(dock_tile)) {
				score += 2; transport_facilities++; reason += "dock+2 ";
			}
		}

		if (st->facilities.Test(StationFacility::BusStop))   { score += 1; reason += "bus+1 "; }
		if (st->facilities.Test(StationFacility::TruckStop) && transport_facilities >= 2) { score += 1; reason += "truck+1 "; }
		if (score == 0) continue;

		bool cluster = IsStationCluster(st);
		TileIndex poi_tile = PickStationPOITile(st, has_train);

		Town *bonus_town = nullptr;
		int building_count = CountTownBuildings(poi_tile, 5, &bonus_town);
		if (building_count >= 4 && bonus_town != nullptr) {
			int town_bonus = std::min(5, (int)(bonus_town->cache.population / 500));
			score += town_bonus;
			if (town_bonus > 0) reason += fmt::format("town(pop={},bld={})+" "{} ", bonus_town->cache.population, building_count, town_bonus);
		}

		auto [fx, fy] = TileToFxy(poi_tile);
		int zoom = cluster ? 1 : 0;
		if (cluster) { score += 3; reason += "cluster+3 "; }

		auto infl = CollectStationInfluences(st, has_train, cluster, bonus_town, building_count);
		candidates.push_back({fx, fy, score, zoom, 5000, fmt::format("station: {}", reason), std::move(infl)});
	}
}

/** Scan lighthouses and emit POI candidates (5% chance each). */
/** Pick one random lighthouse from the map (if any exist). */
static void ScanLighthousePOIs(std::vector<GlesPOI> &candidates)
{
	std::vector<std::pair<uint, uint>> lighthouses;
	for (uint y = 1; y < Map::SizeY() - 1; y++) {
		for (uint x = 1; x < Map::SizeX() - 1; x++) {
			if (IsObjectTypeTile(TileXY(x, y), OBJECT_LIGHTHOUSE)) {
				lighthouses.push_back({x, y});
			}
		}
	}
	if (lighthouses.empty() || (std::rand() % 100) >= 5) return;

	auto &[x, y] = lighthouses[std::rand() % lighthouses.size()];
	float fx = (float)x / Map::SizeX();
	float fy = (float)y / Map::SizeY();
	candidates.push_back({fx, fy, 100, 0, 5000, "lighthouse", {}});
}

/** Scan rail junctions, BFS-cluster them, and emit POI candidates. */
static void ScanJunctionPOIs(std::vector<GlesPOI> &candidates)
{
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

	std::vector<bool> visited(junctions.size(), false);
	for (size_t i = 0; i < junctions.size(); i++) {
		if (visited[i]) continue;

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
		int zoom = (count >= 8) ? 1 : 0;
		candidates.push_back({fx, fy, score, zoom, 5000,
			fmt::format("rail junction: {} junctions in cluster, score={}", count, score), std::move(infl)});
	}
}

/** Scan towns not already covered by a nearby station POI. */
static void ScanTownPOIs(std::vector<GlesPOI> &candidates)
{
	for (Town *t : Town::Iterate()) {
		if (t->xy == INVALID_TILE || t->cache.population < 500) continue;

		bool covered = false;
		for (const GlesPOI &poi : candidates) {
			TileIndex poi_tile = TileXY(
				(uint)(poi.map_fx * Map::SizeX()),
				(uint)(poi.map_fy * Map::SizeY()));
			if (DistanceManhattan(t->xy, poi_tile) < 30) { covered = true; break; }
		}
		if (covered) continue;

		int score = std::min(5, (int)(t->cache.population / 500));
		auto [fx, fy] = TileToFxy(t->xy);
		candidates.push_back({fx, fy, score, 0, 5000,
			fmt::format("town: pop={}, score={}", t->cache.population, score)});
	}
}

/** Scan vehicles and emit a "follow vehicle" POI (20% chance). */
static void ScanVehiclePOIs(std::vector<GlesPOI> &candidates)
{
	if ((std::rand() % 100) >= 20) return;

	/* Collect all front vehicles (trains, road vehicles, ships, aircraft). */
	std::vector<const Vehicle *> front_vehicles;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle()) continue;
		if (v->vehstatus.Test(VehState::Stopped)) continue;
		if (v->vehstatus.Test(VehState::Hidden)) continue;
		front_vehicles.push_back(v);
	}
	if (front_vehicles.empty()) return;

	const Vehicle *veh = front_vehicles[std::rand() % front_vehicles.size()];
	auto [fx, fy] = TileToFxy(veh->tile);

	static const char *vtype_names[] = {"train", "road vehicle", "ship", "aircraft"};
	const char *vtype = (veh->type <= VEH_AIRCRAFT) ? vtype_names[veh->type] : "vehicle";

	GlesPOI poi;
	poi.map_fx = fx;
	poi.map_fy = fy;
	poi.score = 50;
	poi.zoom = 0;
	poi.delay_ms = 8000;
	poi.reason = fmt::format("follow {}: vehicle #{}", vtype, veh->index);
	poi.follow_vehicle = veh->index;
	candidates.push_back(std::move(poi));
}

/** Scan all POI types, sort by score, pick 20 random from top 50. */
static void ScanMapPOIs()
{
	_gles_poi_list.clear();
	_gles_poi_map_tiles = Map::SizeX() * Map::SizeY();

	std::vector<GlesPOI> candidates;
	candidates.reserve(64);

	ScanStationPOIs(candidates);
	int n_stations = (int)candidates.size();
	ScanLighthousePOIs(candidates);
	int n_lighthouses = (int)candidates.size() - n_stations;
	ScanJunctionPOIs(candidates);
	int n_junctions = (int)candidates.size() - n_stations - n_lighthouses;
	ScanTownPOIs(candidates);
	int n_towns = (int)candidates.size() - n_stations - n_lighthouses - n_junctions;
	ScanVehiclePOIs(candidates);
	int n_vehicles = (int)candidates.size() - n_stations - n_lighthouses - n_junctions - n_towns;
	Debug(driver, 0, "GLES POI candidates: {} total (stations={} lighthouses={} junctions={} towns={} vehicles={})",
		(int)candidates.size(), n_stations, n_lighthouses, n_junctions, n_towns, n_vehicles);

	/* Remove POIs within 20 tiles of map edge (skip vehicle-follow POIs). */
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
		[](const GlesPOI &c) {
			if (c.follow_vehicle != VehicleID::Invalid()) return false;
			const uint edge_margin = 20;
			uint tx = (uint)(c.map_fx * Map::SizeX());
			uint ty = (uint)(c.map_fy * Map::SizeY());
			return tx < edge_margin || ty < edge_margin ||
			       tx >= Map::SizeX() - edge_margin || ty >= Map::SizeY() - edge_margin;
		}), candidates.end());

	int n_after_edge = (int)candidates.size();
	Debug(driver, 0, "GLES POI after edge filter: {} (removed {})", n_after_edge,
		n_stations + n_lighthouses + n_junctions + n_towns + n_vehicles - n_after_edge);

	std::sort(candidates.begin(), candidates.end(),
		[](const GlesPOI &a, const GlesPOI &b) { return a.score > b.score; });

	/* Build top-50 pool, skipping any within 10 tiles of an already selected entry.
	 * Vehicle-follow POIs skip distance check (they move). */
	std::vector<GlesPOI> top_pool;
	for (const auto &c : candidates) {
		if ((int)top_pool.size() >= 50) break;
		if (c.follow_vehicle == VehicleID::Invalid()) {
			TileIndex ct = TileXY(
				(uint)(c.map_fx * Map::SizeX()),
				(uint)(c.map_fy * Map::SizeY()));
			bool too_close = false;
			for (const auto &sel : top_pool) {
				TileIndex st = TileXY(
					(uint)(sel.map_fx * Map::SizeX()),
					(uint)(sel.map_fy * Map::SizeY()));
				if (DistanceManhattan(ct, st) < 10) { too_close = true; break; }
			}
			if (too_close) continue;
		}
		top_pool.push_back(c);
	}

	Debug(driver, 0, "GLES POI top-50 pool (10-tile dedup): {} entries", (int)top_pool.size());

	/* Pick 20 random from the pool. */
	if ((int)top_pool.size() <= 20) {
		_gles_poi_list = std::move(top_pool);
	} else {
		/* Fisher-Yates partial shuffle: pick 20 random entries. */
		for (int i = 0; i < 20; i++) {
			int j = i + (std::rand() % ((int)top_pool.size() - i));
			std::swap(top_pool[i], top_pool[j]);
		}
		_gles_poi_list.assign(top_pool.begin(), top_pool.begin() + 20);
	}
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
	if (_gles_poi_list.empty()) return;

	const GlesPOI &poi = _gles_poi_list[_gles_poi_idx];
	ZoomLevel zoom = static_cast<ZoomLevel>(poi.zoom);
	Debug(driver, 0, "GLES ShowCurrentPOI: POI[{}] score={} fx={:.2f} fy={:.2f} zoom={} — {}",
		_gles_poi_idx, poi.score, poi.map_fx, poi.map_fy, poi.zoom, poi.reason);

	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr) return;

	ViewportData &vp = *w->viewport;

	/* Set zoom BEFORE scrolling — ScrollMainWindowTo uses virtual_width/height
	 * to compute the center offset, so zoom must be correct first. */
	vp.zoom = zoom;
	vp.virtual_width = ScaleByZoom(vp.width, vp.zoom);
	vp.virtual_height = ScaleByZoom(vp.height, vp.zoom);

	// TODO: temporarily disabled vehicle following
	// if (poi.follow_vehicle != VehicleID::Invalid() && Vehicle::IsValidID(poi.follow_vehicle)) {
	// 	const Vehicle *veh = Vehicle::Get(poi.follow_vehicle);
	// 	vp.follow_vehicle = poi.follow_vehicle;
	// 	Point pt = RemapCoords(veh->x_pos, veh->y_pos, veh->z_pos);
	// 	vp.dest_scrollpos_x = pt.x - vp.virtual_width / 2;
	// 	vp.dest_scrollpos_y = pt.y - vp.virtual_height / 2;
	// } else {
		vp.follow_vehicle = VehicleID::Invalid();

		int world_x = (int)(poi.map_fx * Map::SizeX() * TILE_SIZE);
		int world_y = (int)(poi.map_fy * Map::SizeY() * TILE_SIZE);
		Point pt = RemapCoords(world_x, world_y, 0);
		int x = pt.x - vp.virtual_width / 2;
		int y = pt.y - vp.virtual_height / 2;
		Debug(driver, 0, "GLES POI camera: world={},{} remap={},{} vw={} vh={} scroll={},{} prev={},{} zoom={}",
			world_x, world_y, pt.x, pt.y, vp.virtual_width, vp.virtual_height, x, y,
			vp.scrollpos_x, vp.scrollpos_y, (int)vp.zoom);
		vp.scrollpos_x = x;
		vp.scrollpos_y = y;
		vp.dest_scrollpos_x = x;
		vp.dest_scrollpos_y = y;
	// }

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

	_poi_manual_browse = false;

	/* Advance to next POI first, then check if we wrapped around. */
	if (!_gles_poi_list.empty()) {
		_gles_poi_idx = (_gles_poi_idx + 1) % (int)_gles_poi_list.size();
	}

	/* Wrapped back to POI[0] — all POIs shown, rotate to next title map. */
	if (!_poi_manual_browse && _gles_poi_idx == 0 && !_gles_poi_list.empty() &&
			_switch_mode == SM_NONE && CanRotateTitleMap()) {
		Debug(driver, 0, "GLES PrepareBackground: all POIs shown, rotating to next title map");
		RequestNextTitleMap();
		return;
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
