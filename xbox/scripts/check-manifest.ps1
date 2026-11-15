# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Lints xbox/AppxManifest.xml before makeappx sees it.

.DESCRIPTION
  makeappx reports one schema violation at a time and stops; every round trip is
  a full CI cycle. This check is the list of things this port has decided about
  its manifest, so a regression shows up as a sentence instead of a schema error:

   * x64, Windows.Universal (the console's app model), one Application whose
     Executable and EntryPoint agree with the code;
   * capabilities: codeGeneration and removableStorage, nothing else - no
     runFullTrust, no broadFileSystemAccess, no internetClient;
   * every Assets\*.png the manifest names exists on disk.

  It does not replace makeappx: the tool is still the authority on the schema.
#>
[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$manifestPath = Join-Path $Root "xbox\AppxManifest.xml"
if (-not (Test-Path $manifestPath)) { throw "xbox/AppxManifest.xml is missing" }
[xml]$manifest = Get-Content -Raw $manifestPath

$problems = New-Object System.Collections.Generic.List[string]

if ($manifest.Package.Identity.ProcessorArchitecture -ne "x64") {
    $problems.Add("Identity ProcessorArchitecture must be x64 (the console is x64 only)")
}
if (-not $manifest.Package.Identity.Name) { $problems.Add("Identity Name is empty") }
if (-not $manifest.Package.Identity.Publisher) { $problems.Add("Identity Publisher is empty") }
if (-not $manifest.Package.Identity.Version) { $problems.Add("Identity Version is empty") }

$family = $manifest.Package.Dependencies.TargetDeviceFamily
if ($family.Name -ne "Windows.Universal") {
    $problems.Add("TargetDeviceFamily must be Windows.Universal, found '$($family.Name)'")
}
if (-not $manifest.Package.PhoneIdentity) {
    $problems.Add("the Windows.Universal family requires mp:PhoneIdentity")
}

$applications = @($manifest.Package.Applications.Application)
if ($applications.Count -ne 1) {
    $problems.Add("exactly one Application is expected, found $($applications.Count)")
} else {
    $app = $applications[0]
    if ($app.Executable -ne "bbxbox.exe") {
        $problems.Add("Application Executable must be bbxbox.exe, found '$($app.Executable)'")
    }
    # The packaged launch activates this name through the EXE's exported
    # DllGetActivationFactory (xbox/bbxbox.def), so it has to be the port's class.
    if ($app.EntryPoint -ne "bbxbox.App") {
        $problems.Add("Application EntryPoint must be bbxbox.App, found '$($app.EntryPoint)'")
    }
}

# The capabilities are read from the element names and namespaces: <rescap:Capability>
# is a Capability element in the restricted-capabilities namespace, not a separate
# XML name, so asking PowerShell for "RestrictedCapability" finds nothing.
$capabilityElements = @($manifest.Package.Capabilities.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })
$names = @($capabilityElements |
        Where-Object { $_.LocalName -eq 'Capability' -and $_.NamespaceURI -notlike '*restrictedcapabilities*' } |
        ForEach-Object { $_.Name })
$restricted = @($capabilityElements |
        Where-Object { $_.LocalName -eq 'Capability' -and $_.NamespaceURI -like '*restrictedcapabilities*' } |
        ForEach-Object { $_.Name })
$device = @($capabilityElements | Where-Object { $_.LocalName -eq 'DeviceCapability' } | ForEach-Object { $_.Name })

foreach ($required in @("codeGeneration")) {
    if ($names -notcontains $required) { $problems.Add("capability '$required' is required") }
}
if ($restricted -notcontains "removableStorage") {
    $problems.Add("restricted capability 'removableStorage' is required (the dump lives on a USB drive)")
}
if ($device -notcontains "gamepad") { $problems.Add("device capability 'gamepad' is required") }
foreach ($forbidden in @("runFullTrust", "broadFileSystemAccess", "internetClient", "internetClientServer", "privateNetworkClientServer", "microphone", "webcam", "documentsLibrary", "picturesLibrary", "videosLibrary", "enterpriseAuthentication", "sharedUserCertificates")) {
    if (($names -contains $forbidden) -or ($restricted -contains $forbidden)) {
        $problems.Add("capability '$forbidden' must not appear; the port does not need it")
    }
}

$text = Get-Content -Raw $manifestPath
# A PowerShell single-quoted string keeps backslashes as written, so the prefix
# is exactly 'Assets' + one backslash; the pattern is escaped from it rather than
# written with escapes inline.
$assetPrefix = 'Assets\'
$assetPattern = [regex]::Escape($assetPrefix) + '[A-Za-z0-9]+\.png'
$missing = @()
foreach ($asset in ([regex]::Matches($text, $assetPattern) | ForEach-Object { $_.Value } | Sort-Object -Unique)) {
    # The manifest path is relative to the package root, which is xbox/.
    $path = Join-Path $Root (Join-Path 'xbox' $asset)
    if (-not (Test-Path $path)) { $missing += "$asset (looked for $path)" }
}
foreach ($asset in ($missing | Sort-Object -Unique)) {
    $problems.Add("manifest names '$asset' but xbox/$asset does not exist (run tools/make-assets.py)")
}

if ($problems.Count -gt 0) {
    Write-Host "check-manifest: FAIL"
    $unique = @($problems | Sort-Object -Unique)
    $unique | ForEach-Object { Write-Host "  $_" }
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $body = ($unique -join "`n") -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::error title=manifest lint::$body"
    }
    exit 1
}
Write-Host "check-manifest: PASS"
