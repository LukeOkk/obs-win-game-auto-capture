<#
.SYNOPSIS
  Package the built plugin into a distributable, install-friendly .zip.

.DESCRIPTION
  Stages

      obs-win-game-auto-capture\
        obs-win-game-auto-capture.dll
        locale\*.ini
        install.ps1
        INSTALL.txt

  and zips it to dist\obs-win-game-auto-capture-<Version>-x64.zip. End users
  unzip and run install.ps1 (it self-elevates and copies into the OBS folder).

.PARAMETER Version
  Version string for the artifact name. Default 0.1.0.
#>
[CmdletBinding()]
param([string]$Version = '0.1.0')

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$PluginName = 'obs-win-game-auto-capture'
$BuildDir   = Join-Path $PSScriptRoot 'build'
$DistDir    = Join-Path $PSScriptRoot 'dist'

$dll = Get-ChildItem $BuildDir -Recurse -Filter "$PluginName.dll" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $dll) {
    Write-Host '==> DLL not built — running build.ps1 first' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1')
    $dll = Get-ChildItem $BuildDir -Recurse -Filter "$PluginName.dll" -ErrorAction SilentlyContinue |
        Select-Object -First 1
}
if (-not $dll) { Write-Host "ERROR: build did not produce $PluginName.dll" -ForegroundColor Red; exit 1 }

$stage = Join-Path ([IO.Path]::GetTempPath()) ("pkg-" + [guid]::NewGuid().ToString('N'))
$root  = Join-Path $stage $PluginName
$loc   = Join-Path $root 'locale'
New-Item -ItemType Directory -Force -Path $loc | Out-Null

Copy-Item $dll.FullName $root -Force
Copy-Item (Join-Path $PSScriptRoot 'data\locale\*') $loc -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot 'install.ps1') $root -Force

@"
obs-win-game-auto-capture $Version

To install:
  1. Right-click install.ps1 -> "Run with PowerShell" (approve the UAC prompt),
     or run it from PowerShell:  ./install.ps1
  2. Restart OBS Studio.
  3. Add a source -> "Windows Game Auto Capture".

Manual install (if you prefer): copy obs-win-game-auto-capture.dll into
  <OBS>\obs-plugins\64bit\
and the locale folder into
  <OBS>\data\obs-plugins\obs-win-game-auto-capture\
"@ | Set-Content -Path (Join-Path $root 'INSTALL.txt') -Encoding UTF8

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
$zip = Join-Path $DistDir "$PluginName-$Version-x64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $root -DestinationPath $zip -Force
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Artifact:" -ForegroundColor Green
Get-Item $zip | Format-List FullName, Length
