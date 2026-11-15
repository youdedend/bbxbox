# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Configures and builds bbxbox.exe for Xbox (WindowsStore, x64) with clang-cl.

.DESCRIPTION
  Ninja, not the Visual Studio generator: `-T ClangCL` with
  `CMAKE_SYSTEM_NAME=WindowsStore` is refused or silently misconfigured by several
  CMake versions, while `-G Ninja` with `CMAKE_CXX_COMPILER=clang-cl` is what the
  prior art's Xbox build uses and what this script follows. The dev shell is
  entered first so link.exe, the Windows SDK and the **Store CRT**
  (`VC/Tools/MSVC/<ver>/lib/x64/store`) are on the environment; without that
  directory the link picks the desktop CRT and the package fails import
  validation on a console.

  Failures are published as GitHub annotations as well as text: the Actions log
  endpoint is not always reachable after a run, an annotation always is.
#>
[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$Configuration = "RelWithDebInfo",
    [switch]$Clean,
    [switch]$SyntaxOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Emit-ErrorAnnotation([string]$Title, [string]$Body) {
    if ($env:GITHUB_ACTIONS -eq 'true' -and $Body) {
        $enc = $Body -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($enc.Length -gt 55000) { $enc = $enc.Substring($enc.Length - 55000) }
        Write-Output "::error title=$Title::$enc"
    }
}

$prefix = Join-Path $Root "build-deps\prefix"

function Invoke-Build {
$build = Join-Path $Root "build\xbox"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

if (-not (Test-Path (Join-Path $prefix "sdl3\include\SDL3\SDL.h"))) {
    throw "dependencies missing; run xbox/scripts/build-deps.ps1 first"
}

# --- toolchain --------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $vs) { throw "no Visual Studio with the C++ toolset" }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$clangCl = "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if (-not (Test-Path $clangCl)) {
    throw "clang-cl missing at $clangCl; install the 'Microsoft.VisualStudio.Component.VC.Llvm.Clang' component"
}
$env:CC = $clangCl
$env:CXX = $clangCl
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"

# The Store CRT lives beside the desktop one; the link must prefer it.
$msvcRoot = Get-ChildItem -Path (Join-Path $vs "VC\Tools\MSVC") -Directory | Sort-Object Name -Descending |
    Select-Object -First 1
$storeLib = Join-Path $msvcRoot.FullName "lib\x64\store"
if (-not (Test-Path $storeLib)) {
    Write-Output "::warning title=Store CRT missing::$storeLib not found; the package may fail import validation"
}

# --- configure --------------------------------------------------------------
$cfgLog = Join-Path $Root "build\xbox-configure.log"
$cfgArgs = @(
    "-S", (Join-Path $Root "xbox"),
    "-B", $build,
    "-G", "Ninja",
    "-DCMAKE_SYSTEM_NAME=WindowsStore",
    "-DCMAKE_SYSTEM_VERSION=10.0",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
    "-DBBX_SDL3_PREFIX=$prefix\sdl3",
    "-DBBX_LIBUWP_PREFIX=$prefix\libuwp",
    "-DBBX_DXC_PREFIX=$prefix\dxc",
    "-DBBX_FFMPEG_PREFIX=$prefix\ffmpeg",
    "-DBBX_BBHOST_ROOT=$Root\upstream\bbhost"
)
# CMAKE_LIBRARY_PATH only seeds find_library; the link needs the directory too,
# which is what BBX_STORE_CRT_DIR does (target_link_directories BEFORE).
if (Test-Path $storeLib) {
    $cfgArgs += "-DCMAKE_LIBRARY_PATH=$storeLib"
    $cfgArgs += "-DBBX_STORE_CRT_DIR=$storeLib"
    # Which import libraries the store CRT actually ships decides whether the
    # _app names can be linked; say it out loud, as a notice, because the run log
    # is not always reachable.
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $names = (Get-ChildItem -Path $storeLib -Filter *.lib | Select-Object -ExpandProperty Name) -join ", "
        Write-Output "::notice title=store CRT::$names"
    }
}
& cmake @cfgArgs *> $cfgLog
if ($LASTEXITCODE -ne 0) {
    $tail = (Get-Content $cfgLog -Tail 80) -join "`n"
    Write-Output $tail
    Emit-ErrorAnnotation 'CMake configure failed' $tail
    throw "xbox CMake configure failed"
}

# --- build ------------------------------------------------------------------
$buildLog = Join-Path $Root "build\xbox-build.log"
$buildArgs = @("--build", $build, "--parallel", "$([Environment]::ProcessorCount)")
if ($SyntaxOnly) { $buildArgs += "--target", "bbxbox_syntax" }
& cmake @buildArgs *> $buildLog
if ($LASTEXITCODE -ne 0) {
    $lines = @(Get-Content $buildLog)
    # Ninja prints the command and the tool's diagnostics *after* its FAILED
    # header, so a filter on 'error:' alone misses tools that report failure with
    # a bare message (dxc, cmd, lld-link). Keep a window around each FAILED.
    $errors = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match 'FAILED:|error:|fatal error:|undefined symbol|dxc:') {
            $end = [Math]::Min($i + 12, $lines.Count - 1)
            for ($j = $i; $j -le $end; ++$j) { $errors.Add($lines[$j]) }
            $i = $end
        }
    }
    if ($errors.Count -eq 0) { $errors.AddRange([string[]]($lines | Select-Object -Last 80)) }
    $summary = ($errors | Select-Object -First 80) -join "`n"
    Write-Output $summary
    Emit-ErrorAnnotation 'CMake build failed' $summary
    throw "xbox build failed (see build/xbox-build.log)"
}

$exe = Get-ChildItem -Recurse -Path $build -Filter "bbxbox.exe" | Select-Object -First 1
if (-not $exe) { throw "bbxbox.exe not produced" }
Write-Host "built $($exe.FullName)"
}

try {
    Invoke-Build
}
catch {
    $message = "$_"
    Write-Host "build: FAILED: $message"
    $dump = @($message)
    if (Test-Path $prefix) {
        $dump += "--- $prefix ---"
        $dump += @(Get-ChildItem -Recurse -Depth 2 -Path $prefix -ErrorAction SilentlyContinue |
            Select-Object -First 60 | ForEach-Object { $_.FullName.Substring($prefix.Length) })
    }
    Emit-ErrorAnnotation 'build failed' ($dump -join "`n")
    if ($env:GITHUB_ACTIONS -eq 'true') { exit 1 }
    throw
}
