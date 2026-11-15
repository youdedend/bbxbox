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

function Get-PeMachine([string]$Path) {
    # 0x8664 is x64. The DXC zip holds bin/arm64, bin/x64 and bin/x86, and a
    # recursive search for "dxc.exe" finds the arm64 one first - which then fails
    # on a runner with "This version of %1 is not compatible with the version of
    # Windows you're running", a message that names neither the file nor the cause.
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $buffer = New-Object byte[] 1024
        $read = $stream.Read($buffer, 0, $buffer.Length)
        $stream.Close()
        if ($read -lt 64) { return 0 }
        $peOffset = [System.BitConverter]::ToInt32($buffer, 0x3C)
        if ($peOffset -lt 0 -or $peOffset + 6 -ge $read) { return 0 }
        if ($buffer[$peOffset] -ne 0x50 -or $buffer[$peOffset + 1] -ne 0x45) { return 0 }
        return [System.BitConverter]::ToUInt16($buffer, $peOffset + 4)
    } catch {
        return 0
    }
}

function Install-DxcFlatten([string]$Prefix) {
    # The compiler and its two DLLs, side by side at the prefix root, from the x64
    # subdirectory only - and never trusting a previous flatten: a cached prefix
    # can hold a copy from before this rule existed.
    $archDir = Join-Path $Prefix "bin\x64"
    foreach ($name in @("dxc.exe", "dxcompiler.dll", "dxil.dll")) {
        $source = Join-Path $archDir $name
        if (-not (Test-Path $source)) {
            $source = (Get-ChildItem -Recurse -Path $Prefix -Filter $name -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -replace '/', '\\' -match '\\x64\\' } |
                Select-Object -First 1).FullName
        }
        if (-not $source -or -not (Test-Path $source)) { throw "$name is missing from the DXC package" }
        $destination = Join-Path $Prefix $name
        $arch = Get-PeMachine $source
        if ($arch -ne 0x8664) {
            throw "$source is not x64 (PE machine 0x$($arch.ToString('X4'))); the build machine cannot run it"
        }
        Copy-Item $source $destination -Force
        $flattened = Get-PeMachine $destination
        if ($flattened -ne 0x8664) {
            throw "$destination is not x64 after copying (PE machine 0x$($flattened.ToString('X4')))"
        }
    }
    Write-Host "build-deps: dxc.exe, dxcompiler.dll and dxil.dll flattened (x64)"
}

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
    # The dev shell puts cl.exe, link.exe, the Windows SDK and the UWP libs
    # (lib/x64/store) on PATH; CMake needs it. clang-cl is build.ps1's business,
    # not this script's: nothing here is compiled by it.
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    return $vs
}

function Step([string]$Name, [scriptblock]$Action) {
    Write-Host "build-deps: $Name"
    & $Action
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)" }
}

if (-not $Force -and (Test-Path $stamp)) {
    Write-Host "build-deps: reusing $stamp (pass -Force to rebuild)"
    Get-Content $stamp | ForEach-Object { Write-Host "  $_" }
    exit 0
}

