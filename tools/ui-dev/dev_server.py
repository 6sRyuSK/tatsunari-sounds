#!/usr/bin/env python3
"""Factory UI · Visage — daily-development dev server (python3 stdlib only).

Serves the wasm gallery build output plus two source-overlaid files (theme.json,
harness.js) so those can be edited without a rebuild. Sets the correct
`application/wasm` MIME and `Cache-Control: no-store`. Optionally (`--watch`)
rebuilds the gallery when a source file changes and triggers a browser reload
through a long-poll `/events` endpoint.

Examples
--------
    # plain serve of a build:
    python3 dev_server.py --web-dir ../../build-ui-dev-dev/web

    # serve + rebuild-on-change + auto reload (the daily loop):
    python3 dev_server.py --web-dir ../../build-ui-dev-dev/web \\
                          --watch --cmake-build-dir ../../build-ui-dev-dev

Single-threaded visage build => no SharedArrayBuffer => no COOP/COEP needed.
Pass --coi only if you ever build with pthreads.
"""
import argparse
import email.utils
import http.server
import json
import os
import socketserver
import subprocess
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--web-dir", required=True, help="build output dir containing index.html/.js/.wasm")
parser.add_argument("--src-dir", default=HERE, help="source dir for theme.json + harness.js overlay")
parser.add_argument("--port", type=int, default=8080)
parser.add_argument("--watch", action="store_true", help="rebuild the gallery on source change + auto reload")
parser.add_argument("--cmake-build-dir", default=None, help="cmake binary dir to `cmake --build` on change")
parser.add_argument("--target", default="gallery", help="cmake target to rebuild (default: gallery)")
parser.add_argument("--theme-file", default=None,
                    help="path served at /theme.json (default: <src-dir>/theme.json). "
                         "Point at plugins/<slug>/ui/theme-rs.json for a plugin editor.")
parser.add_argument("--coi", action="store_true", help="add COOP/COEP (only needed with pthreads)")
args = parser.parse_args()

# Files edited live (no rebuild) are served from the source dir, overriding the
# build output. Everything else (index.html/.js/.wasm) comes from --web-dir.
OVERLAY = {"/theme.json", "/harness.js"}

# Source trees whose changes trigger a rebuild in --watch mode.
WATCH_ROOTS = [
    os.path.join(REPO, "ui", "visage"),
    os.path.join(HERE, "gallery"),
    os.path.join(HERE, "rs-editor"),
    os.path.join(REPO, "plugins", "resonance-suppressor", "ui"),
    os.path.join(HERE, "shell.html"),
]
WATCH_EXTS = (".h", ".hpp", ".cpp", ".cc", ".txt", ".cmake")

# Build state shared with the /events long-poll.
_cond = threading.Condition()
_version = 0


def _bump_version():
    global _version
    with _cond:
        _version += 1
        _cond.notify_all()


def _iter_source_files():
    for root in WATCH_ROOTS:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, _dirs, files in os.walk(root):
            if "build" in dirpath.split(os.sep):
                continue
            for f in files:
                if f.endswith(WATCH_EXTS):
                    yield os.path.join(dirpath, f)


def _snapshot():
    snap = {}
    for path in _iter_source_files():
        try:
            snap[path] = os.path.getmtime(path)
        except OSError:
            pass
    return snap


def _watch_loop():
    if not args.cmake_build_dir:
        print("[watch] --watch given without --cmake-build-dir: reload disabled", flush=True)
        return
    last = _snapshot()
    print("[watch] watching %d source files" % len(last), flush=True)
    while True:
        time.sleep(0.5)
        cur = _snapshot()
        if cur == last:
            continue
        last = cur
        print("[watch] change detected -> cmake --build --target %s" % args.target, flush=True)
        t0 = time.time()
        rc = subprocess.call(["cmake", "--build", args.cmake_build_dir, "--target", args.target])
        dt = time.time() - t0
        if rc == 0:
            print("[watch] rebuild OK in %.1fs -> reload" % dt, flush=True)
            _bump_version()
        else:
            print("[watch] rebuild FAILED (rc=%d) in %.1fs" % (rc, dt), flush=True)


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".html": "text/html",
        ".json": "application/json",
    }

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=args.web_dir, **kw)

    def log_message(self, *a):
        pass

    # --- /events long-poll: block until the build version advances -----------
    def _handle_events(self):
        since = -1
        if "?" in self.path:
            q = self.path.split("?", 1)[1]
            for part in q.split("&"):
                if part.startswith("since="):
                    try:
                        since = int(part[len("since="):])
                    except ValueError:
                        since = -1
        deadline = time.time() + 25.0
        with _cond:
            while _version == since and time.time() < deadline:
                _cond.wait(timeout=deadline - time.time())
            payload = json.dumps({"version": _version}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    # --- theme.json / harness.js overlay from the source dir -----------------
    def _serve_overlay(self, route):
        # /theme.json may be redirected to an explicit --theme-file (a plugin's
        # theme-rs.json) so the same harness.js hot-reload path themes any editor.
        if route == "/theme.json" and args.theme_file:
            local = args.theme_file
        else:
            local = os.path.join(args.src_dir, route.lstrip("/"))
        if not os.path.isfile(local):
            self.send_error(404, "overlay file not found")
            return
        st = os.stat(local)
        last_mod = email.utils.formatdate(st.st_mtime, usegmt=True)

        # Conditional GET (theme.json hot-reload poll sends If-Modified-Since).
        ims = self.headers.get("If-Modified-Since")
        if ims is not None:
            try:
                if email.utils.parsedate_to_datetime(ims).timestamp() >= int(st.st_mtime):
                    self.send_response(304)
                    self.send_header("Last-Modified", last_mod)
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    return
            except (TypeError, ValueError):
                pass

        with open(local, "rb") as f:
            body = f.read()
        ctype = "application/json" if route.endswith(".json") else "text/javascript"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Last-Modified", last_mod)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        route = self.path.split("?", 1)[0]
        if route == "/events":
            self._handle_events()
            return
        if route == "/favicon.ico":
            # The browser auto-requests this; answer 204 so it is not a noisy 404.
            # (end_headers() adds Cache-Control.)
            self.send_response(204)
            self.end_headers()
            return
        if route in OVERLAY:
            self._serve_overlay(route)
            return
        super().do_GET()

    def end_headers(self):
        # Applies to the plain static (super) path; overlay/events set their own.
        if args.coi:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    if args.watch:
        threading.Thread(target=_watch_loop, daemon=True).start()
    with Server(("127.0.0.1", args.port), Handler) as httpd:
        print("serving %s at http://127.0.0.1:%d (overlay: %s)"
              % (args.web_dir, args.port, args.src_dir), flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            # Clean Ctrl-C for the dev.sh loop (no traceback); the `with` block
            # still closes the listening socket on the way out.
            print("\nstopping dev server", flush=True)


if __name__ == "__main__":
    main()
