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
# The Store CRT family: a UWP app loads vcruntime140_app.dll / msvcp140_app.dll /
# vccorlib140_app.dll and their siblings, never the desktop vcruntime140.dll. The
# OS provides the *_app family (it is part of the platform, not the package), so
# they are allowed as modules and skipped by the packaged check; the desktop
# names are *not* allowed, because a console does not have them.
# bbxbox.exe links the static CRT (/MT, xbox/CMakeLists.txt), so it must import
# *no* CRT module at all. The *_app DLLs stay allowed only for the payload
# binaries: SDL3.dll is C++/CX and imports vccorlib140_app, and the siblings come
# from the Microsoft.VCLibs framework package that the manifest depends on. The
# desktop names (vcruntime140.dll, msvcp140.dll) are never allowed for anyone.
$AppCrtModules = @("^vccorlib140_app", "^msvcp140_\d")
# Nothing here is a desktop-only library. The list used to carry user32, gdi32,
# ole32, advapi32 and xaudio2_9 "because the console has them"; that is true and
# beside the point - an AppContainer title that imports them is a title holding
# a pointer it cannot use, and each allowance hid a call site nobody had moved to
# the WinRT/FromApp shape. They are gone, and so are the CRT DLLs.
$AllowedModules = @(
    "^api-ms-win-", "^ext-ms-win-", "^ucrtbase\.dll$",
    "^oleaut32\.dll$", "^combase\.dll$",
    "^kernel32\.dll$", "^kernelbase\.dll$", "^d3d12\.dll$", "^dxgi\.dll$", "^d3d11\.dll$",
    # No dxcompiler.dll / dxil.dll: the package does not carry a shader compiler
    # (DXIL is compiled at build time), so importing one is a packaging bug, not
    # an allowance. An import that is not in this list is a failure.
    "^SDL3\.dll$", "^libuwp\.dll$",
    "^windows\.storage\.dll$", "^sechost\.dll$",
    "^powrprof\.dll$", "^shcore\.dll$",
    "^bcrypt\.dll$", "^ntdll\.dll$", "^rpcrt4\.dll$", "^mmdevapi\.dll$", "^avrt\.dll$",
    "^windowscodecs\.dll$"
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

# Binaries this port builds. Their imports are the port's responsibility. The
# rest (SDL3-uwp, libuwp, Microsoft's DXC) is payload: it is shipped, not written
# here, and its desktop imports are reported as notices so a reviewer can see
# them without the job turning red on code we do not own.
$OwnBinaries = @("bbxbox.exe")
# Exact symbols that are imported but never called on the console's paths. Each
# one is here for a stated reason; the list is not a place to hide a mistake.
$KnownUncalledSymbols = @{
    "bbxbox.exe" = @{
        "LoadLibraryExW" = "C++/WinRT base.h activation fallback; the port itself calls LoadPackagedLibrary only"
    }
}
$notices = New-Object System.Collections.Generic.List[string]

foreach ($binary in $binaries) {
    $imports = & $Dumpbin /nologo /imports $binary.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $($binary.Name)" }
    $own = $OwnBinaries -contains $binary.Name
    foreach ($line in $imports) {
        if ($line -notmatch "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { continue }
        $module = $Matches[1]
        $ok = $false
        foreach ($pattern in $AllowedModules) {
            if ($module -match $pattern) { $ok = $true; break }
        }
        if (-not $ok -and $module -match ("(" + ($AppCrtModules -join "|") + ")")) { $ok = $true }
        if (-not $ok) {
            if ($own) { $failures.Add("$($binary.Name): imports $module") }
            else { $notices.Add("$($binary.Name) (payload): imports $module") }
        }
    }
    foreach ($pattern in $ForbiddenSymbols) {
        if ($imports -match "\b$pattern" -or ($imports | Where-Object { $_ -match $pattern })) {
            $allowed = $false
            if ($KnownUncalledSymbols.ContainsKey($binary.Name)) {
                foreach ($symbol in $KnownUncalledSymbols[$binary.Name].Keys) {
                    if ($pattern -eq "$symbol$") { $allowed = $true; break }
                }
            }
            if (-not $own) { $notices.Add("$($binary.Name) (payload): imports a desktop-only symbol matching /$pattern/") }
            elseif ($allowed) { $notices.Add("$($binary.Name): imports $symbol, documented as never called") }
            else {
                # The symbol alone does not say where it came from: the module it
                # is imported from does, and so do the names beside it in that
                # module (they are all one translation unit's worth of calls).
                $lines = @($imports)
                $at = -1
                for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i] -match $pattern) { $at = $i; break } }
                $where = ""
                if ($at -ge 0) {
                    $module = "?"
                    $from = $at
                    for ($i = $at; $i -ge 0; --$i) {
                        if ($lines[$i] -match "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { $module = $Matches[1]; $from = $i; break }
                    }
                    $names = @($lines[$from..$at] | ForEach-Object {
                        if ($_ -match '^\s+[0-9A-F]+\s+([A-Za-z_][A-Za-z0-9_]*)$') { $Matches[1] } })
                    # Only the names, not dumpbin's addresses: the annotation has
                    # a length limit and the neighbours are the useful half.
                    $where = " (from $module, with " + ($names -join ', ') + ")"
                }
                $failures.Add("$($binary.Name): imports a desktop-only symbol matching /$pattern/$where")
            }
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
        # Modules the platform provides: an import of one is a packaging question
        # only for the *port's* binaries (they may not use them, above), while a
        # payload DLL importing one is Microsoft's business and reaches here as a
        # notice. That is why this list is longer than $AllowedModules.
        if ($module -match "^api-ms-win-" -or $module -match "^(kernel32|kernelbase|ucrtbase|vcruntime140|vcruntime140_1|msvcp140|d3d12|dxgi|d3d11|dxcompiler|dxil|xaudio2_9|ole32|oleaut32|combase|user32|gdi32|advapi32|bcrypt|ntdll|mmdevapi|avrt)\.dll$" -or
            $module -match ("(" + ($AppCrtModules -join "|") + ")")) {
            continue
        }
        if ($packaged -notcontains $module) { $failures.Add("$($binary.Name): imports $module but it is not in the package") }
    }
}

if ($notices.Count -gt 0) {
    $uniqueNotices = @($notices | Sort-Object -Unique)
    Write-Host "check-imports: notices (not failures)"
    $uniqueNotices | ForEach-Object { Write-Host "  $_" }
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $body = ($uniqueNotices -join "`n") -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($body.Length -gt 55000) { $body = $body.Substring($body.Length - 55000) }
        Write-Output "::warning title=import audit notices::$body"
    }
}

