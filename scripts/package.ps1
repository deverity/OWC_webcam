<#
.SYNOPSIS
  Collects the built OWC artifacts into dist\OWC and zips them to dist\OWC-<yyyyMMdd>.zip.

.DESCRIPTION
  Layout of the package (dist\OWC):
    OWC.exe                       desktop app
    vcam\x64\owc_virtualcam.dll   virtual camera, 64-bit
    vcam\x86\owc_virtualcam.dll   virtual camera, 32-bit (if built)
    obs-plugin\                   OBS source plugin tree (bin\64bit, data)
    OWC.apk                       Android APK (release if built, else debug)
    install-virtualcam.ps1, uninstall-virtualcam.ps1, install-obs-plugin.ps1
    README.md, THIRD_PARTY_NOTICES.txt

  Run build-all.ps1 first. Compatible with PowerShell 7 and Windows PowerShell 5.1.

.PARAMETER BuildDir
  x64 build directory (default build\release). The x86 virtual camera is read
  from the sibling directory "<BuildDir>-x86" (default build\release-x86).

.PARAMETER OutDir
  Output directory (default dist).

.EXAMPLE
  pwsh -File scripts\package.ps1
#>
[CmdletBinding()]
param(
  [string]$BuildDir = 'build\release',
  [string]$OutDir = 'dist'
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

function Resolve-Under([string]$path) {
  if ([System.IO.Path]::IsPathRooted($path)) { return [System.IO.Path]::GetFullPath($path) }
  return [System.IO.Path]::GetFullPath((Join-Path $repo $path))
}

function Fail([string]$detail) {
  Write-Host ''
  Write-Host "FAILED: $detail" -ForegroundColor Red
  exit 1
}

function Copy-Required([string]$src, [string]$dstDir, [string]$what) {
  if (-not (Test-Path $src)) { Fail "$what not found: $src (run scripts\build-all.ps1 first)" }
  New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
  Copy-Item -Path $src -Destination $dstDir -Force
}

# Finds a file in the usual CMake output locations (VS multi-config may add Release/).
function Find-Output([string]$root, [string]$relative) {
  foreach ($sub in @('out\Release', 'out')) {
    $p = Join-Path (Join-Path $root $sub) $relative
    if (Test-Path $p) { return $p }
  }
  return $null
}

$buildX64 = Resolve-Under $BuildDir
$leaf = Split-Path -Leaf $buildX64
$buildX86 = Join-Path (Split-Path -Parent $buildX64) ($leaf + '-x86')
$outRoot = Resolve-Under $OutDir
$stage = Join-Path $outRoot 'OWC'
$zip = Join-Path $outRoot ('OWC-' + (Get-Date -Format 'yyyyMMdd') + '.zip')

if (-not (Test-Path $buildX64)) { Fail "build directory not found: $buildX64 (run scripts\build-all.ps1 first)" }

Write-Host "Packaging from $buildX64" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# ---- OWC.exe ---------------------------------------------------------------
$exe = Find-Output $buildX64 'OWC.exe'
if (-not $exe) { Fail "OWC.exe not found under $buildX64\out" }
Copy-Item $exe $stage -Force

# ---- virtual camera --------------------------------------------------------
$vcam64 = Find-Output $buildX64 'owc_virtualcam.dll'
if (-not $vcam64) { Fail "owc_virtualcam.dll (x64) not found under $buildX64\out" }
Copy-Required $vcam64 (Join-Path $stage 'vcam\x64') 'owc_virtualcam.dll (x64)'

$vcam32 = if (Test-Path $buildX86) { Find-Output $buildX86 'owc_virtualcam.dll' } else { $null }
if ($vcam32) {
  Copy-Required $vcam32 (Join-Path $stage 'vcam\x86') 'owc_virtualcam.dll (x86)'
} else {
  Write-Warning "x86 virtual camera not found under $buildX86 - package has no vcam\x86 (32-bit apps will not see the camera)."
}

# ---- OBS plugin ------------------------------------------------------------
$obsSrc = Join-Path $buildX64 'out\obs-plugins\owc-obs-source'
if (-not (Test-Path (Join-Path $obsSrc 'bin\64bit\owc-obs-source.dll'))) {
  Fail "OBS plugin not found: $obsSrc\bin\64bit\owc-obs-source.dll"
}
Copy-Item $obsSrc (Join-Path $stage 'obs-plugin') -Recurse -Force

# ---- Android APK -----------------------------------------------------------
# Prefer the release APK (not debuggable; signed with the debug key), else debug.
$apk = Join-Path $repo 'android\app\build\outputs\apk\release\app-release.apk'
if (-not (Test-Path $apk)) { $apk = Join-Path $repo 'android\app\build\outputs\apk\debug\app-debug.apk' }
if (Test-Path $apk) {
  Copy-Item $apk (Join-Path $stage 'OWC.apk') -Force
} else {
  Write-Warning "APK not found: $apk - OWC.apk NOT included (run build-all.ps1 without -SkipAndroid)."
}

# ---- scripts, README -------------------------------------------------------
foreach ($s in @('install-virtualcam.ps1', 'uninstall-virtualcam.ps1', 'install-obs-plugin.ps1')) {
  Copy-Required (Join-Path $repo "scripts\$s") $stage $s
}
Copy-Required (Join-Path $repo 'README.md') $stage 'README.md'

# ---- THIRD_PARTY_NOTICES.txt -----------------------------------------------
$imguiLic = Join-Path $repo 'windows\third_party\imgui\LICENSE.txt'
$obsLic = Join-Path $repo 'windows\third_party\obs\LICENSE'
$monoLic = Join-Path $repo 'protocol\third_party\monocypher\LICENCE.md'
$notice = New-Object System.Text.StringBuilder
[void]$notice.AppendLine('OWC - third-party notices')
[void]$notice.AppendLine('=========================')
[void]$notice.AppendLine('')
[void]$notice.AppendLine('1. Dear ImGui (windows/third_party/imgui) - used by OWC.exe')
[void]$notice.AppendLine('   License: MIT. Copyright (c) 2014-2025 Omar Cornut.')
[void]$notice.AppendLine('   Full text: windows/third_party/imgui/LICENSE.txt (reproduced below).')
[void]$notice.AppendLine('')
[void]$notice.AppendLine('2. libobs headers (windows/third_party/obs) - used to build obs-plugin\bin\64bit\owc-obs-source.dll')
[void]$notice.AppendLine('   License: GPL-2.0-or-later. The OBS source plugin is therefore distributed under')
[void]$notice.AppendLine('   GPL-2.0-or-later terms. Full text: windows/third_party/obs/LICENSE (reproduced below).')
[void]$notice.AppendLine('')
[void]$notice.AppendLine('3. scrcpy, Moonlight and Sunshine - studied as REFERENCES ONLY. No code from these')
[void]$notice.AppendLine('   projects is included in this package.')
[void]$notice.AppendLine('')
[void]$notice.AppendLine('4. Monocypher 4.0.3 (protocol/third_party/monocypher) - used by OWC.exe and OWC.apk')
[void]$notice.AppendLine('   (X25519, Elligator 2, ChaCha20-Poly1305). License: BSD-2-Clause or CC0-1.0 (dual).')
[void]$notice.AppendLine('   Copyright (c) 2017-2023 Loup Vaillant, Michael Savage, Fabio Scotoni. Full text reproduced below.')
[void]$notice.AppendLine('')
[void]$notice.AppendLine('--- Dear ImGui LICENSE.txt ---')
if (Test-Path $imguiLic) { [void]$notice.AppendLine((Get-Content -Raw $imguiLic)) } else { [void]$notice.AppendLine('(file missing)') }
[void]$notice.AppendLine('')
[void]$notice.AppendLine('--- Monocypher LICENCE.md ---')
if (Test-Path $monoLic) { [void]$notice.AppendLine((Get-Content -Raw $monoLic)) } else { [void]$notice.AppendLine('(file missing)') }
[void]$notice.AppendLine('')
[void]$notice.AppendLine('--- libobs LICENSE (GPL-2.0) ---')
if (Test-Path $obsLic) { [void]$notice.AppendLine((Get-Content -Raw $obsLic)) } else { [void]$notice.AppendLine('(file missing)') }
[System.IO.File]::WriteAllText((Join-Path $stage 'THIRD_PARTY_NOTICES.txt'), $notice.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# ---- zip -------------------------------------------------------------------
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

Write-Host ''
Write-Host 'Package contents:' -ForegroundColor Cyan
Get-ChildItem -Recurse -File $stage | ForEach-Object {
  $rel = $_.FullName.Substring($stage.Length + 1)
  Write-Host ('  {0,-60} {1,10:N0} bytes' -f $rel, $_.Length)
}
$zi = Get-Item $zip
Write-Host ''
Write-Host ('Zip: {0} ({1} MB)' -f $zi.FullName, [math]::Round($zi.Length / 1MB, 2)) -ForegroundColor Green
exit 0
