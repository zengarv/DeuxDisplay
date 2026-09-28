<#
.SYNOPSIS
  Runs the host in this console and connects the Android app over USB and/or Wi-Fi.

.DESCRIPTION
  For everyday use, `scripts\install-autostart.ps1` runs the host in the background instead,
  and a tablet becomes a monitor whenever it's plugged in with the app open. This script is for
  development, Wi-Fi, and trying options:

  1. Optionally (-Install) installs the debug APK.
  2. Launches the DeuxDisplay app on the tablet.
  3. Runs DeuxDisplayHost --serve in this console (Ctrl+C to stop; the monitor unplugs itself).
     The host itself keeps `adb reverse tcp:PORT tcp:PORT` in place whenever the tablet is
     attached (re-plugged cable, USB mode switch, adb server restart).

  -Transport picks what the host serves: Both (default), Usb or Wifi. The app decides which one
  it uses (press Back on the tablet > Connection). With -Transport Wifi, adb is optional: steps
  1-3 are skipped when no device is attached, and the tablet must already be paired (connect
  once over USB, or type the code from `DeuxDisplayHost --pair`).

  No admin rights needed. The driver must already be installed (scripts/install-driver.ps1).
#>
[CmdletBinding()]
param(
    [int]$Port = 27183,
    [string]$Serial, # adb device serial; defaults to the only connected device
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Install,
    [string]$BitrateKbps = 'auto',  # or a fixed bitrate in kbit/s
    [int]$MaxFps = 60,
    [string]$MaxStreamSize, # e.g. 2560x1440: encode at most this size (fits the tablet's decoder)
    [ValidateSet('auto', 'h264', 'hevc', 'vp9')]
    [string]$Codec = 'auto',
    [ValidateSet('Both', 'Usb', 'Wifi')]
    [string]$Transport = 'Both'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$HostExe = Join-Path $RepoRoot "host\x64\$Configuration\DeuxDisplayHost.exe"
$Apk = Join-Path $RepoRoot 'android\app\build\outputs\apk\debug\app-debug.apk'
if (-not (Test-Path $Apk)) {
    # Prebuilt release: the APK downloaded next to the scripts (DeuxDisplay-<version>.apk)
    $released = Get-ChildItem $RepoRoot -Filter 'DeuxDisplay*.apk' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($released) { $Apk = $released.FullName }
}

$adbCommand = Get-Command adb -ErrorAction SilentlyContinue
$adb = if ($adbCommand) { $adbCommand.Source } else { $null }
if (-not $adb) {
    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'DeuxDisplay\tools\android-sdk' }
    $adb = Join-Path $sdk 'platform-tools\adb.exe'
}
if (-not (Test-Path $HostExe)) { throw "Host not built: $HostExe" }
$useAdb = $Transport -ne 'Wifi'
if ($useAdb -and -not (Test-Path $adb)) { throw 'adb not found. Install it (winget install Google.PlatformTools) or run scripts\bootstrap-dev.ps1.' }

$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }

if (Test-Path $adb) {
    $devices = & $adb devices | Select-String -Pattern '^\S+\s+device$'
    if (-not $Serial -and @($devices).Count -ne 1) {
        if ($useAdb) { throw "Expected exactly one authorized adb device, found $(@($devices).Count). Use -Serial." }
        $adb = $null # Wi-Fi only: the tablet needn't be plugged in
    }
} else {
    $adb = $null
}

if (Get-Process DeuxDisplayAgent -ErrorAction SilentlyContinue) {
    throw 'DeuxDisplay is already running in the background (tray icon). Exit it from the tray menu, or run scripts\install-autostart.ps1 -Remove, first.'
}

if ($adb) {
    if ($Install) {
        if (-not (Test-Path $Apk)) { throw "APK not built: $Apk (cd android; .\gradlew assembleDebug)" }
        & $adb @adbArgs install -r $Apk | Out-Host
    }
    & $adb @adbArgs shell am start -n io.github.zengarv.deuxdisplay/.MainActivity | Out-Null
    Write-Host 'Launched DeuxDisplay on the tablet.'
} else {
    Write-Host 'No adb device: open DeuxDisplay on the tablet yourself (Connection: Wi-Fi).'
}
Write-Host 'Starting host (Ctrl+C to stop)...'

$hostArgs = @('--serve', '--port', $Port, '--bitrate', $BitrateKbps, '--max-fps', $MaxFps, '--codec', $Codec,
    '--transport', $Transport.ToLowerInvariant())
if ($MaxStreamSize) { $hostArgs += @('--max-stream-size', $MaxStreamSize) }
if ($adb -and $useAdb) { $hostArgs += @('--adb', $adb) }
$hostProcess = Start-Process -FilePath $HostExe -NoNewWindow -PassThru -ArgumentList $hostArgs
try {
    $hostProcess.WaitForExit()
} finally {
    if (-not $hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id }
}
