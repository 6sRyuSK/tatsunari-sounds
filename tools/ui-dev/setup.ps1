# tools/ui-dev/setup.ps1 -- one-shot developer bootstrap for the Visage UI dev harness (Windows).
#
# UNTESTED: authored on Linux; the maintainer cannot execute PowerShell here.
# It mirrors setup.sh -- verify on a real Windows box before relying on it.
#
# Installs the pinned Emscripten SDK (6.0.3) into tools/ui-dev\.emsdk and verifies
# the host build tools. Idempotent + upgrade-safe.
#
# Usage:
#   .\tools\ui-dev\setup.ps1 [-WithPlaywright]
param(
    [switch]$WithPlaywright
)
$ErrorActionPreference = "Stop"

$Here = Split-Path -Parent $PSCommandPath
$EmsdkVersion = "6.0.3"
$EmsdkDir = Join-Path $Here ".emsdk"

function Have($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

# winget line first, choco as the fallback hint.
function Hint($winget, $choco) {
    Write-Host "      winget install --id $winget -e   # or: choco install $choco"
}

$missing = $false
Write-Host "== host tools =="
foreach ($t in @(
        @{cmd="git";    winget="Git.Git";        choco="git"},
        @{cmd="ninja";  winget="Ninja-build.Ninja"; choco="ninja"},
        @{cmd="cmake";  winget="Kitware.CMake";   choco="cmake"})) {
    if (Have $t.cmd) { Write-Host "  ok    $($t.cmd)" }
    else { Write-Host "  MISS  $($t.cmd)"; Hint $t.winget $t.choco; $missing = $true }
}

# cmake >= 3.22 (v3 presets)
if (Have cmake) {
    $ver = ((cmake --version)[0] -replace '[^0-9.]', '').Split('.')
    if ([int]$ver[0] -lt 3 -or ([int]$ver[0] -eq 3 -and [int]$ver[1] -lt 22)) {
        Write-Host "  MISS  cmake >= 3.22 (found $($ver -join '.'))"
        Hint "Kitware.CMake" "cmake"; $missing = $true
    }
}

# python: Windows usually exposes `python` (or the `py` launcher), not `python3`.
$Python = if (Have python) { "python" } elseif (Have py) { "py" } else { $null }
if ($Python) { Write-Host "  ok    $Python" }
else { Write-Host "  MISS  python"; Hint "Python.Python.3.12" "python"; $missing = $true }

# node/npm are only needed for the optional Playwright verify.
if (Have node) { Write-Host "  ok    node (optional)" }
else { Write-Host "  --    node (optional; only for the Playwright verify)"; Hint "OpenJS.NodeJS" "nodejs" }

if ($missing) {
    Write-Host ""
    Write-Error "Missing required host tools (see the install lines above). Install them and re-run."
    exit 1
}

# --- .emsdk is a local build tool, never committed ---------------------------
$gitignore = Join-Path $Here ".gitignore"
if (-not (Test-Path $gitignore) -or -not (Select-String -Path $gitignore -Pattern '^\.emsdk/$' -Quiet)) {
    Add-Content -Path $gitignore -Value ".emsdk/"
    Write-Host "added '.emsdk/' to tools/ui-dev/.gitignore"
}

# --- install the pinned Emscripten SDK ---------------------------------------
Write-Host ""
Write-Host "== emsdk $EmsdkVersion -> $EmsdkDir =="
if (-not (Test-Path (Join-Path $EmsdkDir "emsdk.bat"))) {
    if (Test-Path $EmsdkDir) { Remove-Item -Recurse -Force $EmsdkDir }
    Write-Host "cloning emsdk..."
    git clone --depth 1 https://github.com/emscripten-core/emsdk $EmsdkDir
}

$versionFile = Join-Path $EmsdkDir "upstream\emscripten\emscripten-version.txt"
$dotEmscripten = Join-Path $EmsdkDir ".emscripten"
if ((Test-Path $dotEmscripten) -and (Test-Path $versionFile) -and
    (Select-String -Path $versionFile -Pattern $EmsdkVersion -Quiet)) {
    Write-Host "emsdk $EmsdkVersion already installed + activated -- skipping"
} else {
    Push-Location $EmsdkDir
    try {
        & .\emsdk.bat install $EmsdkVersion
        & .\emsdk.bat activate $EmsdkVersion
    } finally { Pop-Location }
}

# --- optional: Playwright headless-verify deps -------------------------------
if ($WithPlaywright) {
    Write-Host ""
    Write-Host "== playwright deps =="
    if (-not (Have npm)) { Write-Error "npm is required for -WithPlaywright"; exit 1 }
    Push-Location (Join-Path $Here "playwright")
    try { npm install } finally { Pop-Location }
}

Write-Host ""
Write-Host "== setup complete =="
Write-Host "  emsdk       $EmsdkVersion  ($EmsdkDir)"
Write-Host ""
Write-Host "start the daily loop with:"
Write-Host "    .\tools\ui-dev\dev.ps1            # rs-editor on http://127.0.0.1:8081"
Write-Host "    .\tools\ui-dev\dev.ps1 -Gallery   # widget gallery on http://127.0.0.1:8080"
