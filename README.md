# obs-win-game-auto-capture

An OBS Studio plugin for **Windows** that automatically captures **only the game
you're actually playing** — never the desktop, browser, Discord, or whatever
else is open — and tears the capture down when no game is focused.

It detects games using Windows' own signals: the **Xbox Game Bar** game registry
and the **Game Mode** (fullscreen Direct3D) state. Video is captured with
[Windows.Graphics.Capture](https://learn.microsoft.com/windows/uwp/audio-video-camera/screen-capture)
and the game's audio with **WASAPI per-process loopback**, so background
chat/browser audio never enters the stream.

## Install (easiest)

1. Download `obs-win-game-auto-capture-<version>-x64.zip` from the
   [**Releases**](../../releases) page and unzip it.
2. Right-click **`install.ps1`** → **Run with PowerShell** and approve the UAC
   prompt (it copies the plugin into the OBS folder, which needs admin).
3. Restart OBS, then add a source → **Windows Game Auto Capture**.

<details>
<summary>Manual install</summary>

OBS on Windows loads plugins only from its install directory:

- Copy `obs-win-game-auto-capture.dll` into
  `C:\Program Files\obs-studio\obs-plugins\64bit\`
- Copy the `locale` folder into
  `C:\Program Files\obs-studio\data\obs-plugins\obs-win-game-auto-capture\`

(OBS for Windows does **not** scan `%APPDATA%\obs-studio\plugins\`.)
</details>

## How it detects games

A layered cascade decides whether the foreground app is a game (first match wins):

1. **Manual whitelist** — executables you added explicitly.
2. **Launcher blocklist** — never captures Steam/Epic/Battle.net/etc. UIs or the
   shell, so you stream the game, not the store.
3. **Xbox Game Bar** — games the Game Bar has registered are persisted under
   `HKCU\System\GameConfigStore\Children`; the foreground executable is matched
   against those entries (`MatchedExeFullPath` / `ExeParentDirectory`).
4. **Game Mode** — `SHQueryUserNotificationState()` reports
   `QUNS_RUNNING_D3D_FULL_SCREEN` when a full-screen Direct3D game is running,
   the closest public proxy for "Game Mode would activate".
5. **Known game/emulator executables** — a small built-in list.
6. **Fullscreen fallback** *(optional, when strict mode is off)* — any
   non-launcher borderless-fullscreen window.

### Privacy

The plugin reads the Game Bar registry **only at runtime** to compare the
foreground executable path; it never copies, stores, or transmits that data, and
logs only the executable's base name (never full paths or game titles). There is
no telemetry.

## Settings

In the source's properties:

- **Use Xbox Game Bar recognition** (default on)
- **Use Game Mode signal** (default on)
- **Strict mode** (default on) — only capture recognized games.
- **Fullscreen fallback** (default off) — when strict is off, also capture any
  fullscreen non-launcher window.
- **Keep capturing on alt-tab** (default on)
- **Capture game audio** (default on)
- **Include cursor** (default off)
- **Manual whitelist** — pick a running app from the dropdown and click *Add*.

## Requirements

- Windows 10 1903+ (build 18362) for capture. Game-audio loopback needs
  Windows 10 2004+ (build 19041); on older builds the plugin runs video-only.
- Windows 11 additionally hides the yellow capture border.
- OBS Studio 30+.

## Build from source

```powershell
./build.ps1
```

`build.ps1` checks your toolchain (and prints exact install commands if anything
is missing — Visual Studio 2022 Build Tools with the **Desktop development with
C++** workload + a **Windows 11 SDK**, and **CMake 3.21+**), clones the OBS
source for the libobs headers, generates an `obs.lib` import library from your
installed `obs.dll`, builds with CMake, and installs the plugin into the OBS
install directory. Re-runs are incremental. Pass `-ObsRoot 'C:\path\to\obs-studio'`
if OBS isn't in `C:\Program Files\obs-studio`, or `-Clean` to rebuild from scratch.

To produce a distributable zip (`dist\obs-win-game-auto-capture-<ver>-x64.zip`):

```powershell
./package.ps1
```

## Known limitations

- Games using true exclusive-fullscreen Direct3D may return black frames via
  Windows.Graphics.Capture; **borderless-windowed** mode works best.
- A game must have been seen by Game Bar at least once (or be fullscreen-D3D, in
  the known list, or whitelisted) to be detected. If a game isn't detected, open
  it once with Game Bar enabled, or add it to the whitelist.

## License

MIT — see [LICENSE](LICENSE).
