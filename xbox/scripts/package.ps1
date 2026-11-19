# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Lays out the package, audits its imports and packs the .msix/.appx.

.DESCRIPTION
  The package contains exactly: bbxbox.exe, its .map and .pdb next to it for
  symbolication, the dependency DLLs (SDL3.dll, libuwp.dll), the manifest and the
  assets - and no shader compiler at all: every DXIL blob is built with dxc on
  the build machine (xbox/shaders/*.hlsl). No game data, ever; the dump is read
  from wherever the player granted access.
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

# The symbols go *beside* the .appx in `build\appx`, never inside it. The
# console never reads a .pdb or a .a map file - `LastChance` prints
# `module+0xRVA` from the module table alone - so a symbol file in the package
# is dead weight that inflates every sideload upload to Xbox Device Portal (the
# guest runtime links with /DEBUG, and the .pdb is ~100 MB). Beside the .appx,
# in the artifact a reviewer downloads, it is what turns a stack from the
# player's USB stick (`bbxbox_logs\crash.log`) into names with
# `xbox/scripts/symbolicate.py`.
foreach ($name in @("bbxbox.map", "bbxbox.pdb", "SDL3.pdb")) {
    $found = Get-ChildItem -Recurse -Path $build -Filter $name | Select-Object -First 1
    if ($found) {
        Copy-Item $found.FullName (Join-Path $out $name) -Force
    } elseif ($name -ne "SDL3.pdb") {
        Write-Warning "no $name under build: a crash report from the console cannot be symbolised"
    }
}

foreach ($dll in @("sdl3\bin\SDL3.dll", "libuwp\bin\libuwp.dll")) {
    $path = Join-Path $prefix $dll
    if (Test-Path $path) { Copy-Item $path (Join-Path $layout (Split-Path $path -Leaf)) }
}
# Fixed passes (xbox/shaders/*.hlsl) are still compiled at build time into
# bbx_shaders_generated.h. For Bloodborne's guest shaders, ship the pinned x64
# dxil.dll + dxcompiler.dll so the console can bake bbx_shaders.cache natively
# on first launch (via LoadPackagedLibrary) and unload both DLLs (FreeLibrary)
# before allocating guest RAM (0 MiB compiler overhead during gameplay).
foreach ($dxcDll in @("dxc\dxil.dll", "dxc\dxcompiler.dll")) {
    $dxcPath = Join-Path $prefix $dxcDll
    if (-not (Test-Path $dxcPath)) {
        throw "missing required on-console shader compiler payload: $dxcPath (run xbox/scripts/build-deps.ps1)"
    }
    Copy-Item $dxcPath (Join-Path $layout (Split-Path $dxcPath -Leaf))
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

# Lint the manifest before makeappx, which reports one schema violation at a time.
& (Join-Path $PSScriptRoot "check-manifest.ps1") -Root $Root
if ($LASTEXITCODE -ne 0) { throw "manifest lint failed; not packaging" }

$appx = Join-Path $out "bbxbox_0.1.0.0_x64.appx"
# makeappx and signtool ship with the Windows SDK, side by side.
$makeappx = Get-Command makeappx.exe -ErrorAction SilentlyContinue
$sdkBin = ""
if ($makeappx) { $sdkBin = Split-Path $makeappx.Source -Parent }
else {
    $sdk = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $found = Get-ChildItem -Recurse -Path $sdk -Filter makeappx.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } | Select-Object -First 1
    if ($found) { $makeappx = $found; $sdkBin = $found.DirectoryName }
}
if (-not $makeappx) { throw "makeappx.exe not found (Windows SDK)" }
$makeOut = & $makeappx.FullName pack /d $layout /p $appx /o 2>&1
if ($LASTEXITCODE -ne 0) {
    # makeappx's own words are the only description of why a package was
    # refused; pass them to the annotation.
    throw ("makeappx failed: " + (($makeOut | Out-String).Trim()))
}

# --- the framework package the manifest depends on --------------------------
# Xbox Device Portal refuses an upload that does not carry the Store CRT
# framework package, and the manifest names it. Dependencies/ next to the .appx
# is the layout the portal expects.
$dependencies = Join-Path $out "Dependencies"
New-Item -ItemType Directory -Force -Path $dependencies | Out-Null
$vclibsDir = "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64"
$vclibs = Get-ChildItem -Path $vclibsDir -Filter "Microsoft.VCLibs.x64.14.00.appx" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $vclibs) {
    throw ("Microsoft.VCLibs.x64.14.00.appx not found in $vclibsDir; the manifest depends on it, so the package cannot be sideloaded without it")
}
Copy-Item $vclibs.FullName (Join-Path $dependencies $vclibs.Name) -Force

# --- signing ----------------------------------------------------------------
# A console will not install an unsigned package. With -Certificate the caller
# supplies a .pfx; without it an ephemeral self-signed certificate is created for
# this build, with the subject the manifest's Publisher states (CN=bbxbox), the
# package is signed SHA-256, and the certificate is exported as bbxbox.cer so a
# tester can install it on the console once.
$signtool = if ($sdkBin) { Join-Path $sdkBin "signtool.exe" } else { "" }
if (-not $signtool -or -not (Test-Path $signtool)) {
    $found = Get-ChildItem -Recurse -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } | Select-Object -First 1
    if ($found) { $signtool = $found.FullName }
}
if (-not $signtool -or -not (Test-Path $signtool)) { throw "signtool.exe not found (Windows SDK)" }

$thumbprint = ""
$cerPath = Join-Path $out "bbxbox.cer"
if ($Certificate) {
    if (-not (Test-Path $Certificate)) { throw "certificate not found: $Certificate" }
    if ($Password) {
        $secure = ConvertTo-SecureString -String $Password -AsPlainText -Force
        $imported = Import-PfxCertificate -FilePath $Certificate -CertStoreLocation Cert:\CurrentUser\My -Password $secure
    } else {
        $imported = Import-PfxCertificate -FilePath $Certificate -CertStoreLocation Cert:\CurrentUser\My
    }
    $thumbprint = $imported.Thumbprint
} else {
    # Reuse a certificate from an earlier build in this session rather than
    # collecting one per push; the console only needs the first one installed.
    $existing = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq "CN=bbxbox" -and $_.HasPrivateKey } | Select-Object -First 1
    if ($existing) {
        $thumbprint = $existing.Thumbprint
    } else {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=bbxbox" `
            -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(1) `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
        $thumbprint = $cert.Thumbprint
    }
    Export-Certificate -Cert "Cert:\CurrentUser\My\$thumbprint" -FilePath $cerPath -Type CERT -Force | Out-Null
    Write-Host "package: self-signed certificate CN=bbxbox, thumbprint $thumbprint"
    Write-Host "package: install bbxbox.cer on the console once (certutil -addstore TrustedPeople bbxbox.cer)"
}

$signOut = & $signtool sign /fd SHA256 /sha1 $thumbprint $appx 2>&1
if ($LASTEXITCODE -ne 0) {
    throw ("signtool sign failed: " + (($signOut | Out-String).Trim()))
}
$verifyOut = & $signtool verify /pa $appx 2>&1
if ($LASTEXITCODE -ne 0) {
    # A self-signed chain is untrusted on the runner; the console trusts it after
    # bbxbox.cer is installed. This is information, not a failure.
    Write-Warning ("signtool verify /pa: " + (($verifyOut | Out-String).Trim()))
}

# The sizes, as a notice: the report a reviewer reads says what the package
# costs, and the guest runtime's link is most of it. An annotation because a
# run's log is not always reachable afterwards.
$exeSize = (Get-Item $exe.FullName).Length
$pdbItem = Get-Item (Join-Path $out "bbxbox.pdb") -ErrorAction SilentlyContinue
$pdbSize = if ($pdbItem) { $pdbItem.Length } else { 0 }
$appxSize = (Get-Item $appx).Length
if ($env:GITHUB_ACTIONS -eq 'true') {
    $appxName = Split-Path $appx -Leaf
    Write-Output ("::notice title=sizes::bbxbox.exe {0:N0} B; bbxbox.pdb {1:N0} B; {2} {3:N0} B" -f
        $exeSize, $pdbSize, $appxName, $appxSize)
}
Write-Host "package: $appx"
Write-Host "package: $dependencies\$($vclibs.Name)"
if (Test-Path $cerPath) { Write-Host "package: $cerPath" }
Write-Host "install: Add-AppxPackage -Path `"$appx`" -DependencyPath `"$dependencies\$($vclibs.Name)`" (Developer Mode, certificate installed)"
}
catch {
    # Anything thrown above (including a check-*.ps1 helper) reaches the
    # annotation, because a bare exit code says nothing after the run.
    Fail "$_"
}

# The step's exit code is the script's last native exit code unless it is set
# explicitly, and `signtool verify /pa` on a self-signed package is expected to
# fail: without this, a successful package reported failure.
exit 0
