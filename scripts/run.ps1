<#
.SYNOPSIS
  Starts the host and connects the Android app over USB and/or Wi-Fi.

.DESCRIPTION
  1. Sets up `adb reverse tcp:PORT tcp:PORT` so 127.0.0.1:PORT on the tablet reaches the PC.
  2. Optionally (-Install) installs the debug APK.
  3. Launches the DeuxDisplay app on the tablet.
  4. Runs DeuxDisplayHost --serve in this console (Ctrl+C to stop; the monitor unplugs itself),
     re-applying the adb reverse tunnel whenever it disappears (e.g. another tool restarts the
     adb server).

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
    [int]$BitrateKbps = 30000,
    [int]$MaxFps = 60,
    [string]$MaxStreamSize, # e.g. 2560x1440: encode at most this size (fits the tablet's decoder)
    [ValidateSet('auto', 'h264', 'hevc')]
    [string]$Codec = 'auto',
    [ValidateSet('Both', 'Usb', 'Wifi')]
    [string]$Transport = 'Both'
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
if (-not (Test-Path $HostExe)) { throw "Host not built: $HostExe" }
$useAdb = $Transport -ne 'Wifi'
if ($useAdb -and -not (Test-Path $adb)) { throw 'adb not found. Run scripts\bootstrap-dev.ps1 or put platform-tools on PATH.' }

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

if ($adb) {
    if ($useAdb) {
        & $adb @adbArgs reverse "tcp:$Port" "tcp:$Port" | Out-Null
        Write-Host "adb reverse tcp:$Port -> PC tcp:$Port"
    }
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
$hostProcess = Start-Process -FilePath $HostExe -NoNewWindow -PassThru -ArgumentList $hostArgs
try {
    # The tablet may be unplugged or the adb server restarted at any time: keep retrying quietly.
    $ErrorActionPreference = 'Continue'
    $wasMissing = $false
    while (-not $hostProcess.HasExited) {
        Start-Sleep -Seconds 2
        if (-not ($adb -and $useAdb)) { continue }
        $reverses = cmd /c "`"$adb`" $($adbArgs -join ' ') reverse --list 2>nul"
        if (-not ($reverses -match "tcp:$Port")) {
            cmd /c "`"$adb`" $($adbArgs -join ' ') reverse tcp:$Port tcp:$Port >nul 2>nul"
            if ($LASTEXITCODE -eq 0) {
                Write-Host 'adb reverse tunnel (re)established'
                $wasMissing = $false
            } elseif (-not $wasMissing) {
                Write-Host 'Tablet not reachable over adb; waiting for it to reconnect...'
                $wasMissing = $true
            }
        }
    }
} finally {
    if (-not $hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id }
}
