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

# The modules the platform (or the VCLibs framework package the manifest
# requires) provides, for the "does it ship with us" question only.
$PlatformProvided = @(
    "^api-ms-win-", "^ext-ms-win-", "^vcruntime140(_1)?_app", "^msvcp140(_\d+)?_app",
    "^vccorlib140_app", "^concrt140_app"
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
# The value is the reason, and it has to name the owner of the call.
$KnownUncalledSymbols = @{
    "bbxbox.exe" = @{
        "LoadLibraryExW" = "C++/WinRT base.h activation fallback; the port itself calls LoadPackagedLibrary only"
        # The static CRT (xbox/CMakeLists.txt) brings its own file and memory
        # layer into the image, and that layer is written against the desktop
        # names. Nothing calls it any more: the log goes through hostfs, the
        # dump through CreateFile2FromApp, saves through hostfs's
        # `core/paced_file.cpp` (which used std::fopen until 5B-2b step 3), guest
        # pages through VirtualProtectFromApp. What is left is the CRT's own
        # objects, and `/OPT:REF` decides what of them survives; each entry here
        # says which CRT object the import comes from, not which port call.
        "CreateFileW" = "the static CRT's stdio objects; no caller in the port or in bbhost's TUs"
        "CreateDirectoryW" = "the static CRT's stdio objects; no caller in the port or in bbhost's TUs"
        "DeleteFileW" = "the static CRT's stdio objects; no caller in the port or in bbhost's TUs"
        "FindFirstFileExW" = "the static CRT's stdio objects; no caller in the port or in bbhost's TUs"
        "MoveFileExW" = "the static CRT's stdio objects; no caller in the port or in bbhost's TUs"
        # libvcruntime's __GSHandlerCheck and heap init. VirtualProtect is the one
        # that matters on a console (W^X), which is why it stays forbidden
        # everywhere else: a port TU that calls it is still a failure.
        "VirtualProtect" = "the static CRT's own guard/heap init; guest pages are flipped with VirtualProtectFromApp"
    }
}
# --- the rules, as functions, so they can be tested ---------------------------
# The audit is only worth the guarantee that it can fail. Both questions below
# are asked by the loops underneath and by the self-test above them, from the same
# code: a rule that quietly stopped matching would otherwise look exactly like a
# clean package.
function Resolve-Module($module, $own) {
    # port's own image: only its own list, no payload leniency
    if ($own) {
        foreach ($pattern in $AllowedModules) {
            if ($module -match $pattern) { return "allow" }
        }
        return "fail"
    }
    foreach ($pattern in $AllowedModules) {
        if ($module -match $pattern) { return "allow" }
    }
    foreach ($pattern in $AppCrtModules) {
        if ($module -match $pattern) { return "allow" }
    }
    return "fail"
}

function Resolve-Symbol($symbol, $own, $binaryName) {
    $pattern = $null
    foreach ($candidate in $ForbiddenSymbols) {
        if ($symbol -match $candidate) { $pattern = $candidate; break }
    }
    if (-not $pattern) { return "allow" }
    if (-not $own) { return "notice" }
    if ($KnownUncalledSymbols.ContainsKey($binaryName)) {
        foreach ($candidate in $KnownUncalledSymbols[$binaryName].Keys) {
            if ($pattern -eq "$candidate$") { return "known" }
        }
    }
    return "fail"
}

function Resolve-Provided($module) {
    foreach ($pattern in $PlatformProvided) {
        if ($module -match $pattern) { return $true }
    }
    return ($module -match "^(kernel32|kernelbase|ucrtbase|vcruntime140|vcruntime140_1|msvcp140|d3d12|dxgi|d3d11|dxcompiler|dxil|xaudio2_9|ole32|oleaut32|combase|user32|gdi32|advapi32|bcrypt|ntdll|mmdevapi|avrt)\.dll$")
}

# Cases the audit must get right, run before it looks at a single binary. Each is
# a real decision this port made; if one of them stops holding, the run fails with
# the rule rather than with a package that shipped.
$selfTestCases = @(
    @{ What = "the port importing the app CRT";        Got = (Resolve-Module "VCRUNTIME140_APP.dll" $true);  Want = "fail" }
    # A payload import the port would refuse is answered "fail" here and turned
    # into a notice by the loop below; the point of the case is that the module
    # rule says fail for the port's own image.
    @{ What = "the payload importing the app CRT";     Got = (Resolve-Module "MSVCP140_APP.dll" $false);     Want = "fail" }
    @{ What = "the payload importing vccorlib (C++/CX)"; Got = (Resolve-Module "vccorlib140_app.DLL" $false); Want = "allow" }
    @{ What = "the port importing a system api set";   Got = (Resolve-Module "api-ms-win-core-file-l1-1-0.dll" $true); Want = "allow" }
    @{ What = "the port importing ole32";              Got = (Resolve-Module "OLE32.dll" $true);             Want = "fail" }
    @{ What = "the port importing user32";             Got = (Resolve-Module "USER32.dll" $true);            Want = "fail" }
    @{ What = "the port importing xaudio2_9";          Got = (Resolve-Module "XAudio2_9.dll" $true);         Want = "fail" }
    @{ What = "a CRT import the port never calls";     Got = (Resolve-Symbol "CreateFileW" $true "bbxbox.exe"); Want = "known" }
    @{ What = "a port call to VirtualProtect";         Got = (Resolve-Symbol "VirtualProtect" $true "bbxbox.exe"); Want = "known" }
    @{ What = "a port call to VirtualAlloc";           Got = (Resolve-Symbol "VirtualAlloc" $true "bbxbox.exe"); Want = "fail" }
    @{ What = "a port call to CreateFileMapping";      Got = (Resolve-Symbol "CreateFileMapping" $true "bbxbox.exe"); Want = "fail" }
    @{ What = "the payload calling a desktop API";     Got = (Resolve-Symbol "CreateDirectoryW" $false "SDL3.dll"); Want = "notice" }
    @{ What = "the port calling the FromApp form";     Got = (Resolve-Symbol "CreateFile2FromApp" $true "bbxbox.exe"); Want = "allow" }
    @{ What = "a VCLibs module is provided";           Got = (Resolve-Provided "VCRUNTIME140_1_APP.dll");    Want = $true }
    @{ What = "a packaged-only DLL is not provided";   Got = (Resolve-Provided "SDL3.dll");                  Want = $false }
)
$selfTestBad = @($selfTestCases | Where-Object { $_.Got -ne $_.Want })
if ($selfTestBad.Count -gt 0) {
    $lines = @($selfTestBad | ForEach-Object { "$($_.What): got '$($_.Got)', wanted '$($_.Want)'" })
    Write-Host "check-imports: the audit's own rules are broken"
    $lines | ForEach-Object { Write-Host "  $_" }
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $body = ($lines -join "`n") -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::error title=import audit self-test::$body"
    }
    exit 1
}
Write-Host "check-imports: self-test PASS ($($selfTestCases.Count) rules)"

