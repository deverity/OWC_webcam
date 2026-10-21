<#
.SYNOPSIS
  Builds every OWC component that can be built on this machine.

.DESCRIPTION
  1. Windows x64 (windows/, VS2019 Build Tools, MSVC 14.29) into build/release:
     OWC.exe, owc_virtualcam.dll, owc-obs-source plugin, tests (ctest).
  2. Windows x86 virtual camera only (owc_virtualcam.dll 32-bit) into build/release-x86.
  3. Android debug APK (android/gradlew.bat assembleDebug) unless -SkipAndroid.

  Compatible with PowerShell 7 and Windows PowerShell 5.1. Stops at the first
  failing step with a message that names it.

.PARAMETER Config
  Release (default) or Debug.

.PARAMETER SkipAndroid
  Do not run Gradle.

.PARAMETER SkipTests
  Do not run ctest.

.EXAMPLE
  pwsh -File scripts\build-all.ps1
.EXAMPLE
  pwsh -File scripts\build-all.ps1 -SkipAndroid -SkipTests
#>
[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug')]
  [string]$Config = 'Release',
  [switch]$SkipAndroid,
  [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$winDir = Join-Path $repo 'windows'
$buildX64 = Join-Path $repo 'build\release'
$buildX86 = Join-Path $repo 'build\release-x86'
$androidDir = Join-Path $repo 'android'
$jdkDir = 'C:\Users\gustavo_assis\.jdks\jbr-17.0.14'
$vsCmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

function Write-Step([string]$text) {
  Write-Host ''
  Write-Host "==> $text" -ForegroundColor Cyan
}

function Fail([string]$step, [string]$detail) {
  Write-Host ''
  Write-Host "FAILED: $step" -ForegroundColor Red
  if ($detail) { Write-Host $detail -ForegroundColor Red }
  exit 1
}

# Runs a native command and fails the build when it returns non-zero.
function Invoke-Native([string]$step, [string]$exe, [string[]]$arguments) {
  & $exe @arguments
  if ($LASTEXITCODE -ne 0) { Fail $step "'$exe' exited with code $LASTEXITCODE" }
}

# ---- locate tools ----------------------------------------------------------
Write-Step 'Locating tools'
if (Test-Path $vsCmake) {
  $cmake = $vsCmake
} else {
  $found = Get-Command cmake -ErrorAction SilentlyContinue
  if (-not $found) {
    Fail 'cmake' "cmake.exe not found at the VS2019 BuildTools path and not on PATH.`nInstall Visual Studio 2019 Build Tools (CMake component) or add cmake to PATH."
  }
  $cmake = $found.Source
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path $ctest)) { $ctest = 'ctest' }
Write-Host "cmake: $cmake"

if (-not (Test-Path (Join-Path $winDir 'CMakeLists.txt'))) { Fail 'locate sources' "Missing $winDir\CMakeLists.txt" }

# ---- Windows x64 -----------------------------------------------------------
Write-Step "Configure Windows x64 -> $buildX64"
Invoke-Native 'configure x64' $cmake @('-S', $winDir, '-B', $buildX64, '-G', 'Visual Studio 16 2019', '-A', 'x64')

Write-Step "Build Windows x64 ($Config)"
Invoke-Native 'build x64' $cmake @('--build', $buildX64, '--config', $Config, '--parallel')

if ($SkipTests) {
  Write-Host 'Tests skipped (-SkipTests).'
} else {
  Write-Step "Run tests (ctest, $Config)"
  Push-Location $buildX64
  try {
    & $ctest -C $Config --output-on-failure
    $ctestCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($ctestCode -ne 0) { Fail 'ctest' "ctest exited with code $ctestCode" }
}

# ---- Windows x86 virtual camera --------------------------------------------
Write-Step "Configure + build Windows x86 virtual camera -> $buildX86"
Invoke-Native 'configure x86' $cmake @('-S', $winDir, '-B', $buildX86, '-G', 'Visual Studio 16 2019', '-A', 'Win32',
  '-DOWC_BUILD_TESTS=OFF', '-DOWC_BUILD_CORE=OFF', '-DOWC_BUILD_APP=OFF',
  '-DOWC_BUILD_OBS_PLUGIN=OFF', '-DOWC_BUILD_TOOLS=OFF')
Invoke-Native 'build x86' $cmake @('--build', $buildX86, '--config', $Config, '--parallel', '--target', 'owc_virtualcam')

# ---- Android ---------------------------------------------------------------
$apk = Join-Path $androidDir 'app\build\outputs\apk\debug\app-debug.apk'
if ($SkipAndroid) {
  Write-Step 'Android skipped (-SkipAndroid)'
} else {
  Write-Step 'Build Android debug APK (gradlew assembleDebug)'
  $gradlew = Join-Path $androidDir 'gradlew.bat'
  if (-not (Test-Path $gradlew)) { Fail 'gradle' "Missing $gradlew" }
  if (Test-Path $jdkDir) {
    $env:JAVA_HOME = $jdkDir
    Write-Host "JAVA_HOME = $jdkDir"
  } else {
    Write-Host "JDK not found at $jdkDir; using the JAVA_HOME already set in the environment." -ForegroundColor Yellow
  }
  Push-Location $androidDir
  try {
    & $gradlew assembleDebug
    $gradleCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($gradleCode -ne 0) { Fail 'gradle assembleDebug' "gradlew exited with code $gradleCode" }
}

# ---- summary ---------------------------------------------------------------
function Show-Artifact([string]$label, [string]$path) {
  if (Test-Path $path) {
    $f = Get-Item $path
    $mb = [math]::Round($f.Length / 1MB, 2)
    Write-Host ('  OK       {0,-22} {1}  ({2} MB)' -f $label, $path, $mb)
  } else {
    Write-Host ('  MISSING  {0,-22} {1}' -f $label, $path) -ForegroundColor Yellow
  }
}

Write-Step 'Summary'
$out64 = Join-Path $buildX64 'out'
$exe = Join-Path $out64 'Release\OWC.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $out64 'OWC.exe' }
Show-Artifact 'OWC.exe' $exe
$vcam64 = Get-ChildItem -Path $out64 -Recurse -Filter 'owc_virtualcam.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vcam64) { Show-Artifact 'vcam x64' $vcam64.FullName } else { Show-Artifact 'vcam x64' (Join-Path $out64 'owc_virtualcam.dll') }
Show-Artifact 'OBS plugin' (Join-Path $out64 'obs-plugins\owc-obs-source\bin\64bit\owc-obs-source.dll')
$vcam32 = Get-ChildItem -Path (Join-Path $buildX86 'out') -Recurse -Filter 'owc_virtualcam.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vcam32) { Show-Artifact 'vcam x86' $vcam32.FullName } else { Show-Artifact 'vcam x86' (Join-Path $buildX86 'out\owc_virtualcam.dll') }
if ($SkipAndroid) { Write-Host '  SKIPPED  Android APK' } else { Show-Artifact 'Android APK' $apk }
if ($SkipTests) { Write-Host '  SKIPPED  tests' } else { Write-Host '  OK       ctest passed' }
Write-Host ''
Write-Host "Build finished ($Config). Next: pwsh -File scripts\package.ps1" -ForegroundColor Green
exit 0
