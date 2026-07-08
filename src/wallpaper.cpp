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
#include "gfx_func.h"
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
#include "viewport_func.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#ifdef WALLPAPER_BUILD
#include "video/gles_backend.h"
#endif

#include "safeguards.h"

/** All available title files for rotation: {filename, subdir}. */
static std::vector<std::pair<std::string, Subdirectory>> _title_files;
static size_t _title_file_idx = 0;

#ifdef __ANDROID__
extern std::atomic<bool> _gles_interval_active;
#endif

/**
 * Build the list of all available title map files (called once).
 * Collects opntitle.dat and title/*.sav from search paths.
 */
void BuildTitleFileList()
{
	_title_files.clear();

	/* 1. Default baseset title screen (loaded first). */
	_title_files.push_back({"opntitle.dat", Subdirectory::Baseset});

	/* 2. Scan title/ directory for .sav files. */
	std::set<std::string> seen;
	auto scan_title_dir = [&](const std::string &dir) {
		std::string title_dir = dir + "title";
		std::error_code ec;
		if (!std::filesystem::is_directory(title_dir, ec)) return;
		for (const auto &entry : std::filesystem::directory_iterator(title_dir, ec)) {
			if (!entry.is_regular_file()) continue;
			auto ext = entry.path().extension().string();
			for (auto &c : ext) c = tolower(c);
			if (ext != ".sav") continue;
			std::string path = entry.path().string();
			if (seen.insert(entry.path().filename().string()).second) {
				_title_files.push_back({path, Subdirectory::None});
			}
		}
	};

	auto data_env = GetEnv("OPENTTD_DATA_PATH");
	if (data_env.has_value()) {
		scan_title_dir(std::string(*data_env) + PATHSEP);
	}
	for (Searchpath sp : _valid_searchpaths) {
		scan_title_dir(FioGetDirectory(sp, Subdirectory::Base));
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
#ifdef __ANDROID__
	/* An interval index 1-4 owns cadence; suppress the POI-wrap auto-rotate. */
	if (_gles_interval_active.load()) return;
#endif
	RotateTitleMap(1);
}

void RotateTitleMap(int delta)
{
	if (!CanRotateTitleMap()) return;
	int n = (int)_title_files.size();
	_title_file_idx = ((_title_file_idx + delta) % n + n) % n;
	_switch_mode = (_game_mode == GameMode::Wallpaper) ? SwitchMode::Wallpaper : SwitchMode::Menu;
}

/**
 * Rebuild the title file list after an import/delete and clamp the index.
 * Called from the GL-thread overlay-action drain under game_state_mutex.
 */
void RefreshTitleMaps()
{
	BuildTitleFileList();
	if (_title_file_idx >= _title_files.size()) {
		_title_file_idx = _title_files.empty() ? 0 : _title_files.size() - 1;
	}
	Debug(misc, 0, "RefreshTitleMaps: {} files, idx={}", _title_files.size(), _title_file_idx);
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
		Debug(misc, 0, "[LOAD] map_load_start: [{}] {}", idx, file);
		auto t0 = std::chrono::steady_clock::now();
		SaveLoadResult result = SaveOrLoad(file, SaveLoadOperation::Load, DetailedFileType::GameFile, subdir);
		auto t1 = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
		Debug(misc, 0, "[LOAD] map_load_end: [{}] {} result={} time={}ms", idx, file, static_cast<int>(result), ms);
		if (result == SaveLoadResult::Ok) {
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
	Debug(misc, 0, "[LOAD] wallpaper_load_start: screen={}x{}", _screen.width, _screen.height);
	_game_mode = GameMode::Wallpaper;
	InvalidatePOIs();

	/* Request GLES sprite atlas clear (deferred to GL thread). */
#ifdef WALLPAPER_BUILD
	if (GLESBackend::Get() != nullptr) {
		GLESBackend::Get()->GetSpriteAtlas().RequestClear();
	}
#endif

	ResetWindowSystem();
	SetupColoursAndInitialWindow();

	if (!LoadNextTitleMap()) {
		Debug(misc, 0, "[LOAD] map_generate: no title maps, generating empty 64x64");
		GenerateWorld(GWM_EMPTY, 64, 64);
	}

	SetLocalCompany(COMPANY_SPECTATOR);
	_pause_mode = {};
	_cursor.fix_at = false;

	FixTitleGameZoom(-1);
	PrepareBackground();
	Debug(misc, 0, "[LOAD] wallpaper_load_end: screen={}x{}", _screen.width, _screen.height);
}