try {
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
# The fork pins PlatformToolset v145 and SDK 10.0; retarget to what the runner
# has, and if the retarget itself is refused, let the project choose.
& $msbuild $sdlProject /m /p:Configuration=$Configuration /p:Platform=$Platform `
    /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 `
    /p:OutDir="$sdlOut\" /p:IntDir="$sdlOut\obj\" /v:minimal
if ($LASTEXITCODE -ne 0) {
    Write-Host "build-deps: SDL3 retargeted build failed; retrying with the project's own toolset"
    & $msbuild $sdlProject /m /p:Configuration=$Configuration /p:Platform=$Platform `
        /p:OutDir="$sdlOut\" /p:IntDir="$sdlOut\obj\" /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "SDL3-uwp build failed" }
}

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
# libuwp is C++/WinRT: its own presets pin `CMAKE_SYSTEM_NAME=WindowsStore`
# with a Visual Studio generator, so use whichever VS generator the runner has
# (17 2022 -> vs2022, 18 2026 -> vs2026). Nothing in the app calls into libuwp
# yet, so a failure here is a warning, not a build failure -- but it is logged
# and annotated so it cannot rot silently.
$generators = (cmake --help | Out-String)
$preset = if (Test-Path "$libPath\CMakePresets.json") {
    if ($generators -match 'Visual Studio 18 2026') { "vs2026" }
    elseif ($generators -match 'Visual Studio 17 2022') { "vs2022" }
    else { $null }
} else { $null }
$libOk = $false
$libLog = Join-Path $root "build-deps\libuwp.log"
# libuwp still uses /await + <experimental/coroutine>, which VS 2022 17.10+
# (and 18) reject as a hard STL error; silence it for this dependency only.
$libFlags = "/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS /D_CRT_SECURE_NO_WARNINGS"
if ($preset) {
    & cmake --preset $preset -S $libPath "-DCMAKE_CXX_FLAGS=$libFlags" "-DCMAKE_C_FLAGS=$libFlags" 2>&1 | Tee-Object -FilePath $libLog
    if ($LASTEXITCODE -eq 0) { & cmake --build (Join-Path $libPath "build-$preset") --config $Configuration 2>&1 | Tee-Object -FilePath $libLog -Append }
    $libOk = ($LASTEXITCODE -eq 0)
}
if (-not $libOk) {
    & cmake -S $libPath -B $libOut -A $Platform -DCMAKE_SYSTEM_NAME=WindowsStore `
        -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DCMAKE_BUILD_TYPE=$Configuration `
        "-DCMAKE_CXX_FLAGS=$libFlags" "-DCMAKE_C_FLAGS=$libFlags" 2>&1 |
        Tee-Object -FilePath $libLog
    if ($LASTEXITCODE -eq 0) { & cmake --build $libOut --config $Configuration 2>&1 | Tee-Object -FilePath $libLog -Append; $libOk = ($LASTEXITCODE -eq 0) }
}
if (-not $libOk) {
    Write-Host "build-deps: WARNING libuwp did not build; see $libLog"
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $tail = (Get-Content $libLog -Tail 25 | Out-String) -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::warning title=libuwp not built (unused so far)::$tail"
    }
    $libCommit = "not built"
} else {
    $libCommit = (git -C $libPath rev-parse HEAD).Trim()
}

$libPrefix = Join-Path $prefix "libuwp"
New-Item -ItemType Directory -Force -Path (Join-Path $libPrefix "lib"), (Join-Path $libPrefix "bin") | Out-Null
Copy-Item -Recurse -Force (Join-Path $libPath "src") (Join-Path $libPrefix "include")
$libDll = Get-ChildItem -Recurse -Path $libPath -Filter "libuwp.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
$libLib = Get-ChildItem -Recurse -Path $libPath -Filter "libuwp.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($libDll -and $libLib) {
    Copy-Item $libLib.FullName (Join-Path $libPrefix "lib\libuwp.lib")
    Copy-Item $libDll.FullName (Join-Path $libPrefix "bin\libuwp.dll")
} else {
    $libCommit = "headers only"
    Write-Host "build-deps: libuwp headers staged without binaries (nothing links it yet)"
}

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
    # dxc_2026_09_29.zip holds bin/x64/*; the build wants the three files flat,
    # and only from bin/x64 (see Install-DxcFlatten).
    Install-DxcFlatten $dxcPrefix
    if (-not (Test-Path (Join-Path $dxcPrefix "dxil.dll"))) { throw "dxil.dll missing from the DXC zip" }
    $license = Get-ChildItem -Recurse -Path $dxcPrefix -Filter "LICENSE*" | Select-Object -First 1
    if ($license) { Copy-Item $license.FullName (Join-Path $dxcPrefix "LICENSE-DXC.txt") -Force }
} else {
    # A cached prefix: re-flatten anyway. It costs three local copies and it means
    # a cache that predates a fix cannot keep the build broken.
    Install-DxcFlatten $dxcPrefix
}

}
catch {
    $message = "$_"
    Write-Host "build-deps: FAILED: $message"
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $enc = $message -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::error title=build-deps failed::$enc"
    }
    exit 1
}

@(
    "sdl3=$sdlCommit ($SDL3Branch)"
    "libuwp=$libCommit"
    "ffmpeg=$ffCommit ($FfmpegAsset)"
    "dxc=v1.9.2609 ($DxcSha256, flattened from bin/x64)"
    "toolset=v143, sdk=10.0, platform=$Platform, config=$Configuration"
) | Set-Content -Encoding utf8 $stamp
Write-Host "build-deps: done -> $prefix"
