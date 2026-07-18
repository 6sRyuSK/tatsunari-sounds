#requires -Version 5.1
<#
.SYNOPSIS
  Build (and optionally install) the resonance-suppressor CLAP coexistence
  shell from a normal PowerShell window.

.DESCRIPTION
  Bootstraps the Visual Studio 2022 x64 toolchain into the current session
  (the same environment the "x64 Native Tools Command Prompt for VS 2022"
  gives you), then runs the exact CMake configure/build the CI uses:

      cmake -B build-clap -G Ninja -DCMAKE_BUILD_TYPE=Release `
            -DFACTORY_RS_CLAP=ON -DFACTORY_PLUGINS=resonance-suppressor
      cmake --build build-clap --target resonance-suppressor_all

  So you no longer have to hunt for the Native Tools prompt. With -Install it
  also copies the built .clap / .vst3 into your per-user plugin folders
  (no admin needed), ready for a DAW rescan.

  This drives ONLY the flag-gated coexistence build (FACTORY_RS_CLAP). It does
  not touch the shipping JUCE VST3/AU, which never sees this path.

.EXAMPLE
  .\tools\build-clap.ps1
      Configure + build the CLAP + wrapper VST3 (GUI embedded).

.EXAMPLE
  .\tools\build-clap.ps1 -Clean -Install
      Wipe the build dir, rebuild, then install into the per-user CLAP/VST3 dirs.

.EXAMPLE
  .\tools\build-clap.ps1 -NoGui
      Build the pure-headless 3a shell (no Visage editor linked).
#>
[CmdletBinding()]
param(
    # Plugin slug to build (defaults to the RS coexistence shell).
    [string]$Slug = 'resonance-suppressor',
    # CMake build directory (relative to the repo root).
    [string]$BuildDir = 'build-clap',
    # Remove the build directory before configuring (forces a clean rebuild).
    [switch]$Clean,
    # Build the headless 3a shell (FACTORY_RS_CLAP_GUI=OFF); default embeds the editor.
    [switch]$NoGui,
    # Copy the built .clap / .vst3 into the per-user plugin folders after building.
    [switch]$Install,
    # Override the VS installation path if vswhere cannot locate it.
    [string]$VsPath
)

$ErrorActionPreference = 'Stop'

# --- Locate the repo root (this script lives in <repo>/tools) -----------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot
Write-Host "Repo root: $RepoRoot" -ForegroundColor DarkGray

# --- Bring the VS 2022 x64 toolchain into this PowerShell session -------------
function Enter-VsX64Environment {
    param([string]$Override)

    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host "MSVC (cl.exe) already on PATH - reusing current environment." -ForegroundColor DarkGray
        return
    }

    $vsPath = $Override
    if (-not $vsPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path $vswhere)) {
            throw "vswhere.exe not found ($vswhere). Visual Studio 2017+ (with the C++ workload) is required, or pass -VsPath."
        }
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if (-not $vsPath) {
            throw "No VS install with the x64 C++ toolset was found. In the Visual Studio Installer, add the 'Desktop development with C++' workload."
        }
    }
    Write-Host "Visual Studio: $vsPath" -ForegroundColor DarkGray

    # Preferred: the official DevShell module (clean, keeps our cwd).
    $devShellDll = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (Test-Path $devShellDll) {
        try {
            Import-Module $devShellDll -ErrorAction Stop
            Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
                -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
            Set-Location $RepoRoot   # DevShell can move us; restore.
            if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
                Write-Host "VS x64 environment loaded (DevShell)." -ForegroundColor Green
                return
            }
        } catch {
            Write-Warning "Enter-VsDevShell failed ($($_.Exception.Message)); falling back to vcvars64.bat."
        }
    }

    # Fallback: run vcvars64.bat in cmd and import the resulting environment.
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found ($vcvars)." }
    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "MSVC still not on PATH after loading vcvars64.bat."
    }
    Write-Host "VS x64 environment loaded (vcvars64.bat)." -ForegroundColor Green
}

Enter-VsX64Environment -Override $VsPath

# --- Clean / detect a broken cache from an earlier failed configure -----------
function Test-BrokenCache {
    param([string]$Dir)
    $cache = Join-Path $Dir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return $false }
    $line = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:[^=]*=(.+)$' | Select-Object -First 1
    if (-not $line) { return $true }                          # compiler never detected
    return ($line.Matches[0].Groups[1].Value -match 'NOTFOUND')
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing build dir (-Clean): $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
} elseif (Test-BrokenCache -Dir $BuildDir) {
    Write-Host "Detected a broken CMake cache in $BuildDir (earlier configure failed with no compiler) - removing it." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# --- Pick the generator: Ninja (matches CI) if available, else VS generator ---
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $genArgs  = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
    $buildCfg = @()
    Write-Host "Generator: Ninja" -ForegroundColor DarkGray
} else {
    Write-Warning "ninja not found; using the Visual Studio generator (slower, multi-config)."
    $genArgs  = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    $buildCfg = @('--config', 'Release')
}

$guiArgs = if ($NoGui) { @('-DFACTORY_RS_CLAP_GUI=OFF') } else { @() }

# --- Configure ----------------------------------------------------------------
Write-Host "`n== Configure ==" -ForegroundColor Cyan
& cmake -B $BuildDir @genArgs '-DFACTORY_RS_CLAP=ON' "-DFACTORY_PLUGINS=$Slug" @guiArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

