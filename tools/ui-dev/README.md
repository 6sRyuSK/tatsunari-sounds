# tools/ui-dev — the Visage UI daily-development loop (Phase P2a/P2b)

The vertical slice of the new Visage-based UI foundation: a widget gallery built
to **WebAssembly**, served locally, driven + screenshotted under headless
Chromium. Edit a widget's C++ (or the theme JSON) → see it in the browser in
seconds. This is a **local dev harness only** — it is not wired into the
repository-root build or CI.

Phase P2b completed the widget set: the gallery now has a **second card** with a
Segmented strip, IconButtons, a ValueSetting, a LinkSlider, a PresetSelectorView
(over our own Dropdown overlay) and an animated SpectrumView fed by a
deterministic synthetic generator, plus a runtime **font switch** across three
candidate typefaces (Quicksand / Nunito / M PLUS Rounded 1c).

```
tools/ui-dev/
  gallery/            visage app: GalleryFrame + main + the JS<->WASM Bridge
  rs-editor/          Phase P3 app: main + RsBridge + SyntheticFeed + Mocks (the RS
                      editor itself lives JUCE-free in plugins/resonance-suppressor/ui/)
  shell.html          emscripten shell page (baked into index.html at link)
  harness.js          page JS: bridge wrappers (window.ui + window.rs), theme hot-reload, live reload
  theme.json          live-editable theme (a copy of ui/visage/theme/factory-default.json)
  dev_server.py       static server (+ --watch rebuild + /events reload + --theme-file)
  playwright/         drive.js (reusable driver) + smoke.js (gallery) + rs.spec.js (rs-editor)
  CMakeLists.txt      STANDALONE project; add_subdirectory(../../ui/visage); targets: gallery, rs-editor
  CMakePresets.json   `dev` (-O0 fast link) and `rel` (-O2) configs — both build gallery + rs-editor
  setup.sh / dev.sh   one-command bootstrap + daily loop (macOS/Linux); *.ps1 = Windows (UNTESTED)
```

## Quick start (one command)

First run — bootstrap the toolchain. This installs the **pinned emsdk 6.0.3** into
`tools/ui-dev/.emsdk` and checks the host build tools (it never installs system
packages: a missing tool prints the exact `brew` / `apt-get` line and exits):

```bash
./tools/ui-dev/setup.sh                 # add --with-playwright for the headless-verify deps
```

Then the daily loop — activate emsdk, configure (first run only), build, and serve
with live rebuild + browser auto-reload:

```bash
./tools/ui-dev/dev.sh                   # rs-editor       -> http://127.0.0.1:8081
./tools/ui-dev/dev.sh --gallery         # widget gallery  -> http://127.0.0.1:8080
./tools/ui-dev/dev.sh --rel             # -O2 preset (small wasm, slower link)
./tools/ui-dev/dev.sh --no-serve        # configure + build only, no server
```

`dev.sh` runs `setup.sh` for you when `.emsdk` is missing, so a bare
`./tools/ui-dev/dev.sh` on a fresh checkout is enough. **Ctrl-C** stops the server
cleanly. Both scripts are self-contained bash (macOS + Linux), work from any cwd,
and pass the sandbox override vars (`FACTORY_FREETYPE_MIRROR_DIR`,
`FETCHCONTENT_SOURCE_DIR_VISAGE`) through to the CMake configure **only when set**,
so the same script works in-container and on a normal machine. Everything below is
what these two scripts automate.

**Windows:** `setup.ps1` / `dev.ps1` mirror the bash scripts (winget/choco hints,
emsdk via `emsdk.bat`) but are **UNTESTED** — authored on Linux; verify on a real
Windows box before relying on them.

## rs-editor (Phase P3) — the resonance-suppressor editor

