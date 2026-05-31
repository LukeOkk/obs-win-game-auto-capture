<#
.SYNOPSIS
  One-click installer for obs-win-game-auto-capture.

.DESCRIPTION
  Copies the plugin into the OBS Studio install directory (the only place OBS
  on Windows loads plugins from) and re-elevates with UAC if needed.

  Works both inside an extracted release folder (the .dll + locale\ sit next to
  this script) and inside the source repo (uses build\Release\ + data\locale\).

.PARAMETER ObsRoot
  OBS install folder. Default: "$env:ProgramFiles\obs-studio".

.EXAMPLE
  # Right-click -> Run with PowerShell, or:
  ./install.ps1
#>
[CmdletBinding()]
param(
    [string]$ObsRoot = (Join-Path $env:ProgramFiles 'obs-studio')
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$PluginName = 'obs-win-game-auto-capture'

# Locate the DLL: next to this script (release zip) or in build\Release (repo).
$dll = Join-Path $here "$PluginName.dll"
if (-not (Test-Path $dll)) {
    $cand = Get-ChildItem $here -Recurse -Filter "$PluginName.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $dll = $cand.FullName }
}
if (-not (Test-Path $dll)) {
    Write-Host "ERROR: $PluginName.dll not found. Build it first (./build.ps1) or run this inside the extracted release folder." -ForegroundColor Red
    exit 1
}

# Locate the locale folder: 'locale' (release) or 'data\locale' (repo).
$localeSrc = Join-Path $here 'locale'
if (-not (Test-Path $localeSrc)) { $localeSrc = Join-Path $here 'data\locale' }

# Re-launch elevated if we can't write into the OBS folder.
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator rights (UAC) to write into the OBS folder..." -ForegroundColor Yellow
    $host_exe = (Get-Process -Id $PID).Path
    Start-Process -FilePath $host_exe -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-ObsRoot', "`"$ObsRoot`""
    )
    return
}

$destBin  = Join-Path $ObsRoot 'obs-plugins\64bit'
$destData = Join-Path $ObsRoot "data\obs-plugins\$PluginName\locale"
if (-not (Test-Path $destBin)) {
    Write-Host "ERROR: OBS not found at '$ObsRoot'. Re-run with -ObsRoot 'C:\path\to\obs-studio'." -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $destData | Out-Null
Copy-Item $dll $destBin -Force
if (Test-Path $localeSrc) { Copy-Item (Join-Path $localeSrc '*') $destData -Recurse -Force }

Write-Host ""
Write-Host "Installed obs-win-game-auto-capture into $ObsRoot." -ForegroundColor Green
Write-Host "Restart OBS, then add a source -> 'Windows Game Auto Capture'." -ForegroundColor Green