# --- Build (only the aggregate target; NEVER `all`) ---------------------------
Write-Host "`n== Build (${Slug}_all) ==" -ForegroundColor Cyan
& cmake --build $BuildDir --target "${Slug}_all" @buildCfg
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)." }

# --- Locate the built artifacts ----------------------------------------------
$assets = Join-Path $BuildDir "${Slug}_assets"
$clap = Get-ChildItem -Path $assets -Recurse -Filter *.clap -ErrorAction SilentlyContinue | Select-Object -First 1
$vst3 = Get-ChildItem -Path $assets -Recurse -Filter *.vst3 -ErrorAction SilentlyContinue | Select-Object -First 1

Write-Host "`n== Artifacts ==" -ForegroundColor Cyan
if ($clap) { Write-Host "  CLAP: $($clap.FullName)" -ForegroundColor Green } else { Write-Warning "  No .clap found under $assets" }
if ($vst3) { Write-Host "  VST3: $($vst3.FullName)" -ForegroundColor Green } else { Write-Warning "  No .vst3 found under $assets" }

# --- Optional install into the per-user plugin folders (no admin needed) ------
if ($Install) {
    Write-Host "`n== Install (per-user) ==" -ForegroundColor Cyan
    $clapDst = Join-Path $env:LOCALAPPDATA 'Programs\Common\CLAP'
    $vst3Dst = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
    New-Item -ItemType Directory -Force -Path $clapDst, $vst3Dst | Out-Null

    if ($clap) {
        Copy-Item -Path $clap.FullName -Destination $clapDst -Recurse -Force
        Write-Host "  -> $clapDst\$($clap.Name)" -ForegroundColor Green
    }
    if ($vst3) {
        Copy-Item -Path $vst3.FullName -Destination $vst3Dst -Recurse -Force
        Write-Host "  -> $vst3Dst\$($vst3.Name)" -ForegroundColor Green
    }
    Write-Host "Installed. Rescan plugins in your DAW to pick them up." -ForegroundColor Green
    Write-Host "(These per-user paths are scanned by Reaper/Bitwig/etc. alongside Program Files.)" -ForegroundColor DarkGray
} else {
    Write-Host "`nTip: re-run with -Install to copy these into your per-user CLAP/VST3 folders." -ForegroundColor DarkGray
}

Write-Host "`nDone." -ForegroundColor Green
