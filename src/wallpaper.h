/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallpaper.h Title map rotation and wallpaper mode. */

#ifndef WALLPAPER_H
#define WALLPAPER_H

void BuildTitleFileList();
bool CanRotateTitleMap();
void RequestNextTitleMap();
void RotateTitleMap(int delta);
void RefreshTitleMaps();
void LoadWallpaperGame();

#ifdef WALLPAPER_BUILD
void WallpaperReadConfig();
#endif

/** Try loading a title map starting from the current index. Returns true on success. */
bool LoadNextTitleMap();

#endif /* WALLPAPER_H */
