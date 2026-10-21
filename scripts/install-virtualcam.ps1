<#
.SYNOPSIS
  Installs and registers the "OWC Webcam" DirectShow virtual camera.

.DESCRIPTION
  Copies owc_virtualcam.dll (x64, and optionally x86) to an install directory
  and registers it with regsvr32:
    -PerUser : HKCU\Software\Classes (regsvr32 /n /i:user), no admin rights.
               Visible to apps running as the current user, NOT to elevated
               (admin) processes - COM ignores per-user classes there.
    default  : HKLM\Software\Classes (regsvr32), machine-wide. Self-elevates
               (UAC prompt) when not already running as administrator.
  Copying (instead of registering the build output) keeps rebuilds possible
  while apps such as Zoom/Teams hold the DLL loaded. A DLL that is in use is
  renamed aside (allowed for loaded images) before the new one is copied.

.EXAMPLE
  .\scripts\install-virtualcam.ps1 -PerUser
.EXAMPLE
  .\scripts\install-virtualcam.ps1 -Dll32 build\virtualcam-x86\out\Release\owc_virtualcam.dll
#>
[CmdletBinding()]
param(
  [switch]$PerUser,
  [string]$Dll64,
  [string]$Dll32,
  [string]$InstallDir,
  [switch]$NoCopy
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $PerUser -and -not (Test-Admin)) {
  Write-Host 'Machine-wide install needs administrator rights; relaunching elevated...'
  $fwd = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
  foreach ($k in $PSBoundParameters.Keys) {
    $v = $PSBoundParameters[$k]
    if ($v -is [switch]) { if ($v) { $fwd += "-$k" } }
    else {
      $rp = Resolve-Path $v -ErrorAction SilentlyContinue
      $fwd += "-$k"; $fwd += "`"$(if ($rp) { $rp.Path } else { $v })`""
    }
  }
  $p = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $fwd -Wait -PassThru
  exit $p.ExitCode
}

# Candidates: package layout next to this script (dist\OWC\vcam\<arch>), then repo build trees.
function Find-Dll([string]$given, [string[]]$candidates) {
  if ($given) { return (Resolve-Path $given).Path }
  foreach ($c in $candidates) {
    foreach ($base in @($PSScriptRoot, $repo)) {
      $p = Join-Path $base $c
      if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
  }
  return $null
}

$src64 = Find-Dll $Dll64 @('vcam\x64\owc_virtualcam.dll', 'build\release\out\Release\owc_virtualcam.dll',
                           'build\virtualcam\out\Release\owc_virtualcam.dll', 'build\win\out\Release\owc_virtualcam.dll')
$src32 = Find-Dll $Dll32 @('vcam\x86\owc_virtualcam.dll', 'build\release-x86\out\Release\owc_virtualcam.dll',
                           'build\virtualcam-x86\out\Release\owc_virtualcam.dll')
if (-not $src64 -and -not $src32) { throw 'owc_virtualcam.dll not found; build it or pass -Dll64/-Dll32.' }

if (-not $InstallDir) {
  $InstallDir = if ($PerUser) { Join-Path $env:LOCALAPPDATA 'OWC\VirtualCam' } else { Join-Path $env:ProgramFiles 'OWC\VirtualCam' }
}

function Install-One([string]$src, [string]$arch, [string]$regsvr) {
  if (-not $src) { return }
  $dest = $src
  if (-not $NoCopy) {
    $dir = Join-Path $InstallDir $arch
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dest = Join-Path $dir 'owc_virtualcam.dll'
    if (Test-Path $dest) {
      try { Copy-Item -Force $src $dest }
      catch {
        # in use by some app: move the loaded image aside, then copy
        $aside = "$dest.old-$(Get-Date -Format yyyyMMddHHmmss)"
        Move-Item -Force $dest $aside
        Copy-Item -Force $src $dest
        Write-Host "  previous DLL was in use; moved to $aside (delete it after apps restart)"
      }
    } else {
      Copy-Item -Force $src $dest
    }
  }
  $rsArgs = if ($PerUser) { @('/s', '/n', '/i:user', "`"$dest`"") } else { @('/s', "`"$dest`"") }
  $p = Start-Process -FilePath $regsvr -ArgumentList $rsArgs -Wait -PassThru -NoNewWindow
  if ($p.ExitCode -ne 0) { throw "regsvr32 ($arch) failed with exit code $($p.ExitCode) for $dest" }
  Write-Host "registered $arch : $dest ($(if ($PerUser) { 'per-user, HKCU' } else { 'machine-wide, HKLM' }))"
}

Install-One $src64 'x64' (Join-Path $env:SystemRoot 'System32\regsvr32.exe')
if ($src32) {
  $wow = Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe'
  if (-not (Test-Path $wow)) { $wow = Join-Path $env:SystemRoot 'System32\regsvr32.exe' }  # 32-bit Windows
  Install-One $src32 'x86' $wow
}

$clsid = '{8433CA51-5EEA-4F62-A174-13FA1F6BADD5}'
$hive = if ($PerUser) { 'HKCU:' } else { 'HKLM:' }
$inst = "$hive\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\$clsid"
if (Test-Path $inst) { Write-Host "OK: $inst" } else { Write-Warning "device instance key missing: $inst" }
