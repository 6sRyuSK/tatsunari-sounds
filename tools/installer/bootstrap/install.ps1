# tatsunari-sounds installer bootstrap (Windows).
#
#   irm https://6sryusk.com/tatsunarisounds/install.ps1 | iex
#
# Resolves the matching installer binary from
# /tatsunarisounds/updates/v1/catalog.json, verifies SHA-256, and launches the
# TUI. The installer runs unelevated; it asks the OS for a UAC prompt only when
# you choose a system-wide install.
$ErrorActionPreference = 'Stop'

$Base = 'https://6sryusk.com/tatsunarisounds'
$CatalogUrl = "$Base/updates/v1/catalog.json"
$catalogOs = 'windows'
$catalogArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }

Write-Host "Fetching installer catalog..."
$doc = Invoke-RestMethod -UseBasicParsing $CatalogUrl
$asset = $doc.client.assets | Where-Object { $_.os -eq $catalogOs -and $_.arch -eq $catalogArch } | Select-Object -First 1
if (-not $asset) {
    # Fall back to amd64 when an arm64 client asset is not published yet.
    $asset = $doc.client.assets | Where-Object { $_.os -eq $catalogOs -and $_.arch -eq 'amd64' } | Select-Object -First 1
}
if (-not $asset) {
    throw "Could not resolve a windows installer asset from $CatalogUrl"
}

$dir = Join-Path $env:TEMP ("tatsunari-sounds-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $dir | Out-Null
$bin = Join-Path $dir 'tatsunari-sounds-installer.exe'

Write-Host "Downloading installer..."
Invoke-WebRequest -UseBasicParsing $asset.url -OutFile $bin

$sha = [System.Security.Cryptography.SHA256]::Create()
$fs = [System.IO.File]::OpenRead($bin)
try {
    $hash = ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join ''
} finally {
    $fs.Dispose()
    $sha.Dispose()
}
if ($hash -ne $asset.sha256) {
    throw "SHA-256 mismatch for installer binary (want $($asset.sha256), got $hash)"
}

# Pick the bilingual UI language from the Windows culture (Windows has no $LANG).
if (-not $env:TATSUNARI_LANG) {
    if ((Get-Culture).Name -like 'ja*') { $env:TATSUNARI_LANG = 'ja' } else { $env:TATSUNARI_LANG = 'en' }
}

& $bin
