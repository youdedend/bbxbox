# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Compiles one HLSL entry point to DXIL, with its diagnostics on the annotation.

.DESCRIPTION
  xbox/CMakeLists.txt calls this instead of dxc directly for a reason that cost a
  debugging round trip: when dxc fails inside a CMake custom command, what lands
  in the build log is ninja's "FAILED:" header and, sometimes, nothing else. This
  wrapper prints the exact command line, streams both pipes, and turns every dxc
  diagnostic into a ::error annotation, so a shader error is readable from the
  pull request.

  The target is always *_6_4 and -WX is always on: the console runs SM 6.4 and a
  warning here is a PSO-creation failure on the TV. -enable-16bit-types is never
  passed (the console has no native 16-bit ALU), and nothing may use
  ResourceDescriptorHeap (SM 6.6).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dxc,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Profile,
    [Parameter(Mandatory = $true)][string]$Entry,
    [Parameter(Mandatory = $true)][string]$Source
)

$ErrorActionPreference = "Stop"

function Annotate([string]$Message) {
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $enc = $Message -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($enc.Length -gt 55000) { $enc = $enc.Substring(0, 55000) }
        Write-Output "::error title=dxc $Entry::$enc"
    }
}

if (-not (Test-Path $Dxc)) {
    Annotate "dxc.exe is missing at $Dxc (run xbox/scripts/build-deps.ps1)"
    Write-Host "dxc: missing $Dxc"
    exit 1
}
if (-not (Test-Path $Source)) {
    Annotate "HLSL source is missing: $Source"
    Write-Host "dxc: missing source $Source"
    exit 1
}

$outDir = Split-Path $Out -Parent
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

# -WX, and deliberately not -enable-16bit-types.
$arguments = @("-nologo", "-T", $Profile, "-E", $Entry, "-O3", "-WX", "-Fo", $Out, $Source)
Write-Host "dxc: `"$Dxc`" $($arguments -join ' ')"
# Start-Process rather than `& $Dxc 2>&1`: under a console that is not already
# redirected, PowerShell 7 refuses that call with "StandardOutputEncoding is only
# supported when standard output is redirected", which is what ninja's console is.
# Explicit redirection to files also means dxc's own words are never lost.
$stdout = [System.IO.Path]::GetTempFileName()
$stderr = [System.IO.Path]::GetTempFileName()
$quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
$process = Start-Process -FilePath $Dxc -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$code = $process.ExitCode
$output = @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue) +
          @(Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue)
Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
foreach ($line in @($output)) {
    if ($null -ne $line -and "$line".Length -gt 0) { Write-Host "dxc: $line" }
}
if ($code -ne 0) {
    $text = (@($output) -join "`n")
    if (-not $text) {
        $text = "dxc exited $code for $Entry ($Profile) from $Source and printed nothing; " +
                "check that dxc.exe, dxcompiler.dll and dxil.dll in $(Split-Path $Dxc -Parent) are all x64 and from one release"
    }
    Annotate $text
    Write-Host "dxc: FAILED ($code) $Entry ($Profile)"
    exit 1
}
if (-not (Test-Path $Out)) {
    Annotate "dxc reported success but $Out does not exist"
    exit 1
}
$size = (Get-Item $Out).Length
Write-Host "dxc: $Entry ($Profile) -> $Out ($size bytes)"
# Stage counts are useful when a PSO later fails to create: a 0-byte or 200-byte
# blob means the compile produced nothing usable.
if ($size -lt 64) { Annotate "dxc produced a suspiciously small blob ($size bytes) for $Entry" ; exit 1 }
exit 0
