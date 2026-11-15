# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Builds the console dependencies once, into build-deps/prefix.

.DESCRIPTION
  The expensive dependency is SDL3-uwp: a full SDL 3.4.20 with the WinRT backends
  (CoreWindow video, WASAPI audio, Windows.Gaming.Input gamepads, WinRT pickers).
  It is built with MSVC because its WinRT code is C++/CX, and it is built *once*:
  the result is cached by CI (actions/cache on build-deps/prefix) so a normal
  push does not spend minutes recompiling SDL.

  libuwp gives the two things SDL3 does not: the Xbox TV display size and
  refresh rate (HdmiDisplayInformation, not the CoreWindow bounds, which on
  Series S are not the output resolution) and LoadPackagedLibrary for packaged
  DLLs.

  Outputs (relative to the repository root):
    build-deps/prefix/sdl3/{include,lib,bin,PDB}
    build-deps/prefix/libuwp/{include,lib,bin}
    build-deps/prefix/STAMP.txt      commit ids + toolset, so the cache is keyed
#>
[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$Platform = "x64",
    [string]$Configuration = "Release",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$SDL3Repo = "https://github.com/XboxEmulationHub/SDL3-uwp.git"
$SDL3Branch = "uwp-3.4.20"
$LibUwpRepo = "https://github.com/XboxEmulationHub/libuwp.git"

$deps = Join-Path $Root "build-deps"
$src = Join-Path $deps "src"
$prefix = Join-Path $deps "prefix"
$stamp = Join-Path $prefix "STAMP.txt"
New-Item -ItemType Directory -Force -Path $src, $prefix | Out-Null

function Get-CommandOrFail([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "$Name not found on PATH (Visual Studio Build Tools + Git are required)" }
    return $cmd
}

function Sync-Repo([string]$Url, [string]$Branch, [string]$Path) {
    if (Test-Path (Join-Path $Path ".git")) {
        git -C $Path fetch --depth 1 origin $Branch | Out-Null
        git -C $Path checkout -q FETCH_HEAD
    } else {
        git clone --depth 1 --branch $Branch $Url $Path | Out-Null
    }
    return (git -C $Path rev-parse HEAD).Trim()
}

if (-not $Force -and (Test-Path $stamp)) {
    Write-Host "build-deps: reusing $stamp (pass -Force to rebuild)"
    Get-Content $stamp | ForEach-Object { Write-Host "  $_" }
    exit 0
}

Get-CommandOrFail git | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; install Visual Studio Build Tools" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
    Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found" }

$sdlPath = Join-Path $src "SDL3-uwp"
$sdlCommit = Sync-Repo $SDL3Repo $SDL3Branch $sdlPath

Write-Host "build-deps: SDL3-uwp $sdlCommit ($SDL3Branch)"
$sdlProject = Join-Path $sdlPath "VisualC-WinRT\SDL-UWP.vcxproj"
$sdlOut = Join-Path $deps "out\sdl3"
# Retarget: the fork pins PlatformToolset v145 and SDK 10.0; CI images ship v143.
& $msbuild $sdlProject /m /p:Configuration=$Configuration /p:Platform=$Platform `
    /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 `
    /p:OutDir="$sdlOut\" /p:IntDir="$sdlOut\obj\" /v:minimal
if ($LASTEXITCODE -ne 0) { throw "SDL3-uwp build failed" }

$sdlPrefix = Join-Path $prefix "sdl3"
New-Item -ItemType Directory -Force -Path (Join-Path $sdlPrefix "lib"), (Join-Path $sdlPrefix "bin") | Out-Null
# Headers are the fork's public tree, which is what an SDL app includes.
Copy-Item -Recurse -Force (Join-Path $sdlPath "include") $sdlPrefix
$sdlLib = Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.lib" | Select-Object -First 1
$sdlDll = Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.dll" | Select-Object -First 1
if (-not $sdlLib -or -not $sdlDll) { throw "SDL3.lib/SDL3.dll not produced under $sdlOut" }
Copy-Item $sdlLib.FullName (Join-Path $sdlPrefix "lib\SDL3.lib")
Copy-Item $sdlDll.FullName (Join-Path $sdlPrefix "bin\SDL3.dll")
Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.pdb" | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $sdlPrefix "bin\SDL3.pdb")
}

Write-Host "build-deps: libuwp"
$libPath = Join-Path $src "libuwp"
$libCommit = Sync-Repo $LibUwpRepo "main" $libPath
$libOut = Join-Path $deps "out\libuwp"
& cmake -S $libPath -B $libOut -A $Platform -DCMAKE_SYSTEM_NAME=WindowsStore `
    -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=$Configuration
if ($LASTEXITCODE -ne 0) { throw "libuwp configure failed" }
& cmake --build $libOut --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "libuwp build failed" }

$libPrefix = Join-Path $prefix "libuwp"
New-Item -ItemType Directory -Force -Path (Join-Path $libPrefix "lib"), (Join-Path $libPrefix "bin") | Out-Null
Copy-Item -Recurse -Force (Join-Path $libPath "src") (Join-Path $libPrefix "include")
$libDll = Get-ChildItem -Recurse -Path $libOut -Filter "libuwp.dll" | Select-Object -First 1
$libLib = Get-ChildItem -Recurse -Path $libOut -Filter "libuwp.lib" | Select-Object -First 1
if (-not $libDll -or -not $libLib) { throw "libuwp.dll/.lib not produced under $libOut" }
Copy-Item $libLib.FullName (Join-Path $libPrefix "lib\libuwp.lib")
Copy-Item $libDll.FullName (Join-Path $libPrefix "bin\libuwp.dll")

@(
    "sdl3=$sdlCommit ($SDL3Branch)"
    "libuwp=$libCommit"
    "toolset=v143, sdk=10.0, platform=$Platform, config=$Configuration"
) | Set-Content -Encoding utf8 $stamp
Write-Host "build-deps: done -> $prefix"
