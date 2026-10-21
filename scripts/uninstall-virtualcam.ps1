<#
.SYNOPSIS
  Unregisters (and removes) the "OWC Webcam" DirectShow virtual camera.

.DESCRIPTION
  -PerUser : removes the HKCU registration (no admin rights needed).
  default  : removes the HKLM registration; self-elevates when needed.
  Runs regsvr32 /u on the installed DLLs (x64 and x86 views) and then deletes
  any keys left behind directly, so it also cleans up after a DLL that was
  deleted or moved. Installed files are removed unless -KeepFiles; files still
  loaded by running apps are reported and left in place.
#>
[CmdletBinding()]
param(
  [switch]$PerUser,
  [string]$InstallDir,
  [switch]$KeepFiles
)
$ErrorActionPreference = 'Stop'

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $PerUser -and -not (Test-Admin)) {
  Write-Host 'Machine-wide uninstall needs administrator rights; relaunching elevated...'
  $fwd = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
  foreach ($k in $PSBoundParameters.Keys) {
    $v = $PSBoundParameters[$k]
    if ($v -is [switch]) { if ($v) { $fwd += "-$k" } } else { $fwd += "-$k"; $fwd += "`"$v`"" }
  }
  $p = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $fwd -Wait -PassThru
  exit $p.ExitCode
}

if (-not $InstallDir) {
  $InstallDir = if ($PerUser) { Join-Path $env:LOCALAPPDATA 'OWC\VirtualCam' } else { Join-Path $env:ProgramFiles 'OWC\VirtualCam' }
}
$clsid = '{8433CA51-5EEA-4F62-A174-13FA1F6BADD5}'
$cat = '{860BB310-5D01-11d0-BD3B-00A0C911CE86}'

function Unregister-View([Microsoft.Win32.RegistryView]$view, [string]$regsvr) {
  $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    $(if ($PerUser) { [Microsoft.Win32.RegistryHive]::CurrentUser } else { [Microsoft.Win32.RegistryHive]::LocalMachine }), $view)
  $inproc = $base.OpenSubKey("Software\Classes\CLSID\$clsid\InprocServer32")
  $dll = if ($inproc) { $inproc.GetValue('') } else { $null }
  if ($inproc) { $inproc.Close() }
  if ($dll -and (Test-Path $dll) -and (Test-Path $regsvr)) {
    $rsArgs = if ($PerUser) { @('/u', '/s', '/n', '/i:user', "`"$dll`"") } else { @('/u', '/s', "`"$dll`"") }
    $p = Start-Process -FilePath $regsvr -ArgumentList $rsArgs -Wait -PassThru -NoNewWindow
    Write-Host "regsvr32 /u ($view) $dll -> exit $($p.ExitCode)"
  }
  foreach ($k in @("Software\Classes\CLSID\$cat\Instance\$clsid", "Software\Classes\CLSID\$clsid")) {
    if ($base.OpenSubKey($k)) { $base.DeleteSubKeyTree($k, $false); Write-Host "deleted $view $k" }
  }
  $base.Close()
}

Unregister-View ([Microsoft.Win32.RegistryView]::Registry64) (Join-Path $env:SystemRoot 'System32\regsvr32.exe')
Unregister-View ([Microsoft.Win32.RegistryView]::Registry32) (Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe')

if (-not $KeepFiles -and (Test-Path $InstallDir)) {
  foreach ($f in Get-ChildItem -Recurse -File $InstallDir) {
    try { Remove-Item -Force $f.FullName } catch { Write-Warning "in use, left in place: $($f.FullName)" }
  }
  Get-ChildItem -Recurse -Directory $InstallDir | Sort-Object FullName -Descending |
    Where-Object { -not (Get-ChildItem $_.FullName) } | Remove-Item -Force
  if (-not (Get-ChildItem $InstallDir)) { Remove-Item -Force $InstallDir }
}
Write-Host 'OWC Webcam unregistered.'
