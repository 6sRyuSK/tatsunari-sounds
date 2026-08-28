#!/bin/sh
# tatsunari-sounds bootstrap shim (macOS) — published at
# https://6sryusk.com/tatsunarisounds/install.sh
#
#   curl -fsS --proto '=https' --tlsv1.2 https://6sryusk.com/tatsunarisounds/install.sh | sh
#
# WHY THIS FILE EXISTS (plan §5 "bootstrap", §5.1)
#
# The bootstrap is the one link in the chain minisign cannot protect: verifying
# it would need the public key, and fetching the public key is the very thing
# being bootstrapped. The plan's answer is not to pretend otherwise but to
# shrink the unverifiable surface to something a human can read in one screen:
#
#   * this shim is the ONLY mutable, unverified object, and all it does is fetch
#     a VERSIONED, IMMUTABLE script and check its SHA-256 before running it;
#   * the pinned digest below is generated from the payload by
#     tools/promote/bootstrap.py, and tools/tests/test_bootstrap_pin.py fails
#     the build if it ever drifts from the committed payload.
#
# So an attacker who can rewrite this file can still do anything — but an
# attacker who can only tamper with the (much longer, much more cacheable)
# payload cannot, and neither can a corrupted CDN object or a truncated
# download.
#
# DO NOT add features here. Everything that can live in the verified payload
# must live in the verified payload.
set -eu

BASE="https://6sryusk.com/tatsunarisounds"

# --- generated pin: do not edit by hand (tools/promote/bootstrap.py) ---
BOOTSTRAP_VERSION="1"
BOOTSTRAP_SHA256="222467d4095e12bdb23f36f66af039baa0f5ccd04f647c9cdfce7a103aa92665"
# --- end generated pin ---

PAYLOAD_URL="${BASE}/bootstrap/${BOOTSTRAP_VERSION}/install.sh"

if ! command -v curl >/dev/null 2>&1; then
  echo "curl is required." >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
payload="${tmp}/install.sh"

# --proto '=https' refuses anything but HTTPS. No -L: a redirect is a change of
# origin we did not agree to, and following one silently is how a bootstrap ends
# up executing somebody else's script. -f turns 4xx/5xx into a non-zero exit;
# the explicit status check below catches the 3xx that -f lets through.
status="$(curl -fsS --proto '=https' --tlsv1.2 \
               -o "$payload" -w '%{http_code}' "$PAYLOAD_URL" || true)"
if [ "$status" != "200" ]; then
  echo "Refusing to continue: ${PAYLOAD_URL} returned HTTP ${status:-<none>}." >&2
  echo "A redirect or an error here means the bootstrap is not being served as published." >&2
  exit 1
fi

if command -v shasum >/dev/null 2>&1; then
  got="$(shasum -a 256 "$payload" | cut -d' ' -f1)"
elif command -v sha256sum >/dev/null 2>&1; then
  got="$(sha256sum "$payload" | cut -d' ' -f1)"
else
  echo "Neither shasum nor sha256sum is available; cannot verify the bootstrap." >&2
  exit 1
fi

if [ "$got" != "$BOOTSTRAP_SHA256" ]; then
  echo "SHA-256 mismatch for the bootstrap payload." >&2
  echo "  expected ${BOOTSTRAP_SHA256}" >&2
  echo "  got      ${got}" >&2
  echo "Not executing it. Report this: the published script does not match the pin." >&2
  exit 1
fi

exec sh "$payload" "$@"
