# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Lays out the package, audits its imports and packs the .msix/.appx.

.DESCRIPTION
  The package contains exactly: bbxbox.exe, its .map and .pdb next to it for
  symbolication, the dependency DLLs (SDL3.dll, libuwp.dll, and dxcompiler.dll +
  dxil.dll when the DXIL path is enabled), the manifest and the assets. No game
  data, ever; the dump is read from wherever the player granted access.
#>
[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$Configuration = "Release",
    [string]$Certificate = "",
    [string]$Password = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    Write-Host "package: FAILED: $Message"
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $enc = $Message -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($enc.Length -gt 55000) { $enc = $enc.Substring($enc.Length - 55000) }
        Write-Output "::error title=package failed::$enc"
    }
    exit 1
}

try {

$build = Join-Path $Root "build\xbox"
$prefix = Join-Path $Root "build-deps\prefix"
$layout = Join-Path $Root "build\package"
$out = Join-Path $Root "build\appx"

if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force -Path $layout, $out, (Join-Path $layout "Assets") | Out-Null

$exe = Get-ChildItem -Recurse -Path $build -Filter "bbxbox.exe" | Select-Object -First 1
if (-not $exe) { throw "build first: xbox/scripts/build.ps1" }
Copy-Item $exe.FullName (Join-Path $layout "bbxbox.exe")

foreach ($name in @("bbxbox.map", "bbxbox.pdb")) {
    $found = Get-ChildItem -Recurse -Path $build -Filter $name | Select-Object -First 1
    if ($found) { Copy-Item $found.FullName (Join-Path $layout $name) }
}

foreach ($dll in @("sdl3\bin\SDL3.dll", "sdl3\bin\SDL3.pdb", "libuwp\bin\libuwp.dll")) {
    $path = Join-Path $prefix $dll
    if (Test-Path $path) { Copy-Item $path (Join-Path $layout (Split-Path $path -Leaf)) }
}
# DXIL: the compiler is a packaged DLL, loaded with LoadPackagedLibrary.
foreach ($dll in @("dxcompiler.dll", "dxil.dll")) {
    $path = Join-Path $prefix "dxc\$dll"
    if (Test-Path $path) { Copy-Item $path (Join-Path $layout $dll) }
}

Copy-Item (Join-Path $Root "xbox\AppxManifest.xml") (Join-Path $layout "AppxManifest.xml")
$assets = Join-Path $Root "xbox\Assets"
Get-ChildItem -Path $assets -Filter *.png -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $layout "Assets\$($_.Name)") }
if (-not (Get-ChildItem (Join-Path $layout "Assets") -Filter *.png)) {
    Write-Warning "no assets in xbox/Assets; the manifest references StoreLogo.png and friends"
}

& (Join-Path $PSScriptRoot "check-imports.ps1") -PackageDir $layout
if ($LASTEXITCODE -ne 0) { throw "import audit failed; not packaging" }

$appx = Join-Path $out "bbxbox_0.1.0.0_x64.appx"
# makeappx ships with the Windows SDK.
$makeappx = Get-Command makeappx.exe -ErrorAction SilentlyContinue
if (-not $makeappx) {
    $sdk = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $makeappx = Get-ChildItem -Recurse -Path $sdk -Filter makeappx.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } | Select-Object -First 1
}
if (-not $makeappx) { throw "makeappx.exe not found (Windows SDK)" }
& $makeappx.FullName pack /d $layout /p $appx /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

if ($Certificate) {
    & (Join-Path $PSScriptRoot "sign.ps1") -Package $appx -Certificate $Certificate -Password $Password
}

Write-Host "package: $appx"
Write-Host "install: Add-AppxPackage -Path `"$appx`" (Developer Mode)"
}
catch {
    Fail "$_"
}
