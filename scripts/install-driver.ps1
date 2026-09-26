#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Signs the locally built DeuxDisplayIdd driver package with a local certificate and installs it.

.DESCRIPTION
  DeuxDisplayIdd is a user-mode (UMDF) driver, so it doesn't need Microsoft signing or
  test-signing boot mode. It only needs a catalog signed by a certificate this machine trusts.
  This script:
    1. Creates (once) a self-signed code-signing certificate "DeuxDisplay Local Driver Signing"
       in CurrentUser\My with a non-exportable private key.
    2. Trusts it in LocalMachine\Root and LocalMachine\TrustedPublisher.
    3. Generates and signs the package catalog.
    4. Installs the package into the driver store with pnputil.

  SECURITY: anything signed with this certificate will be trusted by this machine. The private
  key never leaves CurrentUser\My, but remove it with `uninstall-driver.ps1 -RemoveCertificate`
  when you're done developing.

  The virtual monitor itself only appears while the host runs (`DeuxDisplayHost --create-display`).
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$PackageDir = Join-Path $RepoRoot "driver\build\x64\$Configuration\DeuxDisplayIdd"
$Inf = Join-Path $PackageDir 'DeuxDisplayIdd.inf'
$Cat = Join-Path $PackageDir 'deuxdisplayidd.cat'
$CertSubject = 'CN=DeuxDisplay Local Driver Signing'

if (-not (Test-Path $Inf)) {
    throw "Driver package not found at $PackageDir. Build it first: msbuild driver\DeuxDisplayIdd.sln /t:restore,build /p:RestorePackagesConfig=true /p:Configuration=$Configuration /p:Platform=x64"
}

function Find-KitTool([string]$Name) {
    $roots = @(
        (Join-Path $RepoRoot 'driver\packages'),
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem $root -Recurse -Filter $Name -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\(x64|x86)\\' } |
            Sort-Object { $_.FullName -match '\\x64\\' } -Descending |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$Name not found in the WDK NuGet packages or Windows Kits."
}

# 1. Certificate
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $CertSubject | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating certificate '$CertSubject'"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
        -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(3)
}

# 2. Trust it locally (public part only)
$cer = Join-Path ([IO.Path]::GetTempPath()) 'deuxdisplay-driver.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
foreach ($store in 'Root', 'TrustedPublisher') {
    if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cert.Thumbprint)) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Write-Host "Trusted certificate in LocalMachine\$store"
    }
}
Remove-Item $cer

# 3. Catalog + signature
$inf2cat = Find-KitTool 'Inf2Cat.exe'
$signtool = Find-KitTool 'signtool.exe'
if (Test-Path $Cat) { Remove-Item $Cat }
& $inf2cat /driver:$PackageDir /os:10_x64 /uselocaltime | Out-Host
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Cat)) { throw 'Inf2Cat failed' }
& $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My $Cat | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool failed' }

# 4. Install into the driver store
pnputil /add-driver $Inf /install | Out-Host
if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Write-Host 'Driver installed. Run `DeuxDisplayHost --create-display` to attach the virtual monitor.'
