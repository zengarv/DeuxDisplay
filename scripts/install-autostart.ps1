<#
.SYNOPSIS
  Starts DeuxDisplay in the background at every login, so a tablet becomes a monitor as soon as
  it's plugged in with the app open.

.DESCRIPTION
  Registers DeuxDisplayAgent.exe (a tray app that keeps DeuxDisplayHost running over USB) in the
  current user's Run key and starts it now. No admin rights needed. The driver must already be
  installed (scripts/install-driver.ps1). Idempotent: re-running updates the entry.

  -HostArgs passes extra options to the host, e.g. -HostArgs '--codec hevc --max-fps 60'.
  -Remove unregisters it and stops the agent (the monitor goes away).

  Wi-Fi isn't served in the background. Use `scripts\run.ps1 -Transport Wifi` for that, after
  `-Remove` or from the tray menu's Exit, since only one host can run at a time.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$HostArgs = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$Agent = Join-Path $RepoRoot "host\x64\$Configuration\DeuxDisplayAgent.exe"
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Name = 'DeuxDisplay'

function Stop-DeuxDisplay {
    Get-Process DeuxDisplayAgent -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process DeuxDisplayHost -ErrorAction SilentlyContinue | Stop-Process -Force
}

if ($Remove) {
    Remove-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue
    Stop-DeuxDisplay
    Write-Host 'DeuxDisplay no longer starts at login, and the agent is stopped.'
    return
}

if (-not (Test-Path $Agent)) {
    throw "Agent not built: $Agent (msbuild DeuxDisplay.sln /p:Configuration=$Configuration /p:Platform=x64)"
}

$command = "`"$Agent`""
if ($HostArgs) { $command += " $HostArgs" }
Set-ItemProperty -Path $RunKey -Name $Name -Value $command
Write-Host "Starts at login: $command"

# Restart so new binaries/options take effect; a host started by run.ps1 would hold the port.
Stop-DeuxDisplay
Start-Sleep -Milliseconds 500
if ($HostArgs) {
    Start-Process -FilePath $Agent -ArgumentList $HostArgs
} else {
    Start-Process -FilePath $Agent
}
Write-Host 'Agent running (tray icon). Open DeuxDisplay on the tablet and plug it in.'
Write-Host "Log: $env:LOCALAPPDATA\DeuxDisplay\host.log"
