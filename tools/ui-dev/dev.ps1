# tools/ui-dev/dev.ps1 -- the daily Visage UI dev loop entry point (Windows).
#
# Activates the pinned emsdk (running setup.ps1 first if .emsdk is missing),
# configures the wasm build if needed, builds, and serves it with live rebuild.
# Default: the rs-editor app on http://127.0.0.1:8081.
#
# Usage:
#   .\tools\ui-dev\dev.ps1 [-App rs-editor|gallery|pitch-fix|dynamic-eq] [-Rel] [-NoServe | -Verify]
#
# Sandbox overrides (env vars) are honoured when set:
#   FACTORY_FREETYPE_MIRROR_DIR, FETCHCONTENT_SOURCE_DIR_VISAGE
param(
    [ValidateSet("gallery", "rs-editor", "pitch-fix", "dynamic-eq")]
    [string]$App = "rs-editor",
    [switch]$Gallery,
    [switch]$Rel,
    [switch]$NoServe,
    [switch]$Verify
)
$ErrorActionPreference = "Stop"

if ($Verify -and $NoServe) {
    throw "-Verify and -NoServe cannot be combined"
}

function CanRunPython($command) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { return $false }
    try {
        & $command --version *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

$Here = Split-Path -Parent $PSCommandPath
$Repo = Resolve-Path (Join-Path $Here "..\..")
$Preset = if ($Rel) { "rel" } else { "dev" }
if ($Gallery) { $App = "gallery" } # backwards-compatible shorthand

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
    try {
        cmake --preset $Preset @overrides
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    } finally { Pop-Location }
}

# --- build the app being served ----------------------------------------------
Write-Host "== build ($App, $Preset) =="
cmake --build $BuildDir --target $App
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

# --- app -> served dir / port / theme ----------------------------------------
switch ($App) {
    "gallery" {
        $WebDir = Join-Path $BuildDir "web"; $Port = 8080; $ThemeArgs = @()
    }
    "rs-editor" {
        $WebDir = Join-Path $BuildDir "web-rs"; $Port = 8081
        $ThemeArgs = @("--theme-file", (Join-Path $Repo "plugins\resonance-suppressor\ui\theme-rs.json"))
    }
    "pitch-fix" {
        $WebDir = Join-Path $BuildDir "web-pf"; $Port = 8082; $ThemeArgs = @()
    }
    "dynamic-eq" {
        $WebDir = Join-Path $BuildDir "web-deq"; $Port = 8083; $ThemeArgs = @()
    }
}
$Url = "http://127.0.0.1:$Port/index.html"

if ($Verify) {
    if (-not (Test-Path (Join-Path $Here "playwright\node_modules\playwright"))) {
        & (Join-Path $Here "setup.ps1") -WithPlaywright
    }
    node (Join-Path $Here "playwright\verify.js") `
        --app $App --build-dir $BuildDir --out (Join-Path $Here "artifacts")
    if ($LASTEXITCODE -ne 0) { throw "Playwright verification failed" }
    exit 0
}

if ($NoServe) {
    Write-Host ""
    Write-Host "build complete (-NoServe). wasm output: $WebDir"
    exit 0
}

$Python = if ($env:PYTHON_BIN -and (CanRunPython $env:PYTHON_BIN)) {
    $env:PYTHON_BIN
} elseif (CanRunPython "python") {
    "python"
} elseif (CanRunPython "py") {
    "py"
} else {
    throw "Python 3 was not found. Install it or set PYTHON_BIN."
}
Write-Host ""
Write-Host "serving $App at $Url   (edit source -> auto rebuild + reload; Ctrl-C to stop)"
& $Python (Join-Path $Here "dev_server.py") `
    --web-dir $WebDir --port $Port `
    @ThemeArgs `
    --watch --cmake-build-dir $BuildDir --target $App