# --- what the three binaries actually import --------------------------------
# The audit above answers a yes/no question; when the answer is no, the next
# question is always "imports *what*?". dumpbin's table goes to
# `build/imports-<name>.log` (the build-logs artifact picks `build/*.log` up)
# and the module list goes to an annotation, because a run's log is not always
# reachable afterwards. The three are the ones a load failure can come from: the
# port's own image, and the two DLLs the package ships.
$watched = @("bbxbox.exe", "SDL3.dll", "libuwp.dll")
$importLogDir = Split-Path $PackageDir -Parent
foreach ($name in $watched) {
    $path = Join-Path $PackageDir $name
    if (-not (Test-Path $path)) {
        Write-Host "imports $name: not in the package"
        continue
    }
    $table = @(& $Dumpbin /nologo /imports $path 2>&1)
    if (Test-Path $importLogDir) {
        Set-Content -Path (Join-Path $importLogDir "imports-$name.log") -Value $table -Encoding utf8
    }
    $modules = @($table | ForEach-Object { if ($_ -match "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { $Matches[1] } } |
        Sort-Object -Unique)
    $text = ($modules -join ", ")
    if ($text.Length -gt 3500) { $text = $text.Substring(0, 3500) + " ..." }
    Write-Host "imports $name ($($modules.Count) modules): $text"
    if ($env:GITHUB_ACTIONS -eq 'true') {
        Write-Output "::notice title=imports $name::$($text -replace '%', '%25')"
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