The flagship visual port: the full RS editor, composed from `factory_ui_visage`
widgets + the RS-specific views (`RsSuppressionCurveView`, `RsNodePanel`,
`RsPillCell`), all **JUCE-free**, bound to a real `factory_params::ParamStore`
(the actual 64-param `buildRsParams()` table), a synthetic `RsFeed` (deterministic
pre/post/reduction spectra; the reduction curtain deepens with the Depth param;
freezable) and mock preset / A-B models. The editor + its RS theme overlay live in
`plugins/resonance-suppressor/ui/`; only the app shell (`main` + `RsBridge` +
`SyntheticFeed` + `Mocks`) is here.

`./tools/ui-dev/dev.sh` performs exactly the build + serve below (rs-editor on
:8081 with the RS theme overlay); the manual form is:

```bash
cmake --build --preset dev            # builds gallery + rs-editor -> build/dev/{web,web-rs}
# serve rs-editor; theme-rs.json is served at /theme.json so the shared harness.js
# hot-reloads the RS chunky-knob overlay (the "rs" extras block is applied at load).
python3 dev_server.py --web-dir build/dev/web-rs --port 8081 \
        --theme-file ../../plugins/resonance-suppressor/ui/theme-rs.json \
        --watch --cmake-build-dir build/dev --target rs-editor
# verify (30 asserts + 4 screenshots rs-default/busy/min/max):
cd playwright && PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers node rs.spec.js http://127.0.0.1:8081/index.html .
```

