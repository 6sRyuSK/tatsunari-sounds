#!/bin/sh
# tatsunari-sounds installer bootstrap (macOS).
#
#   curl -fsSL https://6sryusk.com/tatsunarisounds/install.sh | bash
#
# Detects OS/arch, downloads the matching installer binary from the signed
# catalog at /tatsunarisounds/updates/v1/catalog.json, verifies SHA-256, and
# launches the TUI with the terminal reattached (so it works under a curl pipe).
#
# Trust model (plan §5.1): this script itself is HTTPS+CDN only; the binary it
# fetches is verified by SHA-256 from the catalog. Full minisign of the catalog
# is enforced inside the Go client once the public keys are embedded.
set -eu

BASE="https://6sryusk.com/tatsunarisounds"
CATALOG_URL="${BASE}/updates/v1/catalog.json"

os="$(uname -s)"
case "$os" in
  Darwin) catalog_os="macos" ;;
  *)
    echo "This installer supports macOS (use the PowerShell one-liner on Windows)." >&2
    echo "Detected OS: $os" >&2
    exit 1
    ;;
esac

arch="$(uname -m)"
case "$arch" in
  arm64 | aarch64) catalog_arch="arm64" ;;
  x86_64 | amd64) catalog_arch="amd64" ;;
  *)
    echo "Unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to resolve the installer asset from catalog.json." >&2
  exit 1
fi

echo "Fetching installer catalog…" >&2
resolved="$(python3 - "$CATALOG_URL" "$catalog_os" "$catalog_arch" <<'PY'
import hashlib, json, sys, urllib.request
url, os_id, arch = sys.argv[1], sys.argv[2], sys.argv[3]
with urllib.request.urlopen(url, timeout=60) as resp:
    doc = json.load(resp)
client = doc.get("client") or {}
assets = client.get("assets") or []
for a in assets:
    if a.get("os") == os_id and a.get("arch") == arch:
        print(a["url"])
        print(a["sha256"])
        print(a.get("size", 0))
        break
else:
    sys.stderr.write(f"no client asset for {os_id}/{arch}\n")
    sys.exit(1)
PY
)" || {
  echo "Could not resolve installer asset from ${CATALOG_URL}." >&2
  echo "The catalog may not be published yet." >&2
  exit 1
}

url="$(printf '%s\n' "$resolved" | sed -n '1p')"
want_sha="$(printf '%s\n' "$resolved" | sed -n '2p')"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
bin="${tmp}/tatsunari"

echo "Downloading installer…" >&2
curl -fsSL "$url" -o "$bin"
chmod +x "$bin"

got_sha="$(python3 - "$bin" <<'PY'
import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b""):
        h.update(chunk)
print(h.hexdigest())
PY
)"
if [ "$got_sha" != "$want_sha" ]; then
  echo "SHA-256 mismatch for installer binary." >&2
  echo "  want: $want_sha" >&2
  echo "  got:  $got_sha" >&2
  exit 1
fi

# Reattach the controlling terminal: under `curl … | bash` our stdin is the
# script pipe, so the TUI would otherwise be unable to read the keyboard.
if [ -r /dev/tty ]; then
  exec "$bin" </dev/tty
else
  exec "$bin"
fi
