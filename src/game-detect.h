// game-detect.h — game detection for the Windows port.
//
// Detection uses two Windows-native signals plus a manual whitelist:
//   * Xbox Game Bar recognition  — games the Game Bar has registered are
//     persisted under HKCU\System\GameConfigStore\Children. We match the
//     foreground executable against those entries (MatchedExeFullPath /
//     ExeParentDirectory). This is read-only and runtime-only; we never copy,
//     persist, or transmit the registry contents and only ever log the exe
//     base name (never full user paths or game titles).
//   * Game Mode (fullscreen D3D) — SHQueryUserNotificationState() reports
//     QUNS_RUNNING_D3D_FULL_SCREEN when a full-screen Direct3D game is running,
//     the closest public proxy for "Game Mode would be active right now".
//
// Nothing here is OBS-specific so it can be unit-tested on its own.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

// A snapshot of the current foreground application.
struct fg_target {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exe_path;  // full image path, original case
    std::wstring exe_name;  // base name, lower-cased (e.g. L"game.exe")
    RECT rect{};            // window rect in screen coords
    bool fullscreen = false;// window covers its monitor (borderless fullscreen)
    bool valid = false;     // false if there is no usable foreground app
};

// Settings that influence the accept/reject decision. Mirrors the relevant
// toggles exposed in the OBS source properties.
struct game_detect_settings {
    bool use_game_bar = true;             // honor Game Bar recognition
    bool use_game_mode = true;            // honor fullscreen-D3D / Game Mode
    bool strict_game_mode_only = true;    // only capture recognized games
    bool allow_fullscreen_fallback = false; // when not strict: any fullscreen non-launcher
    bool keep_capturing_on_alt_tab = true;  // (used by the orchestrator, not here)
    std::vector<std::wstring> whitelist;  // exe base names or full paths, lower-cased
};

// Returns the current foreground target. valid==false when there is nothing
// worth considering (no foreground window, or the foreground is our own
// process / the desktop shell).
fg_target get_foreground_target();

// Result of the accept/reject cascade. reason is a stable ASCII literal for
// logging (never contains user paths).
struct game_decision {
    bool accept = false;
    const char *reason = "";
};

// The accept/reject cascade. First matching rule wins. See game-detect.cpp.
game_decision is_likely_game(const fg_target &t, const game_detect_settings &s);

// Individual signals — exposed so the OBS layer (and a future test harness)
// can log which rule fired.
bool is_gamebar_recognized(const std::wstring &exe_path);  // Game Bar
bool is_game_mode_fullscreen();                            // Game Mode
bool is_launcher_exe(const std::wstring &exe_name_lower);
bool is_known_game_exe(const std::wstring &exe_name_lower);

// One running, windowed process — used to populate the whitelist picker.
struct running_app {
    std::wstring exe_name;  // base name, original case for display
    std::wstring title;     // window title, for display
};
std::vector<running_app> enumerate_running_apps();

// Small string helpers (also used by the OBS layer).
std::wstring to_lower(std::wstring s);
std::wstring basename_of(const std::wstring &path);
