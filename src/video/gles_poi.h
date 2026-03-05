/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gles_poi.h POI scanner and camera rotation for wallpaper/intro mode. */

#ifndef GLES_POI_H
#define GLES_POI_H

/**
 * Advance the camera to the next scored POI on the current map.
 *
 * Scans all stations and towns on first call (or when the map changes),
 * scores them by transport facilities and nearby population, keeps the
 * top 10, and cycles through them in score order.  Falls back to the
 * static kDefaultWaypoints array when the map has no stations or towns.
 *
 * Safe to call from any thread that holds the game state (i.e. the main
 * game / GL thread).  No-op when the map is empty.
 */
void PrepareBackground();
void NavigatePOI(int delta);
void InvalidatePOIs();
void DrawPOIMarkers(const struct Viewport &vp);

#endif /* GLES_POI_H */
