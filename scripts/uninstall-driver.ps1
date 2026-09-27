#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Removes the DeuxDisplayIdd driver package (and optionally the local signing certificate).
#>
[CmdletBinding()]
param(
    [switch]$RemoveCertificate
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CertSubject = 'CN=DeuxDisplay Local Driver Signing'
$RepoRoot = Split-Path $PSScriptRoot -Parent

$hostExe = Get-ChildItem (Join-Path $RepoRoot 'host\x64') -Recurse -Filter DeuxDisplayHost.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($hostExe) {
    & $hostExe.FullName --remove-device | Out-Host
} else {
    Write-Warning 'DeuxDisplayHost.exe not found; the software device (if any) stays until removed in Device Manager.'
}

# pnputil /enum-drivers prints "Published Name" / "Original Name" pairs; match ours.
$published = $null
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match '^\s*Published Name\s*:\s*(\S+)') { $published = $Matches[1] }
    elseif ($line -match '^\s*Original Name\s*:\s*deuxdisplayidd\.inf' -and $published) {
        Write-Host "Removing $published"
        pnputil /delete-driver $published /uninstall /force | Out-Host
        $published = $null
    }
}

if ($RemoveCertificate) {
    # New-SelfSignedCertificate also leaves a public copy in CurrentUser\CA.
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\My', 'Cert:\CurrentUser\CA') {
        Get-ChildItem $store | Where-Object Subject -eq $CertSubject | ForEach-Object {
            Write-Host "Removing certificate $($_.Thumbprint) from $store"
            Remove-Item $_.PSPath
        }
    }
}
