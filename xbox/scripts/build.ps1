# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Configures and builds bbxbox.exe for Xbox (WindowsStore, x64) with clang-cl.

.DESCRIPTION
  Dependencies must already be in build-deps/prefix (build-deps.ps1). The build
  is incremental by design: dependencies are cached, and only the app's own
  translation units are recompiled, so a CI push costs 2-4 minutes.
#>
[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$build = Join-Path $Root "build\xbox"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$prefix = Join-Path $Root "build-deps\prefix"
if (-not (Test-Path (Join-Path $prefix "sdl3\include\SDL3\SDL.h"))) {
    throw "dependencies missing; run xbox/scripts/build-deps.ps1 first"
}

# clang-cl comes from the VS LLVM component; cmake picks it up with -T ClangCL.
& cmake -S (Join-Path $Root "xbox") -B $build `
    -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 `
    -A x64 -T ClangCL `
    -DCMAKE_BUILD_TYPE=$Configuration `
    -DBBX_SDL3_PREFIX="$prefix\sdl3" `
    -DBBX_LIBUWP_PREFIX="$prefix\libuwp" `
    -DCMAKE_CXX_FLAGS="/std:c++20 /permissive- /EHsc /W4 /WX /Zc:__cplusplus"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Get-ChildItem -Recurse -Path $build -Filter "bbxbox.exe" | Select-Object -First 1
if (-not $exe) { throw "bbxbox.exe not produced" }
Write-Host "built $($exe.FullName)"
