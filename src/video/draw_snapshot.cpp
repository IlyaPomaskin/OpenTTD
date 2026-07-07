/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file draw_snapshot.cpp Recording of draw commands for two-thread snapshot rendering. */

#include "../stdafx.h"
#include "draw_snapshot.h"
#include "gles_perf.h"
#include "../safeguards.h"

GLESPerfCounters _gles_perf;
bool _gles_video_active = false;
bool _gles_context_lost = false;

static DrawSnapshot *_recording_snapshot = nullptr;

void StartRecording(DrawSnapshot &snapshot)
{
	snapshot.Clear();
	_recording_snapshot = &snapshot;
}

void StopRecording() { _recording_snapshot = nullptr; }
bool IsRecording() { return _recording_snapshot != nullptr; }

void RecordCommand(const DrawCommand &cmd)
{
	if (_recording_snapshot != nullptr) _recording_snapshot->commands.push_back(cmd);
}
