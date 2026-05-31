// plugin-log.h — shared log macro so every line is greppable with the
// [win-game-auto] prefix in %APPDATA%\obs-studio\logs\.
#pragma once
#include <obs.h>

#define PLUGIN_LOG(level, fmt, ...) blog(level, "[win-game-auto] " fmt, ##__VA_ARGS__)
