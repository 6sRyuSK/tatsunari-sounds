#Requires -Version 5.1
<#
.SYNOPSIS
    Install built VST3 plugins into the shared "tatsunari-sounds" VST3 folder.

.DESCRIPTION
    Copies every built *.vst3 bundle found under build/ into
    "%CommonProgramFiles%\VST3\tatsunari-sounds\" (normally
    C:\Program Files\Common Files\VST3\tatsunari-sounds\).

    Writing to Program Files needs administrator rights, so the script
    self-elevates (UAC prompt) when not already running as admin.

.PARAMETER Name
    Wildcard filter on the plugin (bundle) name. Default "*" installs all.
    Example: -Name "*Reverb*"

.PARAMETER Config
    Build configuration to install from. Default "Release".

.EXAMPLE
    powershell -File tools\install.ps1
    Installs every Release bundle currently in build/.

.EXAMPLE
    powershell -File tools\install.ps1 -Name "Tammer Reverb"
    Installs only the Tammer Reverb bundle.
#>
[CmdletBinding()]
param(
    [string]$Name = "*",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

# Repo root: this script lives in <root>/tools.
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"
$Dest     = Join-Path ([Environment]::GetFolderPath("CommonProgramFiles")) "VST3\tatsunari-sounds"

# --- Self-elevate: Program Files requires administrator rights -------------
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Administrator rights are required to write to:`n  $Dest" -ForegroundColor Yellow
    Write-Host "Relaunching elevated (a UAC prompt will appear)..." -ForegroundColor Yellow
    $hostExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $argList = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit",
        "-File", "`"$PSCommandPath`"",
        "-Name", "`"$Name`"",
        "-Config", "`"$Config`""
    )
    Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList $argList
    return
}

# --- Find built bundles ----------------------------------------------------
$pattern = Join-Path $BuildDir "plugins\*\*_artefacts\$Config\VST3\*.vst3"
# Note: for a directory named "X.vst3", PowerShell's .BaseName keeps the
# ".vst3" (directories are treated as extension-less), so strip it ourselves
# to match the displayed plugin name.
$bundles = @(Get-ChildItem -Path $pattern -Directory -ErrorAction SilentlyContinue |
    Where-Object { ($_.Name -replace '\.vst3$', '') -like $Name })

if ($bundles.Count -eq 0) {
    Write-Host "No '$Config' VST3 bundles matching '$Name' found under:" -ForegroundColor Red
    Write-Host "  $BuildDir" -ForegroundColor Red
    Write-Host "Build the plugins first (e.g. cmake --build build --config $Config)." -ForegroundColor Red
    exit 1
}

# --- Install ---------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Write-Host "Installing $($bundles.Count) plugin(s) -> $Dest" -ForegroundColor Cyan

foreach ($b in $bundles) {
    $target = Join-Path $Dest $b.Name
    if (Test-Path $target) {
        Remove-Item -Recurse -Force $target
    }
    Copy-Item -Recurse -Force -Path $b.FullName -Destination $target
    Write-Host "  installed: $($b.Name)" -ForegroundColor Green
}

Write-Host "Done." -ForegroundColor Cyan