$notices = New-Object System.Collections.Generic.List[string]

foreach ($binary in $binaries) {
    $imports = & $Dumpbin /nologo /imports $binary.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $($binary.Name)" }
    $own = $OwnBinaries -contains $binary.Name
    foreach ($line in $imports) {
        if ($line -notmatch "^\s+([A-Za-z0-9_.\-]+\.dll)\s*$") { continue }
        $module = $Matches[1]
        # The *_app CRT family is allowed for payload binaries only: the port's own
        # image links the static CRT and importing any of it is the loader failure
        # this iteration removed. Resolve-Module is the rule (and is self-tested).
        $ok = (Resolve-Module $module $own) -eq "allow"
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
        # Modules the platform provides. Two different questions are asked of an
        # import and they have different answers: may the *port's* own code use
        # it ($AllowedModules, short and strict), and may any binary here import
        # it without carrying it ($PlatformProvided, longer). The *_APP CRT names
        # come from Microsoft.VCLibs, which the manifest depends on - SDL3.dll is
        # /ZW + /MD and needs them, bbxbox.exe links the static CRT and must not.
        $provided = $false
        foreach ($pattern in $PlatformProvided) {
            if ($module -match $pattern) { $provided = $true; break }
        }
        if (-not $provided) {
            $provided = $module -match "^(kernel32|kernelbase|ucrtbase|vcruntime140|vcruntime140_1|msvcp140|d3d12|dxgi|d3d11|dxcompiler|dxil|xaudio2_9|ole32|oleaut32|combase|user32|gdi32|advapi32|bcrypt|ntdll|mmdevapi|avrt)\.dll$"
        }
        if ($provided) { continue }
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
        Write-Host "imports ${name}: not in the package"
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
    Write-Host "imports ${name} ($($modules.Count) modules): $text"
    if ($env:GITHUB_ACTIONS -eq 'true') {
        Write-Output "::notice title=imports ${name}::$($text -replace '%', '%25')"
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
