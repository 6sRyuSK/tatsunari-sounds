#!/usr/bin/env bash
# tools/ui-dev/dev.sh — the daily Visage UI dev loop entry point.
#
# Activates the pinned emsdk (running setup.sh first if .emsdk is missing),
# configures the wasm build if needed, builds, and serves it with live rebuild +
# browser auto-reload. Default: the rs-editor app on http://127.0.0.1:8081.
#
# Usage:
#   ./tools/ui-dev/dev.sh [--app rs-editor|gallery|pitch-fix|dynamic-eq] [--rel] [--no-serve|--verify]
#
#   --app NAME   select rs-editor (:8081), gallery (:8080), pitch-fix (:8082), or dynamic-eq (:8083)
#   --gallery    backwards-compatible shorthand for --app gallery
#   --rel        use the `rel` preset (-O2, small wasm) instead of `dev` (-O0, fast link)
#   --no-serve   configure + build only, then exit (no dev server)
#   --verify     configure + build, run Playwright with a temporary server, exit
#
# Sandbox overrides (pointed at local checkouts) are honoured when set:
#   FACTORY_FREETYPE_MIRROR_DIR      -> -DFACTORY_FREETYPE_MIRROR_DIR (FreeType source)
#   FETCHCONTENT_SOURCE_DIR_VISAGE   -> -DFETCHCONTENT_SOURCE_DIR_VISAGE (visage source)
# Leave them unset on a normal machine and the deps fetch over the network.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

PRESET="dev"
SERVE=1
VERIFY=0
APP="rs-editor"

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,"");print;next} {exit}' "$0"; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --app)
      [ "$#" -ge 2 ] || { echo "dev.sh: --app requires a value" >&2; exit 2; }
      APP="$2"; shift ;;
    --gallery)  APP="gallery" ;;
    --rel)      PRESET="rel" ;;
    --no-serve) SERVE=0 ;;
    --verify)   VERIFY=1 ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "dev.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

case "$APP" in
  gallery|rs-editor|pitch-fix|dynamic-eq) ;;
  *) echo "dev.sh: invalid --app: $APP" >&2; exit 2 ;;
esac

if [ "$VERIFY" -eq 1 ] && [ "$SERVE" -eq 0 ]; then
  echo "dev.sh: --verify and --no-serve cannot be combined" >&2
  exit 2
fi

# --- ensure emsdk exists, then activate it -----------------------------------
if [ ! -f "$HERE/.emsdk/emsdk_env.sh" ]; then
  echo "no .emsdk found — running setup.sh first"
  "$HERE/setup.sh"
fi

export EMSDK_QUIET=1
set +eu # emsdk_env.sh is not written for `set -euo pipefail`
# shellcheck disable=SC1091
. "$HERE/.emsdk/emsdk_env.sh"
set -eu

# --- sandbox source overrides -> -D flags, only when non-empty ---------------
OVERRIDES=()
[ -n "${FACTORY_FREETYPE_MIRROR_DIR:-}" ] \
  && OVERRIDES+=("-DFACTORY_FREETYPE_MIRROR_DIR=$FACTORY_FREETYPE_MIRROR_DIR")
[ -n "${FETCHCONTENT_SOURCE_DIR_VISAGE:-}" ] \
  && OVERRIDES+=("-DFETCHCONTENT_SOURCE_DIR_VISAGE=$FETCHCONTENT_SOURCE_DIR_VISAGE")

BUILD_DIR="$HERE/build/$PRESET"

# --- configure (only when the build dir is absent) ---------------------------
if [ ! -d "$BUILD_DIR" ]; then
  echo "== configure ($PRESET) =="
  ( cd "$HERE" && cmake --preset "$PRESET" ${OVERRIDES[@]+"${OVERRIDES[@]}"} )
fi

# --- build the app being served ----------------------------------------------
echo "== build ($APP, $PRESET) =="
cmake --build "$BUILD_DIR" --target "$APP"

# --- app -> served dir / port / theme ----------------------------------------
case "$APP" in
  gallery)    WEB_DIR="$BUILD_DIR/web";     PORT=8080; THEME_ARGS=() ;;
  rs-editor)  WEB_DIR="$BUILD_DIR/web-rs";  PORT=8081; THEME_ARGS=(--theme-file "$REPO/plugins/resonance-suppressor/ui/theme-rs.json") ;;
  pitch-fix)  WEB_DIR="$BUILD_DIR/web-pf";  PORT=8082; THEME_ARGS=() ;;
  dynamic-eq) WEB_DIR="$BUILD_DIR/web-deq"; PORT=8083; THEME_ARGS=() ;;
esac
URL="http://127.0.0.1:$PORT/index.html"

if [ "$VERIFY" -eq 1 ]; then
  if [ ! -d "$HERE/playwright/node_modules/playwright" ]; then
    "$HERE/setup.sh" --with-playwright
  fi
  node "$HERE/playwright/verify.js" --app "$APP" --build-dir "$BUILD_DIR" --out "$HERE/artifacts"
  exit 0
fi

if [ "$SERVE" -eq 0 ]; then
  echo
  echo "build complete (--no-serve). wasm output: $WEB_DIR"
  echo "serve it with: ./tools/ui-dev/dev.sh --app $APP$([ "$PRESET" = rel ] && echo ' --rel')"
  exit 0
fi

echo
echo "serving $APP at $URL   (edit source -> auto rebuild + reload; Ctrl-C to stop)"
# exec so Ctrl-C is delivered straight to dev_server.py, which exits cleanly.
exec python3 "$HERE/dev_server.py" \
  --web-dir "$WEB_DIR" --port "$PORT" \
  ${THEME_ARGS[@]+"${THEME_ARGS[@]}"} \
  --watch --cmake-build-dir "$BUILD_DIR" --target "$APP"
