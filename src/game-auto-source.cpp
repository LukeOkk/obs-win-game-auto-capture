// game-auto-source.cpp — the OBS source: settings, properties, and the worker
// thread that ties detection (game-detect) to capture (game-capture).
//
// A dedicated worker thread polls the foreground app roughly once a second
// (mirroring the macOS plugin's 3s poll + activation observer), decides whether
// it is a game, and binds/tears down the capture accordingly. The thread runs
// in a multithreaded COM apartment because Windows.Graphics.Capture activation
// happens there.

#include "game-auto-source.h"
#include "game-detect.h"
#include "game-capture.h"
#include "plugin-log.h"

#include <obs-module.h>

#include <winrt/Windows.Foundation.h>  // init_apartment

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <string.h>

// Settings keys
#define S_USE_GAME_BAR        "use_game_bar"
#define S_USE_GAME_MODE       "use_game_mode"
#define S_STRICT              "strict_game_mode_only"
#define S_FULLSCREEN_FALLBACK "allow_fullscreen_fallback"
#define S_KEEP_CAPTURING      "keep_capturing_on_alt_tab"
#define S_CAPTURE_AUDIO       "capture_audio"
#define S_CAPTURE_CURSOR      "capture_cursor"
#define S_WHITELIST           "whitelist"
#define S_RUNNING_APP_PICKER  "running_app_picker"
#define S_ADD_BUTTON          "add_running_app_btn"

// --------------------------------------------------------------------------
// Narrow/wide helpers (local; game-detect owns the wide-only helpers)
// --------------------------------------------------------------------------

