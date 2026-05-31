// game-detect.cpp — see game-detect.h for the design overview.
#include "game-detect.h"

#include <shellapi.h>   // SHQueryUserNotificationState, QUNS_RUNNING_D3D_FULL_SCREEN
#include <algorithm>
#include <cwctype>
#include <cwchar>       // wcsnlen
#include <cstdlib>      // _countof

#pragma comment(lib, "shell32.lib")

// --------------------------------------------------------------------------
// String helpers
// --------------------------------------------------------------------------

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring basename_of(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

static std::wstring dirname_of(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

// --------------------------------------------------------------------------
// Static signal tables
// --------------------------------------------------------------------------

// Launcher / shell / utility executables we never want to capture even when
// they are frontmost (so the user streams the game, not the store UI). All
// entries are lower-case base names.
static const wchar_t *kLauncherExes[] = {
    L"steam.exe", L"steamwebhelper.exe",
    L"epicgameslauncher.exe", L"epicwebhelper.exe",
    L"galaxyclient.exe", L"galaxyclienthelper.exe", L"gog galaxy.exe",
    L"battle.net.exe", L"battle.net helper.exe", L"agent.exe",
    L"riotclientservices.exe", L"riotclientux.exe", L"riotclient.exe",
    L"ubisoftconnect.exe", L"upc.exe", L"ubisoftgamelauncher.exe",
    L"eadesktop.exe", L"eabackgroundservice.exe", L"origin.exe", L"originwebhelperservice.exe",
    L"rockstargameslauncher.exe", L"socialclubhelper.exe",
    L"itch.exe", L"playnite.desktopapp.exe",
    // OBS itself and the desktop shell / common foreground non-games.
    L"obs64.exe", L"obs32.exe", L"obs.exe",
    L"explorer.exe", L"searchhost.exe", L"startmenuexperiencehost.exe",
    L"shellexperiencehost.exe", L"applicationframehost.exe",
    L"textinputhost.exe", L"systemsettings.exe",
};

// A modest list of well-known game / emulator executables that may not always
// be registered with Game Bar. Kept conservative to avoid false positives;
// the Game Bar and Game Mode signals do most of the work. Lower-case.
static const wchar_t *kKnownGameExes[] = {
    L"retroarch.exe",
    L"dolphin.exe",
    L"pcsx2-qt.exe", L"pcsx2.exe",
    L"rpcs3.exe",
    L"duckstation-qt-x64-releaseltcg.exe", L"duckstation-nogui-x64-releaseltcg.exe",
    L"cemu.exe",
    L"ppsspp.exe", L"ppssppwindows.exe", L"ppssppwindows64.exe",
    L"ryujinx.exe", L"ryujinx.ava.exe",
    L"xenia.exe", L"xenia_canary.exe",
    L"flycast.exe",
    L"mgba.exe",
    L"snes9x.exe", L"snes9x-x64.exe",
    L"project64.exe",
    L"vita3k.exe",
    L"shadps4.exe",
};

static bool in_table(const std::wstring &name_lower, const wchar_t *const *table, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (name_lower == table[i]) return true;
    }
    return false;
}

bool is_launcher_exe(const std::wstring &exe_name_lower) {
    return in_table(exe_name_lower, kLauncherExes, _countof(kLauncherExes));
}

bool is_known_game_exe(const std::wstring &exe_name_lower) {
    return in_table(exe_name_lower, kKnownGameExes, _countof(kKnownGameExes));
}

// --------------------------------------------------------------------------
// Foreground target
// --------------------------------------------------------------------------

static std::wstring exe_path_for_pid(DWORD pid) {
    std::wstring out;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = (DWORD)_countof(buf);
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        out.assign(buf, len);
    }
    CloseHandle(proc);
    return out;
}

static bool window_is_fullscreen(HWND hwnd, const RECT &wr) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT &m = mi.rcMonitor;
    // Window covers (>=) the whole monitor: borderless / fullscreen.
    return wr.left <= m.left && wr.top <= m.top &&
           wr.right >= m.right && wr.bottom >= m.bottom;
}

fg_target get_foreground_target() {
    fg_target t;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return t;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return t;  // skip our own process

    std::wstring path = exe_path_for_pid(pid);
    if (path.empty()) return t;

    t.hwnd = hwnd;
    t.pid = pid;
    t.exe_path = path;
    t.exe_name = to_lower(basename_of(path));
    GetWindowRect(hwnd, &t.rect);
    t.fullscreen = window_is_fullscreen(hwnd, t.rect);
    t.valid = true;
    return t;
}

// --------------------------------------------------------------------------
// Game Bar detector — match the foreground exe against GameConfigStore.
// --------------------------------------------------------------------------

static std::wstring read_reg_string(HKEY key, const wchar_t *value) {
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
        return std::wstring();
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || cb == 0)
        return std::wstring();
    std::wstring buf(cb / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, value, nullptr, &type,
                         reinterpret_cast<BYTE *>(&buf[0]), &cb) != ERROR_SUCCESS)
        return std::wstring();
    buf.resize(wcsnlen(buf.c_str(), buf.size()));  // trim trailing NULs
    return buf;
}

