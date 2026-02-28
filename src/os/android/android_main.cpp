/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file android_main.cpp Main entry for Android via SDL2. */

#include "../../stdafx.h"
#include "../../openttd.h"
#include "../../crashlog.h"
#include "../../core/random_func.hpp"
#include "../../string_func.h"

#include <time.h>
#include <signal.h>
#include <android/log.h>

#include "../../safeguards.h"

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "OpenTTD", __VA_ARGS__)

/**
 * SDL2 on Android calls SDL_main (looked up via dlsym) instead of main().
 * We define it explicitly to avoid any macro conflicts with safeguards.h.
 */
extern "C" int SDL_main(int argc, char *argv[])
{
	ALOG("SDL_main entered, argc=%d", argc);
	for (int i = 0; i < argc; ++i) {
		ALOG("  argv[%d] = %s", i, argv[i]);
	}

	std::vector<std::string_view> params;
	for (int i = 0; i < argc; ++i) {
		StrMakeValidInPlace(argv[i]);
		params.emplace_back(argv[i]);
	}

	const char *dataPath = getenv("OPENTTD_DATA_PATH");
	ALOG("OPENTTD_DATA_PATH = %s", dataPath ? dataPath : "(null)");

	CrashLog::InitialiseCrashLog();

	SetRandomSeed(time(nullptr));

	signal(SIGPIPE, SIG_IGN);

	ALOG("Calling openttd_main...");
	int ret = openttd_main(params);
	ALOG("openttd_main returned %d", ret);
	return ret;
}
