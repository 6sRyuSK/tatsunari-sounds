#!/usr/bin/env bash
# tools/ui-dev/setup.sh — one-shot developer bootstrap for the Visage UI dev harness.
#
# Installs the pinned Emscripten SDK (6.0.3) into tools/ui-dev/.emsdk and verifies
# the host build tools are present. Idempotent + upgrade-safe: re-running is cheap
# (emsdk install/activate are skipped when 6.0.3 is already active). macOS + Linux.
#
# Usage:
#   ./tools/ui-dev/setup.sh [--with-playwright]
#
#   --with-playwright   also `npm install` the headless-verify deps in playwright/
#
# It never installs system packages for you: for any missing host tool it prints
# the exact per-OS install command and exits non-zero.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EMSDK_VERSION="6.0.3"
EMSDK_DIR="$HERE/.emsdk"
WITH_PLAYWRIGHT=0

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,"");print;next} {exit}' "$0"; }

for arg in "$@"; do
  case "$arg" in
    --with-playwright) WITH_PLAYWRIGHT=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "setup.sh: unknown argument: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

# --- per-OS install hints ----------------------------------------------------
OS="$(uname -s)"
hint() { # hint <tool> <brew-formula> <apt-package>
  case "$OS" in
    Darwin) echo "      brew install $2" ;;
    Linux)  echo "      sudo apt-get install -y $3" ;;
    *)      echo "      (install '$1' with your package manager)" ;;
  esac
}

# --- host-tool checks --------------------------------------------------------
MISSING=0
have() { command -v "$1" >/dev/null 2>&1; }

cmake_ge_322() { # cmake >= 3.22 required by CMakePresets.json (v3 presets)
  local v major minor
  v="$(cmake --version 2>/dev/null | awk 'NR==1{print $3}')" # e.g. 3.28.3
  major="${v%%.*}"; minor="${v#*.}"; minor="${minor%%.*}"
  [ -n "$major" ] && [ -n "$minor" ] || return 1
  [ "$major" -gt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -ge 22 ]; }
}

echo "== host tools =="
for spec in "git:git:git" "ninja:ninja:ninja-build" "python3:python:python3"; do
  cmd="${spec%%:*}"; rest="${spec#*:}"; brew="${rest%%:*}"; apt="${rest#*:}"
  if have "$cmd"; then
    echo "  ok    $cmd"
  else
    echo "  MISS  $cmd"; hint "$cmd" "$brew" "$apt"; MISSING=1
  fi
done

if ! have cmake; then
  echo "  MISS  cmake"; hint cmake cmake cmake; MISSING=1
elif ! cmake_ge_322; then
  echo "  MISS  cmake >= 3.22 (found $(cmake --version | awk 'NR==1{print $3}'))"
  hint cmake cmake cmake; MISSING=1
else
  echo "  ok    cmake"
fi

# node/npm are ONLY needed for the optional Playwright verify — never fatal here
# unless --with-playwright was requested.
if have node; then
  echo "  ok    node (optional)"
else
  echo "  --    node (optional; only for the Playwright verify)"
  hint node node "nodejs npm"
fi

if [ "$MISSING" -ne 0 ]; then
  echo
  echo "Missing required host tools (see the install lines above). Install them and re-run." >&2
  exit 1
fi

# --- .emsdk is a local build tool, never committed ---------------------------
GITIGNORE="$HERE/.gitignore"
if [ ! -f "$GITIGNORE" ] || ! grep -qxF ".emsdk/" "$GITIGNORE"; then
  echo ".emsdk/" >> "$GITIGNORE"
  echo "added '.emsdk/' to tools/ui-dev/.gitignore"
fi

# --- install the pinned Emscripten SDK ---------------------------------------
echo
echo "== emsdk $EMSDK_VERSION -> $EMSDK_DIR =="
if [ ! -x "$EMSDK_DIR/emsdk" ]; then
  rm -rf "$EMSDK_DIR"           # clear any half-clone so git clone succeeds
  echo "cloning emsdk..."
  git clone --depth 1 https://github.com/emscripten-core/emsdk "$EMSDK_DIR"
fi

VERSION_FILE="$EMSDK_DIR/upstream/emscripten/emscripten-version.txt"
if [ -f "$EMSDK_DIR/.emscripten" ] && [ -f "$VERSION_FILE" ] \
   && grep -q "$EMSDK_VERSION" "$VERSION_FILE"; then
  echo "emsdk $EMSDK_VERSION already installed + activated — skipping"
else
  ( cd "$EMSDK_DIR" && ./emsdk install "$EMSDK_VERSION" && ./emsdk activate "$EMSDK_VERSION" )
fi

# --- optional: Playwright headless-verify deps -------------------------------
if [ "$WITH_PLAYWRIGHT" -eq 1 ]; then
  echo
  echo "== playwright deps =="
  if ! have npm; then
    echo "npm is required for --with-playwright:" >&2
    hint npm node "nodejs npm" >&2
    exit 1
  fi
  ( cd "$HERE/playwright" && npm install )
  echo "playwright deps installed (browsers are provided at /opt/pw-browsers; never 'playwright install')"
fi

# --- summary -----------------------------------------------------------------
echo
echo "== setup complete =="
echo "  emsdk       $EMSDK_VERSION  ($EMSDK_DIR)"
[ "$WITH_PLAYWRIGHT" -eq 1 ] && echo "  playwright  tools/ui-dev/playwright/node_modules"
echo
echo "start the daily loop with:"
echo "    ./tools/ui-dev/dev.sh            # rs-editor on http://127.0.0.1:8081"
echo "    ./tools/ui-dev/dev.sh --gallery  # widget gallery on http://127.0.0.1:8080"
