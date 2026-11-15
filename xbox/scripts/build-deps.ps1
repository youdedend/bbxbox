# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Builds every console dependency once, into build-deps/prefix (CI caches it).

.DESCRIPTION
  Four dependencies, all pinned, all downloaded or built *here* so a normal CI
  push never pays for them again:

    sdl3      XboxEmulationHub/SDL3-uwp @ uwp-3.4.20, built with MSBuild. Its
              WinRT code is C++/CX, so MSVC builds it; the app is clang-cl and
              meets it at SDL's C ABI. The fork pins toolset v145, so the project
              is retargeted to the toolset the runner has (v143).
    libuwp    XboxEmulationHub/libuwp: the TV display mode (HdmiDisplayInformation)
              and LoadPackagedLibrary. C++/WinRT, so it builds with CMake.
    ffmpeg    Cosmo/ext-ffmpeg-core-xbox ships *prebuilt* static libraries as a
              release asset keyed by the 12-character short SHA of the commit
              (CMakeLists.txt in that repo downloads it too). Building FFmpeg on
              CI would cost tens of minutes for no benefit: download the asset
              for the pinned commit and unpack it.
    dxc       Microsoft DirectXShaderCompiler v1.9.2609 (dxc_2026_09_29.zip):
              dxcompiler.dll + dxil.dll + dxc.exe, the compiler the console
              package loads with LoadPackagedLibrary and signs DXIL with.

  Pins live at the top of this file; changing one changes the cache key
  (`.github/workflows/ci.yml` hashes this script), so a bump rebuilds once and
  every other push is a cache hit.
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

# --- pins -------------------------------------------------------------------
$SDL3Repo = "https://github.com/XboxEmulationHub/SDL3-uwp.git"
$SDL3Branch = "uwp-3.4.20"
$LibUwpRepo = "https://github.com/XboxEmulationHub/libuwp.git"
$LibUwpBranch = "main"
# ext-ffmpeg-core-xbox publishes per-commit prebuilts; this is master's HEAD that
# has a release. `git ls-remote` would work too, but a pin is a pin.
$FfmpegCommit = "9a1c1987542880ec237f9535dfb37ff0558c23e"
$FfmpegAsset = "ffmpeg-windows-x64.zip"
$DxcUrl = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip"
$DxcSha256 = "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1"

$deps = Join-Path $Root "build-deps"
$src = Join-Path $deps "src"
$prefix = Join-Path $deps "prefix"
$stamp = Join-Path $prefix "STAMP.txt"
New-Item -ItemType Directory -Force -Path $src, $prefix | Out-Null

function Get-CommandOrFail([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "$Name not found on PATH" }
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

function Enter-Vs {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found; Visual Studio Build Tools are required" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if (-not $vs) { throw "no Visual Studio with the C++ toolset found" }
    # The dev shell is what puts cl.exe, link.exe, the Windows SDK and the UWP
    # libs (lib/x64/store) on PATH; CMake with Ninja needs it, MSBuild needs its
    # own environment but tolerates this one.
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $clang = "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe"
    if (-not (Test-Path $clang)) {
        throw "clang-cl not found at $clang; the runner needs the 'Microsoft.VisualStudio.Component.VC.Llvm.Clang' component"
    }
    & $clang --version | Select-Object -First 1 | Write-Host
    return $vs
}

if (-not $Force -and (Test-Path $stamp)) {
    Write-Host "build-deps: reusing $stamp (pass -Force to rebuild)"
    Get-Content $stamp | ForEach-Object { Write-Host "  $_" }
    exit 0
}

Get-CommandOrFail git | Out-Null
$vs = Enter-Vs
$msbuild = Get-ChildItem -Path (Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe") -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $msbuild) {
    $msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
        Select-Object -First 1
}
if (-not $msbuild) { throw "MSBuild not found" }

# --- SDL3-uwp ---------------------------------------------------------------
$sdlPath = Join-Path $src "SDL3-uwp"
$sdlCommit = Sync-Repo $SDL3Repo $SDL3Branch $sdlPath
Write-Host "build-deps: SDL3-uwp $sdlCommit ($SDL3Branch)"
$sdlProject = Join-Path $sdlPath "VisualC-WinRT\SDL-UWP.vcxproj"
$sdlOut = Join-Path $deps "out\sdl3"
New-Item -ItemType Directory -Force -Path $sdlOut | Out-Null
& $msbuild $sdlProject /m /p:Configuration=$Configuration /p:Platform=$Platform `
    /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 `
    /p:OutDir="$sdlOut\" /p:IntDir="$sdlOut\obj\" /v:minimal
if ($LASTEXITCODE -ne 0) { throw "SDL3-uwp build failed" }

$sdlPrefix = Join-Path $prefix "sdl3"
New-Item -ItemType Directory -Force -Path (Join-Path $sdlPrefix "lib"), (Join-Path $sdlPrefix "bin") | Out-Null
Copy-Item -Recurse -Force (Join-Path $sdlPath "include") $sdlPrefix
$sdlLib = Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.lib" | Select-Object -First 1
$sdlDll = Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.dll" | Select-Object -First 1
if (-not $sdlLib -or -not $sdlDll) { throw "SDL3.lib/SDL3.dll not produced under $sdlOut" }
Copy-Item $sdlLib.FullName (Join-Path $sdlPrefix "lib\SDL3.lib")
Copy-Item $sdlDll.FullName (Join-Path $sdlPrefix "bin\SDL3.dll")
Get-ChildItem -Recurse -Path $sdlOut -Filter "SDL3.pdb" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $sdlPrefix "bin\SDL3.pdb")
}

