#!/bin/sh
# tatsunari-plugins installer bootstrap (macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.sh | bash
#
# Detects OS/arch, downloads the matching installer binary from the latest
# release, and launches the TUI with the terminal reattached (so it works under
# a curl pipe).
set -eu

REPO="6sRyuSK/tatsunari-plugins"

os="$(uname -s)"
case "$os" in
  Darwin) goos="darwin" ;;
  *)
    echo "This installer supports macOS (use the PowerShell one-liner on Windows)." >&2
    echo "Detected OS: $os" >&2
    exit 1
    ;;
esac

arch="$(uname -m)"
case "$arch" in
  arm64 | aarch64) goarch="arm64" ;;
  x86_64 | amd64) goarch="amd64" ;;
  *)
    echo "Unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

asset="tatsunari-${goos}-${goarch}"
api="https://api.github.com/repos/${REPO}/releases/latest"

echo "Finding the latest tatsunari-plugins release…" >&2
url="$(curl -fsSL "$api" | grep -o "https://[^\"[:space:]]*/${asset}" | head -n1)"
if [ -z "${url:-}" ]; then
  echo "Could not find asset '${asset}' in the latest release." >&2
  echo "The installer binary may not be published yet." >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
bin="${tmp}/tatsunari"

echo "Downloading ${asset}…" >&2
curl -fsSL "$url" -o "$bin"
chmod +x "$bin"

# Reattach the controlling terminal: under `curl … | bash` our stdin is the
# script pipe, so the TUI would otherwise be unable to read the keyboard.
if [ -r /dev/tty ]; then
  exec "$bin" </dev/tty
else
  exec "$bin"
fi
