// game-capture.h — captures one window's video (Windows.Graphics.Capture) and
// the target process's audio (WASAPI process loopback) and pushes both into an
// OBS async source.
//
// The implementation lives entirely behind a PImpl so that the heavy
// C++/WinRT, Direct3D and WASAPI headers stay out of every other translation
// unit. Construct on any thread, but drive bind()/teardown() from a single
// thread that has a multithreaded COM apartment initialized (the orchestrator
// worker thread does this).
#pragma once

#include <obs.h>
#include <windows.h>
#include <memory>
#include <string>

struct capture_settings {
    bool capture_cursor = false;  // WGC IsCursorCaptureEnabled
    bool capture_audio = true;    // pull the game's audio via process loopback
};

class GameCapture {
public:
    explicit GameCapture(obs_source_t *source);
    ~GameCapture();

    GameCapture(const GameCapture &) = delete;
    GameCapture &operator=(const GameCapture &) = delete;

    // Apply the latest settings (takes effect on the next bind()).
    void update(const capture_settings &cs);

    // Capture the given window + audio of the given process. Tears down any
    // previous capture first. Returns true if video capture started; audio is
    // best-effort and never fails the bind.
    bool bind(HWND hwnd, DWORD pid, const std::wstring &exe_name);

    // Stop video + audio and blank the source.
    void teardown();

    bool is_bound() const;
    HWND current_hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