# --- libuwp -----------------------------------------------------------------
Write-Host "build-deps: libuwp"
$libPath = Join-Path $src "libuwp"
$libCommit = Sync-Repo $LibUwpRepo $LibUwpBranch $libPath
$libOut = Join-Path $deps "out\libuwp"
& cmake -S $libPath -B $libOut -G Ninja `
    -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_BUILD_TYPE=$Configuration
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

# --- FFmpeg (prebuilt release asset) ---------------------------------------
Write-Host "build-deps: FFmpeg prebuilt $FfmpegCommit"
$ffPrefix = Join-Path $prefix "ffmpeg"
$ffStamp = Join-Path $ffPrefix ".commit"
if (-not (Test-Path (Join-Path $ffPrefix "include\libavcodec\avcodec.h"))) {
    $zip = Join-Path $deps "out\ffmpeg.zip"
    New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
    $url = "https://github.com/Cosmo/ext-ffmpeg-core-xbox/releases/download/$($FfmpegCommit.Substring(0,12))/$FfmpegAsset"
    Invoke-WebRequest -Uri $url -OutFile $zip
    Remove-Item -Recurse -Force $ffPrefix -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ffPrefix | Out-Null
    Expand-Archive -Path $zip -DestinationPath $ffPrefix -Force
    # The archive holds include/ and lib/ at its root (or one level down).
    if (-not (Test-Path (Join-Path $ffPrefix "include\libavcodec\avcodec.h"))) {
        $inner = Get-ChildItem -Path $ffPrefix -Directory | Where-Object {
            Test-Path (Join-Path $_.FullName "include\libavcodec\avcodec.h") } | Select-Object -First 1
        if (-not $inner) { throw "FFmpeg archive layout not understood" }
        Get-ChildItem -Path $inner.FullName | ForEach-Object { Move-Item $_.FullName $ffPrefix -Force }
        Remove-Item -Recurse -Force $inner.FullName
    }
    Set-Content -Path $ffStamp -Value $FfmpegCommit -Encoding utf8
    Write-Host "build-deps: FFmpeg unpacked"
} else {
    Write-Host "build-deps: FFmpeg already unpacked"
}
$ffCommit = $FfmpegCommit

# --- DXC --------------------------------------------------------------------
Write-Host "build-deps: DXC v1.9.2609"
$dxcPrefix = Join-Path $prefix "dxc"
if (-not (Test-Path (Join-Path $dxcPrefix "dxcompiler.dll"))) {
    $zip = Join-Path $deps "out\dxc.zip"
    New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
    Invoke-WebRequest -Uri $DxcUrl -OutFile $zip
    $hash = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($hash -ne $DxcSha256) { throw "DXC zip hash $hash does not match the pin $DxcSha256" }
    Remove-Item -Recurse -Force $dxcPrefix -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $dxcPrefix | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dxcPrefix -Force
    # dxc_2026_09_29.zip holds bin/x64/*; the package wants the DLLs flat.
    foreach ($name in @("dxcompiler.dll", "dxil.dll", "dxc.exe")) {
        $found = Get-ChildItem -Recurse -Path $dxcPrefix -Filter $name | Select-Object -First 1
        if ($found -and $found.DirectoryName -ne $dxcPrefix) {
            Copy-Item $found.FullName (Join-Path $dxcPrefix $name) -Force
        }
    }
    if (-not (Test-Path (Join-Path $dxcPrefix "dxil.dll"))) { throw "dxil.dll missing from the DXC zip" }
    $license = Get-ChildItem -Recurse -Path $dxcPrefix -Filter "LICENSE*" | Select-Object -First 1
    if ($license) { Copy-Item $license.FullName (Join-Path $dxcPrefix "LICENSE-DXC.txt") -Force }
}

@(
    "sdl3=$sdlCommit ($SDL3Branch)"
    "libuwp=$libCommit"
    "ffmpeg=$ffCommit ($FfmpegAsset)"
    "dxc=v1.9.2609 ($DxcSha256)"
    "toolset=v143, sdk=10.0, platform=$Platform, config=$Configuration"
) | Set-Content -Encoding utf8 $stamp
Write-Host "build-deps: done -> $prefix"
