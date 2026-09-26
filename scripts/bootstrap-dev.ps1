<#
.SYNOPSIS
  Downloads the user-local toolchain needed to build DeuxDisplay (no admin rights needed).

.DESCRIPTION
  Installs into $env:DEUXDISPLAY_TOOLS (default: %LOCALAPPDATA%\DeuxDisplay\tools):
    - Microsoft Build of OpenJDK 17 (for Gradle / Android Gradle Plugin)
    - Android SDK command-line tools, platform-tools (adb), platform + build-tools
  Then prints the environment variables to set. Re-running is safe; completed steps are skipped.

  Windows-side C++ builds need Visual Studio 2022 (or Build Tools) with the
  "Desktop development with C++" workload. The driver's WDK comes from NuGet at build time.
#>
[CmdletBinding()]
param(
    [string]$ToolsDir = $(if ($env:DEUXDISPLAY_TOOLS) { $env:DEUXDISPLAY_TOOLS } else { Join-Path $env:LOCALAPPDATA 'DeuxDisplay\tools' }),
    [string]$AndroidPlatform = 'android-35',
    [string]$BuildTools = '34.0.0', # AGP 8.7 default

    [switch]$PersistEnv
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$JdkUrl = 'https://aka.ms/download-jdk/microsoft-jdk-17-windows-x64.zip'
$CmdlineToolsUrl = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'

function Get-Zip([string]$Url, [string]$Dest) {
    $zip = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName() + '.zip')
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    $tmp = "$Dest.extract"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $tmp
    Remove-Item $zip
    return $tmp
}

New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null

$JdkHome = Join-Path $ToolsDir 'jdk17'
if (-not (Test-Path (Join-Path $JdkHome 'bin\java.exe'))) {
    $tmp = Get-Zip $JdkUrl $JdkHome
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    Move-Item $inner.FullName $JdkHome
    Remove-Item $tmp -Recurse -Force
}

$SdkRoot = Join-Path $ToolsDir 'android-sdk'
$CmdlineLatest = Join-Path $SdkRoot 'cmdline-tools\latest'
if (-not (Test-Path (Join-Path $CmdlineLatest 'bin\sdkmanager.bat'))) {
    $tmp = Get-Zip $CmdlineToolsUrl $CmdlineLatest
    New-Item -ItemType Directory -Force -Path (Split-Path $CmdlineLatest) | Out-Null
    Move-Item (Join-Path $tmp 'cmdline-tools') $CmdlineLatest
    Remove-Item $tmp -Recurse -Force
}

$env:JAVA_HOME = $JdkHome
$env:ANDROID_HOME = $SdkRoot
$sdkmanager = Join-Path $CmdlineLatest 'bin\sdkmanager.bat'

Write-Host 'Accepting Android SDK licenses (running this script implies you accept them)'
# Feeding sdkmanager.bat from PowerShell/.NET pipes doesn't work reliably, but Git Bash's `yes` does.
# Only Git for Windows' bash: System32/WindowsApps bash.exe are WSL, which can't run .bat files.
$bash = $null
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) {
    $candidate = Join-Path (Split-Path (Split-Path $git.Source)) 'bin\bash.exe'
    if (Test-Path $candidate) { $bash = $candidate }
}
if ($bash) {
    $toUnix = { param($p) ($p -replace '\\', '/') }
    & $bash -c "yes | '$(& $toUnix $sdkmanager)' --sdk_root='$(& $toUnix $SdkRoot)' --licenses" | Out-Null
} else {
    Write-Host 'Git Bash not found; answer the license prompts below.'
    & $sdkmanager "--sdk_root=$SdkRoot" --licenses
}

Write-Host 'Installing Android SDK packages'
& $sdkmanager "--sdk_root=$SdkRoot" 'platform-tools' "platforms;$AndroidPlatform" "build-tools;$BuildTools" | Out-Null

$PlatformTools = Join-Path $SdkRoot 'platform-tools'
foreach ($required in @(
        (Join-Path $PlatformTools 'adb.exe'),
        (Join-Path $SdkRoot "platforms\$AndroidPlatform\android.jar"),
        (Join-Path $SdkRoot "build-tools\$BuildTools"))) {
    if (-not (Test-Path $required)) { throw "sdkmanager did not install $required" }
}
Write-Host ''
Write-Host 'Toolchain ready. For this shell:'
Write-Host "  `$env:JAVA_HOME = '$JdkHome'"
Write-Host "  `$env:ANDROID_HOME = '$SdkRoot'"
Write-Host "  `$env:Path = '$PlatformTools;' + `$env:Path"

if ($PersistEnv) {
    [Environment]::SetEnvironmentVariable('JAVA_HOME', $JdkHome, 'User')
    [Environment]::SetEnvironmentVariable('ANDROID_HOME', $SdkRoot, 'User')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($userPath -notlike "*$PlatformTools*") {
        [Environment]::SetEnvironmentVariable('Path', "$PlatformTools;$userPath", 'User')
    }
    Write-Host 'Persisted JAVA_HOME, ANDROID_HOME and platform-tools on PATH for the current user (new shells).'
}