The rs-editor canvas is fixed at the **max** resize size (1320×922) and the editor
frame renders into its top-left sub-rect. The editor ALWAYS lays out at the fixed
1069×747 design; `rs.setSize(w,h)` uniform-zooms it by setting the editor frame's
`dpiScale = h/747` (the harness analogue of the CLAP shell's window `setDpiScale`,
since the native window can't be resized under Emscripten — `computeWindowBounds`
isn't linkable) so it renders design-scaled into a `(w,h)` native sub-rect at
471×329 (min) / 706×493 (default) / 1069×747 (design) / 1320×922 (max) — fixed
1069:747 aspect — and the driver clips the screenshot to `(w,h)`. At the design
size dpi==1, so the clicking tests (which run there) see window px == logical px.

The design system it exercises lives in **`ui/visage/`** (`factory_ui_visage`):
`Theme` (+ JSON parser), `Fonts` (3-family runtime switch), `Chrome`, `Knob`,
`PillToggle`, and the P2b set — `Segmented`, `IconButton`/`Icons`, `ValueSetting`,
`LinkSlider`, `PresetSelectorView`, `Dropdown`, `SpectrumModel` + `SpectrumView`.
See that directory for the widgets; this README is just the loop.

**Visage mouse gotcha (load-bearing).** In a widget's `mouseDown`/`mouseDrag`, use
`e.position` (the click's frame-local coordinate = `windowPosition − positionInWindow`)
for hit-testing. `e.relativePosition()` is a **movement delta** since the last
event (for relative-drag mode), NOT a hit point — using it silently sends every
hit to `(0,0)`. Also: a single click is `repeatClickCount() == 1` (double-click is
`2`), so gate double-click actions on `>= 2`. The `ui_last_mouse_x/y` bridge calls
report where visage actually delivered the last click, for coordinate calibration.

## Prerequisites (pinned — see docs/migration/s1-wasm-loop.md)

`setup.sh` installs the emsdk pin for you (into `tools/ui-dev/.emsdk`); this table
is the reference for every pinned version the harness depends on.

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

## Under the hood — the manual loop (what `dev.sh` automates)

Everything in this section is run for you by `setup.sh` + `dev.sh` above; reach for
it when you want to drive a single step by hand or understand what the scripts do.

### Configure + build

With `emsdk_env.sh` sourced (`$EMSDK` set — `dev.sh` sources
`.emsdk/emsdk_env.sh`), presets do the rest:

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

### Serve + edit

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

`smoke.js` asserts the P2a foundation (bridge lists the 9 params, store
round-trip, a knob **drag** moves the bound param, gallery non-blank, **theme hot
reload → pixel** with a malformed theme rejected) AND the P2b widget set through
real mouse events: **Segmented** click → Choice param changes; **LinkSlider** drag
→ Float param changes; **PresetSelectorView** `<`/`>` step (skipping the non-
steppable "Save As…"); **ValueSetting** → Dropdown open + row click → Choice param
changes; and the **SpectrumView** region is non-blank, **changes** between two
synthetic frames, then **freezes deterministically** (two frozen frames identical).
It writes `gallery.png`, `gallery2.png` (extended, frozen), `dropdown.png`,
`theme-edit.png`, and `smoke_result.json`.

**Native tests** (visage-free/JUCE-free, host compiler — like the theme test):
```bash
# theme model + JSON parser round-trip (pass the document path — the built-in
# "../theme/factory-default.json" fallback assumes cwd == ui/visage/tests)
c++ -std=c++17 -I ../../ui/visage/include \
    ../../ui/visage/tests/theme_roundtrip_test.cpp ../../ui/visage/src/Theme.cpp -o t \
  && ./t ../../ui/visage/theme/factory-default.json
# SpectrumModel: on-bin sinusoid -> peak bin + dB (independent Hann-gain oracle),
# across the full 44.1–192 kHz rate matrix
c++ -std=c++17 -I ../../ui/visage/include -I ../../core/include \
    ../../ui/visage/tests/spectrum_model_test.cpp ../../ui/visage/src/SpectrumModel.cpp -o s && ./s
```
(Both are also CTest cases on any native — non-Emscripten — configure that
includes RS: `factory_ui_visage_theme` and `factory_ui_visage_spectrum_<rate>`
across the standard rate matrix, registered by `ui/visage/CMakeLists.txt`; the
RS overlay's `resonance_suppressor_theme_roundtrip` registers from the RS
plugin CMakeLists. The wasm gallery build still skips them.)

**Font comparison.** `font_compare.js` renders the identical frozen gallery in each
candidate typeface (`ui.setFont`) → `font-quicksand.png` / `font-nunito.png` /
`font-mplus.png`, then restores the Quicksand default. The three faces are embedded
from `ui/visage/fonts/` (`*-OFL.txt` licences). **Quicksand is the confirmed default
typeface** (human sign-off 2026-07-17); the runtime switch (`ui.setFont` / Nunito /
M PLUS Rounded 1c) is retained for future experiments only — changing the shipped
default is a one-place change in `src/Fonts.cpp` and needs a fresh human decision.

Font provenance (all OFL, fetched via a sparse checkout of `google/fonts`):
```bash
# Nunito: static 400/700 instanced from the variable font (full Latin, ~132 KB/face)
fonttools varLib.instancer 'ofl/nunito/Nunito[wght].ttf' wght=400 -o Nunito-Regular.ttf
fonttools varLib.instancer 'ofl/nunito/Nunito[wght].ttf' wght=700 -o Nunito-Bold.ttf
# M PLUS Rounded 1c: the full face is ~3.4 MB (CJK); subset to Latin -> ~43 KB/face
UNI="U+0020-007E,U+00A0-00FF,U+2013,U+2014,U+2018,U+2019,U+201C,U+201D,U+2026,U+2190,U+2192,U+25B2,U+25BC,U+2212"
pyftsubset ofl/mplusrounded1c/MPLUSRounded1c-Regular.ttf --unicodes="$UNI" --layout-features='*' --output-file=MPLUSRounded1c-Regular.ttf
pyftsubset ofl/mplusrounded1c/MPLUSRounded1c-Bold.ttf    --unicodes="$UNI" --layout-features='*' --output-file=MPLUSRounded1c-Bold.ttf
```

## JS ↔ WASM bridge (window.ui in harness.js; C in gallery/Bridge.cpp)

| Call | Meaning |
|---|---|
| `ui.list()` | JSON of every param (index, id, name, type, range, default, live value, unit) |
| `ui.get(id)` / `ui.set(id, real)` | read / **host-drive** a param (`setFromHost`) by id |
| `ui.freeze(bool)` | stop the SpectrumView animation for deterministic capture; freezing also injects a fixed synthetic frame so the held image is stable |
| `ui.reloadTheme(jsonText)` | re-apply a theme at runtime → returns false on malformed input (`ui.lastError()`) |
| `ui.accent()` | current theme accent `0xAARRGGBB` (for verifying a reload landed) |
| `ui.widgetX(id)` / `ui.widgetY(id)` | bound widget centre in window px (aim mouse events) |
| `ui.widgetRect(key)` | rect `{x,y,w,h}` (window px) of a control by param id or a special name (`"preset"` / `"spectrum"` / `"valueSetting"`); `null` if unknown |
| `ui.setFont(name)` / `ui.font()` | switch / read the active typeface (`"quicksand"` \| `"nunito"` \| `"mplus"`); default stays Quicksand |
| `ui.feedSpectrum(phase)` | inject a **fixed** synthetic spectrum frame and converge the model (deterministic frozen capture) |
| `ui.openDropdown(which)` | open a control's Dropdown (`0` = preset selector, `1` = value setting) |
| `ui.dropdownOpen()` / `ui.dropdownCount()` | overlay state + number of item rows |
| `ui.dropdownX(i)` / `ui.dropdownRowY(i)` | window-px centre of an open dropdown's item row `i` (aim a row click) |
| `ui.presetIndex()` | current PresetSelectorView item-row index (verify `<`/`>` onChange) |

The store is a real `factory_params::ParamStore` with no host draining its
write queue (a mock host): UI edits go through `beginGesture`/`setFromUi`/
`endGesture`, host edits through `setFromHost`.

**rs-editor extras** (`window.rs` in harness.js; C in `rs-editor/RsBridge.cpp`).
The rs-editor also exports the core `ui.*` calls above (`list`/`get`/`set`/
`freeze`/`reloadTheme`/`accent`/`widgetRect`/`setFont`), plus:

| Call | Meaning |
|---|---|
| `rs.selectNode(i)` / `rs.selectedNode()` | select a curve node (opens the NodePanel) / read the selection |
| `rs.nodeX(i)` / `rs.nodeY(i)` | node handle centre in window px (aim a drag); `-1` for a hidden band |
| `rs.listenNode()` | the feed's live Listen (solo) target (`-1` = off) |
| `rs.displaySmooth()` | the analyser display-smoothing ms the editor pushed (`RsFeed::setDisplaySmoothMs`) |
| `rs.abSlot()` / `rs.setAb(s)` / `rs.copyAb()` | A/B compare: active slot / switch / copy active→other |
| `rs.presetIndex()` / `rs.presetLoad(i)` | preset selection / load (resets + clears undo) |
| `rs.uiEdit(id, v)` | simulate a UI gesture (begin/setFromUi/end + pump) — deterministic undo capture |
| `rs.undo()` / `rs.redo()` / `rs.canUndo()` / `rs.canRedo()` | undo timeline (`factory_params::UndoStack`) |
| `rs.setClock(s)` / `rs.pump()` | inject a fixed clock (undo coalescing) / drain the gesture queue |
| `rs.openDropdown(w)` | open a value-setting/preset dropdown (`0`=quality, `1`=channel, `2`=preset) |
| `rs.dropdownOpen()` / `rs.dropdownCount()` / `rs.dropdownX(i)` / `rs.dropdownRowY(i)` | shared-dropdown state + row positions |
| `rs.plotRect()` | analyser plot rect `{x,y,w,h}` (window px) |
| `rs.setSize(w,h)` | re-lay-out the editor at `(w,h)` (min/max screenshots) |

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
