<#
.SYNOPSIS
  Connects the Android app to the host over USB and starts streaming.

.DESCRIPTION
  1. Sets up `adb reverse tcp:PORT tcp:PORT` so 127.0.0.1:PORT on the tablet reaches the PC.
  2. Optionally (-Install) installs the debug APK.
  3. Launches the DeuxDisplay app on the tablet.
  4. Runs DeuxDisplayHost --serve in this console (Ctrl+C to stop; the monitor unplugs itself).

  No admin rights needed. The driver must already be installed (scripts/install-driver.ps1).
#>
[CmdletBinding()]
param(
    [int]$Port = 27183,
    [string]$Serial, # adb device serial; defaults to the only connected device
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Install,
    [int]$BitrateKbps = 30000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$HostExe = Join-Path $RepoRoot "host\x64\$Configuration\DeuxDisplayHost.exe"
$Apk = Join-Path $RepoRoot 'android\app\build\outputs\apk\debug\app-debug.apk'

$adbCommand = Get-Command adb -ErrorAction SilentlyContinue
$adb = if ($adbCommand) { $adbCommand.Source } else { $null }
if (-not $adb) {
    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'DeuxDisplay\tools\android-sdk' }
    $adb = Join-Path $sdk 'platform-tools\adb.exe'
}
if (-not (Test-Path $adb)) { throw 'adb not found. Run scripts\bootstrap-dev.ps1 or put platform-tools on PATH.' }
if (-not (Test-Path $HostExe)) { throw "Host not built: $HostExe" }

$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }

$devices = & $adb devices | Select-String -Pattern '^\S+\s+device$'
if (-not $Serial -and @($devices).Count -ne 1) {
    throw "Expected exactly one authorized adb device, found $(@($devices).Count). Use -Serial."
}

& $adb @adbArgs reverse "tcp:$Port" "tcp:$Port" | Out-Null
Write-Host "adb reverse tcp:$Port -> PC tcp:$Port"

if ($Install) {
    if (-not (Test-Path $Apk)) { throw "APK not built: $Apk (cd android; .\gradlew assembleDebug)" }
    & $adb @adbArgs install -r $Apk | Out-Host
}

& $adb @adbArgs shell am start -n io.github.zengarv.deuxdisplay/.MainActivity | Out-Null
Write-Host 'Launched DeuxDisplay on the tablet. Starting host (Ctrl+C to stop)...'

& $HostExe --serve --port $Port --bitrate $BitrateKbps
