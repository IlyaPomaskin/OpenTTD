/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallpaper.cpp Title map rotation and wallpaper mode. */

#include "stdafx.h"
#include "wallpaper.h"
#include "openttd.h"
#include "debug.h"
#include "fileio_func.h"
#include "fios.h"
#include "saveload/saveload.h"
#include "company_func.h"
#include "genworld.h"
#include "window_func.h"
#include "gui.h"
#include "string_func.h"
#include "video/gles_poi.h"
#include <chrono>
#include "video/gles_backend.h"

#include "safeguards.h"

/** All available title files for rotation: {filename, subdir}. */
static std::vector<std::pair<std::string, Subdirectory>> _title_files;
static size_t _title_file_idx = 0;

/**
 * Build the list of all available title map files (called once).
 * Collects opntitle.dat and title/*.sav from search paths.
 */
void BuildTitleFileList()
{
	_title_files.clear();

	/* 1. Default baseset title screen. */
	_title_files.push_back({"opntitle.dat", BASESET_DIR});

	/* 2. title/*.sav files from search paths. */
	static const char *title_savs[] = {
		"title/2TallTyler-Title15.sav",
		"title/EratoTitle15.sav",
		"title/title_15.sav",
	};

	for (const char *rel : title_savs) {
		auto data_env = GetEnv("OPENTTD_DATA_PATH");
		if (data_env.has_value()) {
			std::string full = std::string(*data_env) + PATHSEP + rel;
			if (FioCheckFileExists(full, NO_DIRECTORY)) {
				_title_files.push_back({full, NO_DIRECTORY});
				continue;
			}
		}
		for (Searchpath sp : _valid_searchpaths) {
			std::string full = FioGetDirectory(sp, BASE_DIR) + rel;
			if (FioCheckFileExists(full, NO_DIRECTORY)) {
				_title_files.push_back({full, NO_DIRECTORY});
				break;
			}
		}
	}

	Debug(misc, 0, "BuildTitleFileList: {} title files", _title_files.size());
	for (size_t i = 0; i < _title_files.size(); i++) {
		Debug(misc, 0, "  [{}] {}", i, _title_files[i].first);
	}
}

bool CanRotateTitleMap()
{
	return _title_files.size() >= 1;
}

void RequestNextTitleMap()
{
	RotateTitleMap(1);
}

void RotateTitleMap(int delta)
{
	if (!CanRotateTitleMap()) return;
	int n = (int)_title_files.size();
	_title_file_idx = ((_title_file_idx + delta) % n + n) % n;
	_switch_mode = (_game_mode == GM_WALLPAPER) ? SM_WALLPAPER : SM_MENU;
}

/**
 * Try loading a title map starting from the current index.
 * Advances _title_file_idx to the file that loaded successfully.
 * @return true if a map was loaded, false if all files failed.
 */
bool LoadNextTitleMap()
{
	if (_title_files.empty()) BuildTitleFileList();

	size_t attempts = _title_files.size();
	for (size_t i = 0; i < attempts; i++) {
		size_t idx = (_title_file_idx + i) % _title_files.size();
		const auto &[file, subdir] = _title_files[idx];
		auto t0 = std::chrono::steady_clock::now();
		SaveOrLoadResult result = SaveOrLoad(file, SLO_LOAD, DFT_GAME_FILE, subdir);
		auto t1 = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
		Debug(misc, 0, "LoadNextTitleMap: [{}] {} result={} time={}ms", idx, file, static_cast<int>(result), ms);
		if (result == SL_OK) {
			_title_file_idx = idx;
			return true;
		}
	}
	return false;
}

/**
 * Load a title map in wallpaper mode.
 *
 * Minimal initialization: reset windows, load a title savegame,
 * scan POIs and position the camera.  No NewGRF reload, no network,
 * no sound/music.
 */
void LoadWallpaperGame()
{
	// Debug(misc, 0, "LoadWallpaperGame: entering, GLESBackend={}", GLESBackend::Get() != nullptr ? "present" : "null");
	_game_mode = GM_WALLPAPER;
	InvalidatePOIs();

	/* Request GLES sprite atlas clear (deferred to GL thread). */
	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->GetSpriteAtlas().RequestClear();
	}

	ResetWindowSystem();
	SetupColoursAndInitialWindow();

	if (!LoadNextTitleMap()) {
		GenerateWorld(GWM_EMPTY, 64, 64);
	}

	SetLocalCompany(COMPANY_SPECTATOR);
	_pause_mode = {};
	_cursor.fix_at = false;

	PrepareBackground();
}
