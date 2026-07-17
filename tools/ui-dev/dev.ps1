# tools/ui-dev/dev.ps1 -- the daily Visage UI dev loop entry point (Windows).
#
# UNTESTED: authored on Linux; the maintainer cannot execute PowerShell here.
# It mirrors dev.sh -- verify on a real Windows box before relying on it.
#
# Activates the pinned emsdk (running setup.ps1 first if .emsdk is missing),
# configures the wasm build if needed, builds, and serves it with live rebuild.
# Default: the rs-editor app on http://127.0.0.1:8081.
#
# Usage:
#   .\tools\ui-dev\dev.ps1 [-Gallery] [-Rel] [-NoServe]
#
# Sandbox overrides (env vars) are honoured when set:
#   FACTORY_FREETYPE_MIRROR_DIR, FETCHCONTENT_SOURCE_DIR_VISAGE
param(
    [switch]$Gallery,
    [switch]$Rel,
    [switch]$NoServe
)
$ErrorActionPreference = "Stop"

$Here = Split-Path -Parent $PSCommandPath
$Repo = Resolve-Path (Join-Path $Here "..\..")
$Preset = if ($Rel) { "rel" } else { "dev" }
$App = if ($Gallery) { "gallery" } else { "rs-editor" }

# --- ensure emsdk exists, then activate it -----------------------------------
if (-not (Test-Path (Join-Path $Here ".emsdk\emsdk_env.ps1"))) {
    Write-Host "no .emsdk found -- running setup.ps1 first"
    & (Join-Path $Here "setup.ps1")
}

# emsdk_env quirk: emsdk_env.bat scopes its vars with setlocal, so calling it does
# NOT persist EMSDK/PATH into this session. Use the PowerShell entry (emsdk_env.ps1),
# which sets the process-level $env: vars that CMake's preset ($env{EMSDK}) reads.
# If emcc still isn't found afterward, run once: .\.emsdk\emsdk.bat activate 6.0.3
$env:EMSDK_QUIET = "1"
& (Join-Path $Here ".emsdk\emsdk_env.ps1") | Out-Null

# --- sandbox source overrides -> -D flags, only when non-empty ---------------
$overrides = @()
if ($env:FACTORY_FREETYPE_MIRROR_DIR) {
    $overrides += "-DFACTORY_FREETYPE_MIRROR_DIR=$($env:FACTORY_FREETYPE_MIRROR_DIR)"
}
if ($env:FETCHCONTENT_SOURCE_DIR_VISAGE) {
    $overrides += "-DFETCHCONTENT_SOURCE_DIR_VISAGE=$($env:FETCHCONTENT_SOURCE_DIR_VISAGE)"
}

$BuildDir = Join-Path $Here "build\$Preset"

# --- configure (only when the build dir is absent) ---------------------------
if (-not (Test-Path $BuildDir)) {
    Write-Host "== configure ($Preset) =="
    Push-Location $Here
    try { cmake --preset $Preset @overrides } finally { Pop-Location }
}

# --- build the app being served ----------------------------------------------
Write-Host "== build ($App, $Preset) =="
cmake --build $BuildDir --target $App

# --- app -> served dir / port / theme ----------------------------------------
if ($App -eq "gallery") {
    $WebDir = Join-Path $BuildDir "web"
    $Port = 8080
    $ThemeArgs = @()
} else {
    $WebDir = Join-Path $BuildDir "web-rs"
    $Port = 8081
    $ThemeArgs = @("--theme-file", (Join-Path $Repo "plugins\resonance-suppressor\ui\theme-rs.json"))
}
$Url = "http://127.0.0.1:$Port/index.html"

if ($NoServe) {
    Write-Host ""
    Write-Host "build complete (-NoServe). wasm output: $WebDir"
    exit 0
}

$Python = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } else { "py" }
Write-Host ""
Write-Host "serving $App at $Url   (edit source -> auto rebuild + reload; Ctrl-C to stop)"
& $Python (Join-Path $Here "dev_server.py") `
    --web-dir $WebDir --port $Port `
    @ThemeArgs `
    --watch --cmake-build-dir $BuildDir --target $App
