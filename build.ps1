<#
.SYNOPSIS
  Build and install obs-win-game-auto-capture into the local OBS instance.

.DESCRIPTION
  What it does (re-run safe — caches expensive steps under windows\.deps):
    1. Detects the build toolchain (CMake, Visual Studio C++ tools, Windows SDK)
       and prints install guidance if anything is missing.
    2. Clones the OBS Studio source matching your installed OBS version into
       windows\.deps\obs-studio (only for the libobs headers — no full build).
    3. Generates an import library (obs.lib) from your installed obs.dll using
       dumpbin + lib, so we can link libobs without building it.
    4. Runs CMake configure + build (Release, x64).
    5. Installs the resulting .dll + data\ locale into the per-user OBS plugin
       folder: %APPDATA%\obs-studio\plugins\obs-win-game-auto-capture\

  No personal data is read or embedded: OBS and Game Bar are discovered through
  environment variables and the registry/Appx at runtime, never hard-coded.

.PARAMETER ObsRoot
  OBS Studio install folder. Defaults to "$env:ProgramFiles\obs-studio".

.PARAMETER Clean
  Remove cached deps + build output and rebuild from scratch.

.EXAMPLE
  ./windows/build.ps1
#>
[CmdletBinding()]
param(
    [string]$ObsRoot = (Join-Path $env:ProgramFiles 'obs-studio'),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$PluginName = 'obs-win-game-auto-capture'
$DepsDir    = Join-Path $PSScriptRoot '.deps'
$ObsSrcDir  = Join-Path $DepsDir 'obs-studio'
$ObsLib     = Join-Path $DepsDir 'obs.lib'
$ObsDef     = Join-Path $DepsDir 'obs.def'
$BuildDir   = Join-Path $PSScriptRoot 'build'

function Fail($msg) { Write-Host "`nERROR: $msg" -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

if ($Clean) {
    Step 'Cleaning cached deps and build output'
    Remove-Item -Recurse -Force $BuildDir, $ObsSrcDir, $ObsLib, $ObsDef -ErrorAction SilentlyContinue
}

# --------------------------------------------------------------------------
# 1. Toolchain detection
# --------------------------------------------------------------------------
Step 'Checking build tools'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    # A freshly winget-installed CMake may not be on this session's PATH yet.
    foreach ($cmBin in @((Join-Path $env:ProgramFiles 'CMake\bin'),
                         (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin'))) {
        if (Test-Path (Join-Path $cmBin 'cmake.exe')) { $env:PATH = "$cmBin;$env:PATH"; break }
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail @"
CMake not found. Install it, then re-run:
    winget install Kitware.CMake
(or https://cmake.org/download/). Reopen the terminal afterwards.
"@
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Fail @"
Visual Studio (C++ build tools) not found.
Install "Visual Studio 2022 Build Tools" with the
"Desktop development with C++" workload AND a Windows 11 SDK:
    winget install Microsoft.VisualStudio.2022.BuildTools `
        --override "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
Then reopen the terminal and re-run this script.
"@
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null
if (-not $vsPath) {
    Fail @"
A Visual Studio install was found but without the C++ tools workload.
Add it via the Visual Studio Installer:
    "Desktop development with C++" + a Windows 11 SDK.
"@
}

# Pick the matching CMake generator from the VS major version so we don't
# depend on CMake's default-generator guess.
$vsVer = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationVersion 2>$null
$vsMajor = if ($vsVer) { ($vsVer -split '\.')[0] } else { '17' }
switch ($vsMajor) {
    '17' { $Generator = 'Visual Studio 17 2022' }
    '16' { $Generator = 'Visual Studio 16 2019' }
    default { $Generator = 'Visual Studio 17 2022' }
}

Step "Activating Visual Studio dev environment ($vsPath)"
# Put vswhere.exe on PATH so Launch-VsDevShell's internal lookups resolve quietly.
$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
if (Test-Path $installerDir) { $env:PATH = "$installerDir;$env:PATH" }
$devShell = Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1'
if (-not (Test-Path $devShell)) { Fail "Launch-VsDevShell.ps1 not found under $vsPath." }
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Set-Location $PSScriptRoot  # DevShell may change the directory

foreach ($tool in 'cl', 'lib', 'dumpbin') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "'$tool' is not available even after activating the VS dev shell. Repair the C++ workload."
    }
}

# Windows SDK with C++/WinRT headers (winrt\*.h). Warn (not fatal) if missing.
$kitInc = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$haveCppWinRT = $false
if (Test-Path $kitInc) {
    if (Get-ChildItem $kitInc -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Test-Path (Join-Path $_.FullName 'cppwinrt\winrt\base.h') } |
        Where-Object { $_ }) { $haveCppWinRT = $true }
}
if (-not $haveCppWinRT) {
    Write-Host "WARNING: Could not find C++/WinRT headers (cppwinrt\winrt\base.h) in a Windows SDK." -ForegroundColor Yellow
    Write-Host "         If the build fails on <winrt/...> includes, install/repair the Windows 11 SDK." -ForegroundColor Yellow
}

# --------------------------------------------------------------------------
# 2. OBS detection + headers
# --------------------------------------------------------------------------
$ObsExe = Join-Path $ObsRoot 'bin\64bit\obs64.exe'
$ObsDll = Join-Path $ObsRoot 'bin\64bit\obs.dll'
if (-not (Test-Path $ObsDll)) {
    Fail "obs.dll not found at $ObsDll. Pass -ObsRoot 'C:\path\to\obs-studio' if OBS is elsewhere."
}

$ObsVersion = $env:OBS_VERSION
if (-not $ObsVersion -and (Test-Path $ObsExe)) {
    $pv = (Get-Item $ObsExe).VersionInfo.ProductVersion
    if ($pv) {
        $m = [regex]::Match($pv, '^\d+\.\d+\.\d+')
        if ($m.Success) { $ObsVersion = $m.Value }
    }
}
if (-not $ObsVersion) { $ObsVersion = '32.1.2' }

New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null

Step "Cloning OBS Studio source for headers (v$ObsVersion)"
if (-not (Test-Path (Join-Path $ObsSrcDir 'libobs\obs-module.h'))) {
    Remove-Item -Recurse -Force $ObsSrcDir -ErrorAction SilentlyContinue
    git clone --depth 1 --branch $ObsVersion `
        https://github.com/obsproject/obs-studio.git $ObsSrcDir 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  tag $ObsVersion not found; falling back to master" -ForegroundColor Yellow
        git clone --depth 1 https://github.com/obsproject/obs-studio.git $ObsSrcDir
        if ($LASTEXITCODE -ne 0) { Fail 'git clone of obs-studio failed.' }
    }
} else {
    Write-Host "  cached at $ObsSrcDir"
}
$LibObsInclude = Join-Path $ObsSrcDir 'libobs'
if (-not (Test-Path (Join-Path $LibObsInclude 'obs-module.h'))) {
    Fail "libobs headers missing at $LibObsInclude"
}

# --------------------------------------------------------------------------
# 3. Generate obs.lib import library from the installed obs.dll
# --------------------------------------------------------------------------
if (-not (Test-Path $ObsLib)) {
    Step 'Generating obs.lib import library from obs.dll'
    $exports = & dumpbin /exports $ObsDll
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in $exports) {
        # Match: "  <ordinal> <hint> <RVA> <name>"  (skip by-ordinal/[NONAME])
        $m = [regex]::Match($line, '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)')
        if ($m.Success) {
            $n = $m.Groups[1].Value
            if ($n -ne '[NONAME]' -and $n -match '^[A-Za-z_]') { $names.Add($n) }
        }
    }
    if ($names.Count -eq 0) { Fail 'No exports parsed from obs.dll — cannot build import lib.' }

    # LIBRARY fixes the import module name to obs.dll (the module OBS loads),
    # so the plugin's imports resolve against the already-loaded libobs.
    $defLines = @('LIBRARY obs.dll', 'EXPORTS')
    $defLines += ($names | Sort-Object -Unique)
    Set-Content -Path $ObsDef -Value $defLines -Encoding ASCII

    & lib "/def:$ObsDef" /machine:x64 "/out:$ObsLib" | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ObsLib)) { Fail 'lib.exe failed to create obs.lib.' }
    Write-Host "  wrote $ObsLib ($($names.Count) exports)"
} else {
    Write-Host "==> obs.lib cached at $ObsLib" -ForegroundColor Cyan
}

# --------------------------------------------------------------------------
# 4. Configure + build
# --------------------------------------------------------------------------
$incFwd = $LibObsInclude.Replace('\', '/')
$libFwd = $ObsLib.Replace('\', '/')

Step "Configuring (CMake, $Generator)"
cmake -S "$PSScriptRoot" -B "$BuildDir" -G "$Generator" -A x64 `
    "-DLIBOBS_INCLUDE_DIR=$incFwd" `
    "-DOBS_LIB=$libFwd"
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed.' }

Step 'Building (Release)'
cmake --build "$BuildDir" --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail 'Build failed.' }

$dll = Get-ChildItem $BuildDir -Recurse -Filter "$PluginName.dll" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $dll) { Fail "Build did not produce $PluginName.dll." }

# --------------------------------------------------------------------------
# 5. Install into the OBS install dir.
#
# OBS Studio on Windows loads plugins from <ObsRoot>\obs-plugins\64bit (NOT from
# %APPDATA%\obs-studio\plugins, which it does not scan on Windows). That folder
# lives under Program Files, so writing it needs administrator rights.
# --------------------------------------------------------------------------
$destBin  = Join-Path $ObsRoot 'obs-plugins\64bit'
$destData = Join-Path $ObsRoot "data\obs-plugins\$PluginName\locale"

Step "Installing into OBS ($ObsRoot\obs-plugins\64bit)"
$ok = $true
try {
    New-Item -ItemType Directory -Force -Path $destData -ErrorAction Stop | Out-Null
    Copy-Item $dll.FullName $destBin -Force -ErrorAction Stop
    Copy-Item (Join-Path $PSScriptRoot 'data\locale\*') $destData -Recurse -Force -ErrorAction Stop
} catch { $ok = $false }

if (-not $ok) {
    Write-Host ""
    Write-Host "Could not write to '$ObsRoot' — it needs administrator rights." -ForegroundColor Yellow
    Write-Host "Open an elevated PowerShell (Run as administrator) and run:" -ForegroundColor Yellow
    Write-Host "  Copy-Item '$($dll.FullName)' '$destBin' -Force"
    Write-Host "  New-Item -ItemType Directory -Force '$destData' | Out-Null"
    Write-Host "  Copy-Item '$(Join-Path $PSScriptRoot 'data\locale\*')' '$destData' -Recurse -Force"
    exit 1
}

Write-Host ""
Write-Host "Installed. Quit and reopen OBS, then add a source -> 'Windows Game Auto Capture'." -ForegroundColor Green
