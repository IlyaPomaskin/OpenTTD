/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_poi.cpp Stage-1 STUB — camera centers on map. Real POI scanner lands in Stage 2 (see docs/wallpaper/wallpaper-mode.md). */

#include "../stdafx.h"
#include "gles_poi.h"
#include "../map_func.h"
#include "../viewport_func.h"
#include "../window_func.h"
#include "../window_gui.h"
#include "../debug.h"

#include "../safeguards.h"

void PrepareBackground()
{
	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr) return;
	ScrollWindowToTile(TileXY(Map::SizeX() / 2, Map::SizeY() / 2), w, true);
	Debug(misc, 1, "POI(stub): centered on map");
}

void NavigatePOI(int) { PrepareBackground(); }
void RecenterOnCurrentPOI() { PrepareBackground(); }
void InvalidatePOIs() {}
void DrawPOIMarkers(const Viewport &) {}
