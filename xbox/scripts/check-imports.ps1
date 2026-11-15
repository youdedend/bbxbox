# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Audits what the package imports, before it reaches a console.

.DESCRIPTION
  A UWP app that calls a desktop-only API does not fail at build time; it fails
  on the console, in someone's living room, after the store validation step. So
  every binary in the package is listed by dumpbin and checked twice:

   1. the DLLs it imports must be packaged with it or part of the AppContainer's
      allowed set (WindowsApp.lib's api sets, ucrt, vcruntime, d3d12, dxgi, SDL3);
   2. the *symbols* it imports from kernel32/user32 must be the FromApp variants.
      CreateFileW and VirtualAlloc are importable and then denied at run time in
      an AppContainer; CreateFile2FromApp and VirtualAlloc2FromApp are the ones
      that work. This is the check the prior art hit the hard way.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [string]$Dumpbin = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $Dumpbin) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
    $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
    $Dumpbin = Get-ChildItem -Path (Join-Path $vs "VC\Tools\MSVC") -Recurse -Filter dumpbin.exe |
        Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1 -ExpandProperty FullName
}
if (-not (Test-Path $Dumpbin)) { throw "dumpbin.exe not found" }

# Imports the AppContainer guarantees. api-ms-win-* covers WindowsApp.lib's
# surface; the rest are the graphics, audio and CRT libraries the port links.
$AllowedModules = @(
    "^api-ms-win-", "^ext-ms-win-", "^ucrtbase\.dll$", "^vcruntime140", "^msvcp140",
    "^kernel32\.dll$", "^kernelbase\.dll$", "^d3d12\.dll$", "^dxgi\.dll$", "^d3d11\.dll$",
    "^dxcompiler\.dll$", "^dxil\.dll$", "^xaudio2_9\.dll$", "^SDL3\.dll$", "^libuwp\.dll$",
    "^ole32\.dll$", "^windows\.storage\.dll$", "^combase\.dll$", "^sechost\.dll$",
    "^powrprof\.dll$", "^user32\.dll$", "^gdi32\.dll$", "^advapi32\.dll$", "^shcore\.dll$",
    "^bcrypt\.dll$", "^ntdll\.dll$", "^rpcrt4\.dll$", "^mmdevapi\.dll$", "^avrt\.dll$",
    "^dwrite\.dll$", "^d2d1\.dll$", "^windowscodecs\.dll$", "^setupapi\.dll$", "^cfgmgr32\.dll$"
)

# Symbols that exist but are refused at run time inside an AppContainer. Each has
# a FromApp replacement, and the port must use it.
$ForbiddenSymbols = @(
    "VirtualAlloc$", "VirtualAlloc2$", "VirtualProtect$", "VirtualProtect2$",
    "MapViewOfFile$", "MapViewOfFile2$", "MapViewOfFile3$", "CreateFileMapping$",
    "CreateFileMapping2$", "CreateFileA$", "CreateFileW$", "FindFirstFileA$",
    "FindFirstFileW$", "FindFirstFileExA$", "FindFirstFileExW$", "LoadLibraryA$",
    "LoadLibraryW$", "LoadLibraryExA$", "LoadLibraryExW$", "GetFileAttributesA$",
    "GetFileAttributesW$", "CreateDirectoryA$", "CreateDirectoryW$",
    "CreateFile2$", "CopyFileA$", "CopyFileW$", "MoveFileA$", "MoveFileW$",
    "MoveFileExA$", "MoveFileExW$", "DeleteFileA$", "DeleteFileW$"
)

$failures = New-Object System.Collections.Generic.List[string]
$binaries = Get-ChildItem -Path $PackageDir -Recurse -Include *.exe, *.dll
if ($binaries.Count -eq 0) { throw "no binaries under $PackageDir" }

foreach ($binary in $binaries) {
    $imports = & $Dumpbin /nologo /imports $binary.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $($binary.Name)" }
    foreach ($line in $imports) {
        if ($line -notmatch "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { continue }
        $module = $Matches[1]
        $ok = $false
        foreach ($pattern in $AllowedModules) {
            if ($module -match $pattern) { $ok = $true; break }
        }
        if (-not $ok) { $failures.Add("$($binary.Name): imports $module") }
    }
    foreach ($pattern in $ForbiddenSymbols) {
        if ($imports -match "\b$pattern" -or ($imports | Where-Object { $_ -match $pattern })) {
            $failures.Add("$($binary.Name): imports a desktop-only symbol matching /$pattern/")
        }
    }
}

# Everything the package's binaries import must ship with it, unless it is part
# of Windows or an allowed system module.
$packaged = $binaries | ForEach-Object { $_.Name }
foreach ($binary in $binaries) {
    $imports = & $Dumpbin /nologo /imports $binary.FullName 2>&1
    foreach ($line in $imports) {
        if ($line -notmatch "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { continue }
        $module = $Matches[1]
        if ($module -match "^api-ms-win-" -or $module -match "^(kernel32|kernelbase|ucrtbase|vcruntime140|msvcp140|d3d12|dxgi|d3d11|dxcompiler|dxil|xaudio2_9|ole32|combase|user32|gdi32|advapi32|bcrypt|ntdll|mmdevapi|avrt)\.dll$") {
            continue
        }
        if ($packaged -notcontains $module) { $failures.Add("$($binary.Name): imports $module but it is not in the package") }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "check-imports: FAIL"
    $unique = @($failures | Sort-Object -Unique)
    $unique | ForEach-Object { Write-Host "  $_" }
    if ($env:GITHUB_ACTIONS -eq 'true') {
        # The annotation is the only channel that survives the run afterwards.
        $body = ($unique -join "`n") -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($body.Length -gt 55000) { $body = $body.Substring($body.Length - 55000) }
        Write-Output "::error title=import audit::$body"
    }
    exit 1
}
Write-Host "check-imports: PASS ($($binaries.Count) binaries)"
