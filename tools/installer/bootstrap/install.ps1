# tatsunari-plugins installer bootstrap (Windows).
#
#   irm https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.ps1 | iex
#
# Downloads the matching installer binary from the latest release and launches
# the TUI. The installer runs unelevated; it asks the OS for a UAC prompt only
# when you choose a system-wide install.
$ErrorActionPreference = 'Stop'

$repo = '6sRyuSK/tatsunari-plugins'

$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
$asset = "tatsunari-windows-$arch.exe"

Write-Host "Finding the latest tatsunari-plugins release..."
$rel = Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$repo/releases/latest"
$dl = ($rel.assets | Where-Object { $_.name -eq $asset }).browser_download_url
if (-not $dl) {
    # Only windows-amd64 is currently published; fall back to it on arm64.
    $asset = 'tatsunari-windows-amd64.exe'
    $dl = ($rel.assets | Where-Object { $_.name -eq $asset }).browser_download_url
}
if (-not $dl) {
    throw "Could not find '$asset' in the latest release. The installer binary may not be published yet."
}

$dir = Join-Path $env:TEMP ("tatsunari-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $dir | Out-Null
$bin = Join-Path $dir 'tatsunari.exe'

Write-Host "Downloading $asset..."
Invoke-WebRequest -UseBasicParsing $dl -OutFile $bin

# Pick the bilingual UI language from the Windows culture (Windows has no $LANG).
if (-not $env:TATSUNARI_LANG) {
    if ((Get-Culture).Name -like 'ja*') { $env:TATSUNARI_LANG = 'ja' } else { $env:TATSUNARI_LANG = 'en' }
}

& $bin