static std::string narrow(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_w(const char *s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// --------------------------------------------------------------------------
// Source instance data
// --------------------------------------------------------------------------

struct win_game_auto_data {
    obs_source_t *source = nullptr;
    GameCapture *capture = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    HANDLE wake = nullptr;  // auto-reset: signaled to stop or to re-evaluate now

    std::mutex settings_mutex;
    game_detect_settings ds;
    capture_settings cs;
};

// --------------------------------------------------------------------------
// Worker: detection -> capture orchestration
// --------------------------------------------------------------------------

static void reevaluate(win_game_auto_data *d) {
    game_detect_settings ds;
    capture_settings cs;
    {
        std::lock_guard<std::mutex> lk(d->settings_mutex);
        ds = d->ds;
        cs = d->cs;
    }
    d->capture->update(cs);

    fg_target t = get_foreground_target();
    game_decision dec = is_likely_game(t, ds);

    if (dec.accept) {
        if (d->capture->current_hwnd() != t.hwnd) {
            PLUGIN_LOG(LOG_INFO, "accept: exe=%s (%s)", narrow(t.exe_name).c_str(), dec.reason);
            d->capture->bind(t.hwnd, t.pid, t.exe_name);
        }
        return;
    }

    // Frontmost is not a game.
    if (ds.keep_capturing_on_alt_tab && d->capture->is_bound() &&
        IsWindow(d->capture->current_hwnd())) {
        return;  // keep streaming the previously-bound game on alt-tab
    }
    if (d->capture->is_bound()) {
        PLUGIN_LOG(LOG_INFO, "teardown: front not a game (%s)", dec.reason);
        d->capture->teardown();
    }
}

static void worker_main(win_game_auto_data *d) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bool mixers_checked = false;

    while (d->running.load()) {
        reevaluate(d);
        WaitForSingleObject(d->wake, 1000);  // poll ~1s; woken early on update/stop

        // Once, ~1s in: if a pre-existing scene saved this source with zero
        // audio-mixer tracks, route it to all 6 so captured audio reaches the
        // output (same fix the macOS plugin applies on a delay).
        if (!mixers_checked) {
            mixers_checked = true;
            if (obs_source_get_audio_mixers(d->source) == 0) {
                obs_source_set_audio_mixers(d->source, 0x3F);
                PLUGIN_LOG(LOG_INFO, "audio mixers were 0 — set to all 6 tracks (0x3F)");
            }
        }
    }

    if (d->capture) d->capture->teardown();
    winrt::uninit_apartment();
}

// --------------------------------------------------------------------------
// OBS source vtable
// --------------------------------------------------------------------------

static const char *gas_get_name(void *) { return obs_module_text("SourceName"); }

static void gas_update(void *data, obs_data_t *settings) {
    auto *d = static_cast<win_game_auto_data *>(data);

    game_detect_settings ds;
    ds.use_game_bar = obs_data_get_bool(settings, S_USE_GAME_BAR);
    ds.use_game_mode = obs_data_get_bool(settings, S_USE_GAME_MODE);
    ds.strict_game_mode_only = obs_data_get_bool(settings, S_STRICT);
    ds.allow_fullscreen_fallback = obs_data_get_bool(settings, S_FULLSCREEN_FALLBACK);
    ds.keep_capturing_on_alt_tab = obs_data_get_bool(settings, S_KEEP_CAPTURING);

    obs_data_array_t *arr = obs_data_get_array(settings, S_WHITELIST);
    if (arr) {
        size_t n = obs_data_array_count(arr);
        for (size_t i = 0; i < n; i++) {
            obs_data_t *it = obs_data_array_item(arr, i);
            const char *v = obs_data_get_string(it, "value");
            if (v && *v) ds.whitelist.push_back(to_lower(utf8_to_w(v)));
            obs_data_release(it);
        }
        obs_data_array_release(arr);
    }

    capture_settings cs;
    cs.capture_audio = obs_data_get_bool(settings, S_CAPTURE_AUDIO);
    cs.capture_cursor = obs_data_get_bool(settings, S_CAPTURE_CURSOR);

    {
        std::lock_guard<std::mutex> lk(d->settings_mutex);
        d->ds = std::move(ds);
        d->cs = cs;
    }
    if (d->wake) SetEvent(d->wake);  // re-evaluate promptly
}

static void gas_defaults(obs_data_t *s) {
    obs_data_set_default_bool(s, S_USE_GAME_BAR, true);
    obs_data_set_default_bool(s, S_USE_GAME_MODE, true);
    obs_data_set_default_bool(s, S_STRICT, true);
    obs_data_set_default_bool(s, S_FULLSCREEN_FALLBACK, false);
    obs_data_set_default_bool(s, S_KEEP_CAPTURING, true);
    obs_data_set_default_bool(s, S_CAPTURE_AUDIO, true);
    obs_data_set_default_bool(s, S_CAPTURE_CURSOR, false);
}

static void *gas_create(obs_data_t *settings, obs_source_t *source) {
    auto *d = new win_game_auto_data();
    d->source = source;
    d->capture = new GameCapture(source);
    d->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    gas_update(d, settings);

    d->running = true;
    d->worker = std::thread(worker_main, d);

    PLUGIN_LOG(LOG_INFO, "source created");
    return d;
}

static void gas_destroy(void *data) {
    auto *d = static_cast<win_game_auto_data *>(data);
    d->running = false;
    if (d->wake) SetEvent(d->wake);
    if (d->worker.joinable()) d->worker.join();

    delete d->capture;
    if (d->wake) CloseHandle(d->wake);
    delete d;
}

// Button: append the picker's selected exe to the whitelist array.
static bool gas_add_running_app(obs_properties_t *props, obs_property_t *prop, void *data) {
    (void)props;
    (void)prop;
    auto *d = static_cast<win_game_auto_data *>(data);
    if (!d || !d->source) return false;

    obs_data_t *settings = obs_source_get_settings(d->source);
    const char *picked = obs_data_get_string(settings, S_RUNNING_APP_PICKER);
    bool changed = false;
    if (picked && *picked) {
        obs_data_array_t *arr = obs_data_get_array(settings, S_WHITELIST);
        if (!arr) arr = obs_data_array_create();
        bool exists = false;
        size_t n = obs_data_array_count(arr);
        for (size_t i = 0; i < n; i++) {
            obs_data_t *it = obs_data_array_item(arr, i);
            const char *v = obs_data_get_string(it, "value");
            if (v && _stricmp(v, picked) == 0) exists = true;
            obs_data_release(it);
            if (exists) break;
        }
        if (!exists) {
            obs_data_t *it = obs_data_create();
            obs_data_set_string(it, "value", picked);
            obs_data_array_push_back(arr, it);
            obs_data_release(it);
            obs_data_set_array(settings, S_WHITELIST, arr);
            obs_data_set_string(settings, S_RUNNING_APP_PICKER, "");
            obs_source_update(d->source, settings);
            changed = true;
        }
        obs_data_array_release(arr);
    }
    obs_data_release(settings);
    return changed;  // true → OBS refreshes the properties pane
}

static obs_properties_t *gas_properties(void *data) {
    (void)data;
    obs_properties_t *p = obs_properties_create();

    obs_properties_add_bool(p, S_USE_GAME_BAR, obs_module_text("UseGameBar"));
    obs_properties_add_bool(p, S_USE_GAME_MODE, obs_module_text("UseGameMode"));
    obs_properties_add_bool(p, S_STRICT, obs_module_text("StrictGameModeOnly"));
    obs_properties_add_bool(p, S_FULLSCREEN_FALLBACK, obs_module_text("AllowFullscreenFallback"));
    obs_properties_add_bool(p, S_KEEP_CAPTURING, obs_module_text("KeepCapturingOnAltTab"));
    obs_properties_add_bool(p, S_CAPTURE_AUDIO, obs_module_text("CaptureAudio"));
    obs_properties_add_bool(p, S_CAPTURE_CURSOR, obs_module_text("CaptureCursor"));

    obs_property_t *picker = obs_properties_add_list(p, S_RUNNING_APP_PICKER,
                                                     obs_module_text("PickRunningApp"),
                                                     OBS_COMBO_TYPE_LIST,
                                                     OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(picker, "—", "");
    for (const auto &a : enumerate_running_apps()) {
        std::string disp = narrow(a.title) + "  ·  " + narrow(a.exe_name);
        std::string val = narrow(a.exe_name);  // gas_update lower-cases on read
        obs_property_list_add_string(picker, disp.c_str(), val.c_str());
    }

    obs_properties_add_button(p, S_ADD_BUTTON, obs_module_text("AddToWhitelist"),
                              gas_add_running_app);
    obs_properties_add_editable_list(p, S_WHITELIST, obs_module_text("Whitelist"),
                                     OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);
    return p;
}

static obs_source_info build_info() {
    obs_source_info si{};
    si.id = "win_game_auto_capture";
    si.type = OBS_SOURCE_TYPE_INPUT;
    si.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    si.get_name = gas_get_name;
    si.create = gas_create;
    si.destroy = gas_destroy;
    si.update = gas_update;
    si.get_defaults = gas_defaults;
    si.get_properties = gas_properties;
    si.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
    return si;
}

struct obs_source_info game_auto_source_info = build_info();