bool is_gamebar_recognized(const std::wstring &exe_path) {
    if (exe_path.empty()) return false;

    HKEY children;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"System\\GameConfigStore\\Children",
                      0, KEY_READ, &children) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring exe_lower = to_lower(exe_path);
    const std::wstring dir_lower = to_lower(dirname_of(exe_path));
    bool recognized = false;

    wchar_t sub[256];
    DWORD idx = 0;
    for (;;) {
        DWORD sub_len = _countof(sub);
        LONG e = RegEnumKeyExW(children, idx++, sub, &sub_len,
                               nullptr, nullptr, nullptr, nullptr);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS) continue;

        HKEY child;
        if (RegOpenKeyExW(children, sub, 0, KEY_READ, &child) != ERROR_SUCCESS)
            continue;

        // Strongest: exact full-path match (Win32 games matched by Game Bar).
        std::wstring matched = read_reg_string(child, L"MatchedExeFullPath");
        if (!matched.empty() && to_lower(matched) == exe_lower) {
            recognized = true;
        }
        // Weaker: the exe lives in a directory Game Bar associated with a game.
        if (!recognized && !dir_lower.empty()) {
            std::wstring parent = read_reg_string(child, L"ExeParentDirectory");
            if (!parent.empty()) {
                std::wstring parent_lower = to_lower(parent);
                // dir == parent, or dir is parent\sub...
                if (dir_lower == parent_lower ||
                    (dir_lower.size() > parent_lower.size() &&
                     dir_lower.compare(0, parent_lower.size(), parent_lower) == 0 &&
                     dir_lower[parent_lower.size()] == L'\\')) {
                    recognized = true;
                }
            }
        }

        RegCloseKey(child);
        if (recognized) break;
    }

    RegCloseKey(children);
    return recognized;
}

// --------------------------------------------------------------------------
// Game Mode detector — fullscreen Direct3D app running right now.
// --------------------------------------------------------------------------

bool is_game_mode_fullscreen() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    HRESULT hr = SHQueryUserNotificationState(&state);
    return SUCCEEDED(hr) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

// --------------------------------------------------------------------------
// Decision cascade
// --------------------------------------------------------------------------

static bool in_whitelist(const fg_target &t, const game_detect_settings &s) {
    const std::wstring path_lower = to_lower(t.exe_path);
    for (const std::wstring &w : s.whitelist) {
        if (w.empty()) continue;
        if (w == t.exe_name || w == path_lower) return true;
    }
    return false;
}

game_decision is_likely_game(const fg_target &t, const game_detect_settings &s) {
    if (!t.valid) return {false, "no foreground"};

    // 1. Manual whitelist always wins.
    if (in_whitelist(t, s)) return {true, "whitelist"};

    // 2. Never capture a launcher / shell UI.
    if (is_launcher_exe(t.exe_name)) return {false, "launcher blocklist"};

    // 3. Game Bar recognizes this executable as a game.
    if (s.use_game_bar && is_gamebar_recognized(t.exe_path))
        return {true, "game bar"};

    // 4. Game Mode: a fullscreen Direct3D game is running (== the foreground).
    if (s.use_game_mode && is_game_mode_fullscreen())
        return {true, "game mode (fullscreen d3d)"};

    // 5. Known game / emulator executable.
    if (is_known_game_exe(t.exe_name))
        return {true, "known game exe"};

    // 6. Strict mode: nothing else qualifies.
    if (s.strict_game_mode_only) return {false, "strict, no game signal"};

    // 7. Loose mode: any non-launcher borderless-fullscreen window.
    if (s.allow_fullscreen_fallback && t.fullscreen)
        return {true, "fullscreen fallback"};

    return {false, "no rule matched"};
}

// --------------------------------------------------------------------------
// Running-app enumeration for the whitelist picker
// --------------------------------------------------------------------------

struct enum_ctx {
    std::vector<running_app> *apps;
    DWORD self_pid;
};

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<enum_ctx *>(lparam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // owned popups
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    int title_len = GetWindowTextLengthW(hwnd);
    if (title_len <= 0) return TRUE;
    std::wstring title(title_len + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], title_len + 1);
    title.resize(wcsnlen(title.c_str(), title.size()));

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == ctx->self_pid) return TRUE;

    std::wstring path = exe_path_for_pid(pid);
    if (path.empty()) return TRUE;
    std::wstring name = basename_of(path);
    std::wstring name_lower = to_lower(name);

    // Skip the desktop shell / our own UI; keep launchers (user may still want
    // to pick one explicitly) but skip pure shell noise.
    if (name_lower == L"explorer.exe" || name_lower == L"applicationframehost.exe")
        return TRUE;

    // Dedupe by exe base name.
    for (const auto &a : *ctx->apps) {
        if (to_lower(a.exe_name) == name_lower) return TRUE;
    }
    ctx->apps->push_back({name, title});
    return TRUE;
}

std::vector<running_app> enumerate_running_apps() {
    std::vector<running_app> apps;
    enum_ctx ctx{&apps, GetCurrentProcessId()};
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    std::sort(apps.begin(), apps.end(),
              [](const running_app &a, const running_app &b) {
                  return to_lower(a.exe_name) < to_lower(b.exe_name);
              });
    return apps;
}
