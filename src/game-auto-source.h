// game-auto-source.h — shared declaration of the OBS source vtable so both
// plugin-main.cpp and game-auto-source.cpp see the same symbol.
#pragma once
#include <obs.h>

extern struct obs_source_info game_auto_source_info;
