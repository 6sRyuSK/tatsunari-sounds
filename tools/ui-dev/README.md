# tools/ui-dev — the Visage UI daily-development loop (Phase P2a)

The vertical slice of the new Visage-based UI foundation: a widget gallery built
to **WebAssembly**, served locally, driven + screenshotted under headless
Chromium. Edit a widget's C++ (or the theme JSON) → see it in the browser in
seconds. This is a **local dev harness only** — it is not wired into the
repository-root build or CI.

```
tools/ui-dev/
  gallery/            visage app: GalleryFrame + main + the JS<->WASM Bridge
  shell.html          emscripten shell page (baked into index.html at link)
  harness.js          page JS: bridge wrappers, theme hot-reload, live rebuild reload
  theme.json          live-editable theme (a copy of ui/visage/theme/factory-default.json)
  dev_server.py       static server (+ --watch rebuild + /events reload)
  playwright/         drive.js (reusable driver) + smoke.js (scripted verification)
  CMakeLists.txt      STANDALONE project; add_subdirectory(../../ui/visage)
  CMakePresets.json   `dev` (-O0 fast link) and `rel` (-O2) configs
```

The design system it exercises lives in **`ui/visage/`** (`factory_ui_visage`):
`Theme` (+ JSON parser), `Fonts`, `Chrome`, `Knob`, `PillToggle`. See that
directory for the widgets; this README is just the loop.

## Prerequisites (pinned — see docs/migration/s1-wasm-loop.md)

| Tool | Pin | Where |
|---|---|---|
| emsdk / emscripten | **6.0.3** | `git clone https://github.com/emscripten-core/emsdk && ./emsdk install 6.0.3 && ./emsdk activate 6.0.3 && source ./emsdk_env.sh` |
| visage | commit `20de59464243447816d142e9d38e9723d068f755` | fetched by `ui/visage/CMakeLists.txt` (FetchContent) |
| FreeType | tag `VER-2-14-1` | fetched by visage; **GitHub mirror** needed when `gitlab.freedesktop.org` is proxy-blocked (see below) |
| Node / Playwright | Playwright `1.55.0`, pngjs `7.0.0`, Chromium at `/opt/pw-browsers` | `PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers`; never `playwright install` |

Two sandbox workarounds are wired into `ui/visage/CMakeLists.txt` so you don't
have to think about them:
- **`EMSCRIPTEN=1`** is auto-defined for the visage build (bgfx/nvtt `posh.h`
  needs the legacy macro modern emcc dropped).
- **`FACTORY_FREETYPE_MIRROR_DIR`** cache var: point it at a local FreeType
  checkout (GitHub mirror, tag `VER-2-14-1`) and it is handed to
  `FETCHCONTENT_SOURCE_DIR_FREETYPE`. Leave it empty on machines that can reach
  `gitlab.freedesktop.org` directly.

## Configure + build

With `emsdk_env.sh` sourced (`$EMSDK` set), presets do the rest:

```bash
cd tools/ui-dev
cmake --preset dev            # or: cmake --preset rel
cmake --build --preset dev    # -> build/dev/web/index.{html,js,wasm}
```

Or explicit (what the in-container verification uses — also lets you reuse an
existing visage checkout via `FETCHCONTENT_SOURCE_DIR_VISAGE`):

```bash
emcmake cmake -S tools/ui-dev -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS=-O0 -DCMAKE_CXX_FLAGS=-O0 \
  -DFACTORY_FREETYPE_MIRROR_DIR=/path/to/freetype-mirror
cmake --build build/dev --target gallery
```

**Two configs.** `dev` = Debug/`-O0`: ~5–7 s incremental rebuild (fast wasm link),
big wasm (~30 MB, irrelevant locally) — the daily loop. `rel` = Release/`-O2`:
small wasm (~4–5 MB), slower link — for publish/CI.

## Serve + edit

```bash
python3 dev_server.py --web-dir build/dev/web            # plain serve
# the daily loop — rebuild on source change + auto browser reload:
python3 dev_server.py --web-dir build/dev/web --watch --cmake-build-dir build/dev
```

Then open `http://127.0.0.1:8080/index.html`.

- **Edit a widget** (`ui/visage/src/*.cpp`, `gallery/*.cpp`): with `--watch`, the
  server rebuilds and the page reloads itself (via the long-poll `/events`
  endpoint). Without `--watch`, rebuild manually and refresh.
- **Edit the theme** (`tools/ui-dev/theme.json`): served from source (no rebuild).
  `harness.js` polls it every ~300 ms (`If-Modified-Since`) and re-applies it live
  via `ui_reload_theme`. `theme.json` and `harness.js` are both overlaid from
  source, so tweaking colours/geometry or the page JS never needs a rebuild.

## Verify (headless Chromium)

```bash
cd playwright
PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers node smoke.js http://127.0.0.1:8080/index.html .
# quick non-blank-only check: node drive.js <url> out.png
```

`smoke.js` asserts: bridge lists the params, store round-trip (`set depth=55` →
readback 55), a simulated knob **drag** moves the bound store parameter, the
gallery renders non-blank, and **theme hot reload → pixel** (accent changed via
`ui_reload_theme`, no rebuild) with a malformed theme rejected. It writes
`gallery.png` + `theme-edit.png` + `smoke_result.json`.

## JS ↔ WASM bridge (window.ui in harness.js; C in gallery/Bridge.cpp)

| Call | Meaning |
|---|---|
| `ui.list()` | JSON of every param (index, id, name, type, range, default, live value, unit) |
| `ui.get(id)` / `ui.set(id, real)` | read / **host-drive** a param (`setFromHost`) by id |
| `ui.freeze(bool)` | stop continuous animation for deterministic capture (P2a widgets are static; reserved for animated P2b widgets) |
| `ui.reloadTheme(jsonText)` | re-apply a theme at runtime → returns false on malformed input (`ui.lastError()`) |
| `ui.accent()` | current theme accent `0xAARRGGBB` (for verifying a reload landed) |
| `ui.widgetX(id)` / `ui.widgetY(id)` | bound widget centre in window px (aim mouse events) |

The store is a real `factory_params::ParamStore` with no host draining its
write queue (a mock host): UI edits go through `beginGesture`/`setFromUi`/
`endGesture`, host edits through `setFromHost`.

## Measured loop timings (in-container, SwiftShader WebGL2)

| | dev (`-O0`) | rel (`-O2`) |
|---|---|---|
| cold configure | ~42 s | ~30 s |
| cold build (gallery) | ~39 s | ~57 s (wasm ~3 MB vs ~30 MB) |
| incremental — touch `ui/visage/src/Knob.cpp` | **~7.4 s** | — |
| incremental — touch `gallery/GalleryFrame.cpp` | **~5.5 s** | — |
| page load → bridge ready | ~1.4 s | — |
| theme.json edit → pixel (auto-poll) | **~0.39 s** | — |
| `ui_reload_theme` bridge call → applied | **<1 ms** | — |

Recorded pins + gotchas: `docs/migration/s1-wasm-loop.md` (WASM loop) and
`docs/migration/s2-clap-first.md` (CLAP-first). Headless Chromium flags live in
`playwright/drive.js`; the load-bearing one is `--enable-unsafe-swiftshader`.
