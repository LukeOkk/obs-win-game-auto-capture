// plugin-main.cpp — module entry point for the Windows port.
#include <obs-module.h>
#include "game-auto-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-win-game-auto-capture", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Auto-detects and captures Windows games via Game Bar / Game Mode "
           "using Windows.Graphics.Capture";
}

MODULE_EXPORT const char *obs_module_name(void)
{
    return "Windows Game Auto Capture";
}

bool obs_module_load(void)
{
    obs_register_source(&game_auto_source_info);
    blog(LOG_INFO, "[win-game-auto] loaded v0.1.0");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[win-game-auto] unloaded");
}
