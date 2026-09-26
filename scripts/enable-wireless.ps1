#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Lets tablets reach DeuxDisplayHost over Wi-Fi through Windows Firewall.

.DESCRIPTION
  Over Wi-Fi the tablet connects to the host on the PC's own Wi-Fi Direct network, which Windows
  treats as a public network. Without an inbound rule, Windows asks the first time the host
  listens there ("allow on public networks?"); this script creates the rule up front instead.

  The rule only allows DeuxDisplayHost.exe (Release and Debug builds of this checkout), TCP,
  inbound. The host itself only binds to 127.0.0.1 and to its own access point's address, and
  Wi-Fi clients must pass the pairing-code handshake (docs/wire-protocol.md).

  Idempotent: re-running replaces the rules. -Remove deletes them.
#>
[CmdletBinding()]
param(
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$Group = 'DeuxDisplay'

Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if ($Remove) {
    Write-Host 'Removed the DeuxDisplay firewall rules.'
    return
}

foreach ($configuration in 'Release', 'Debug') {
    $exe = Join-Path $RepoRoot "host\x64\$configuration\DeuxDisplayHost.exe"
    New-NetFirewallRule -DisplayName "DeuxDisplay host ($configuration) - Wi-Fi" -Group $Group `
        -Direction Inbound -Action Allow -Protocol TCP -Program $exe -Profile Any | Out-Null
    Write-Host "Allowed inbound TCP for $exe"
}
