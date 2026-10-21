<#
.SYNOPSIS
  Installs (or removes) the OWC OBS Studio source plugin.
.DESCRIPTION
  OBS 28+ searches %ProgramData%\obs-studio\plugins\<name>\bin\64bit and ...\data
  (frontend/widgets/OBSBasic.cpp AddExtraModulePaths, verified for 32.0.4).
  ProgramData is normally writable only by admins for new subfolders; this script
  does NOT elevate - it reports and exits 1 if access is denied.
  -Uninstall removes the plugin folder.
#>
[CmdletBinding()]
param(
  [switch]$Uninstall,
  [string]$BuildDir = '',
  [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $root '..\build\obs\out\obs-plugins\owc-obs-source' }
if (-not $Destination) { $Destination = Join-Path $env:ProgramData 'obs-studio\plugins\owc-obs-source' }

if ($Uninstall) {
  if (-not (Test-Path $Destination)) { Write-Host "Not installed: $Destination"; exit 0 }
  try { Remove-Item -Recurse -Force $Destination; Write-Host "Removed $Destination" }
  catch { Write-Error "Cannot remove $Destination (admin rights needed?): $($_.Exception.Message)"; exit 1 }
  exit 0
}

$dll = Join-Path $BuildDir 'bin\64bit\owc-obs-source.dll'
if (-not (Test-Path $dll)) { Write-Error "Build the plugin first; missing $dll"; exit 1 }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { Write-Warning 'OBS is running; the DLL may be locked. Close OBS first.' }

try {
  New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'bin\64bit') | Out-Null
  New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'data') | Out-Null
  Copy-Item $dll (Join-Path $Destination 'bin\64bit') -Force
  Copy-Item (Join-Path $BuildDir 'data\*') (Join-Path $Destination 'data') -Recurse -Force
  Write-Host "Installed to $Destination"
} catch {
  Write-Error "Install failed (run from an elevated PowerShell if ProgramData is not writable): $($_.Exception.Message)"
  exit 1
}
