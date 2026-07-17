> Migration spike **S1** (WASM UI loop), captured 2026-07-17 during Phase P2 planning. Preserved verbatim in-repo for its toolchain pins + gotchas; the Phase P2a harness in tools/ui-dev/ is built on it.

# Spike S1 — Visage → Emscripten/WASM → headless-Chromium screenshot loop

**Verdict: PROVEN.** The full daily UI loop works in this Linux container: a
visually-rich Visage example (Showcase) compiles to WASM with the pinned
emsdk toolchain, is served locally, renders under headless Chromium via
SwiftShader WebGL2, and is screenshotted + asserted non-blank by Playwright.
Editing one source file and re-running produces an updated, verified screenshot
in ~25–36 s (Release), dominated by the emscripten wasm link step.

All work is under
`/tmp/claude-0/-home-user-tatsunari-sounds/8535a60f-a210-5aa4-a1da-508cbfe91400/scratchpad/s1-wasm/`.
The repo `/home/user/tatsunari-sounds` was not touched.

---

## 1. Toolchain pins (record these)

| Component | Pin | Notes |
|---|---|---|
| **emsdk / emscripten** | **`6.0.3`** | `emcc 6.0.3 (283e2d130132859fde6a4e4c87fd254b38127651)`; emscripten-releases commit `9074aa513b501925adb1361e208932ad32a29a5f`; bundles its own node `22.16.0` |
| **Visage** | **`20de59464243447816d142e9d38e9723d068f755`** | HEAD of `main`, 2026-06-15 ("Fixed implicit line to in SVG when move has 4 values") |
| bgfx.cmake (transitive) | tag `v1.129.8958-499` → commit `a952acef35b431a39c790d39736867c515f32aff` | fetched by visage_graphics at configure |
| FreeType (transitive) | tag `VER-2-14-1` → commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | **see Obstacle #1** — pulled from the GitHub mirror, not gitlab.freedesktop.org |
| Host tooling | cmake `3.28.3`, ninja `1.11.1`, host node `22.22.2` | preinstalled |
| Browser / driver | Chromium `141.0.7390.37` (`/opt/pw-browsers`), Playwright `1.55.0`, pngjs `7.0.0` | Playwright driven via `executablePath`, so its version need not match the browser build |

emsdk install + activate was ~47 s (one-time, ~1.5 GB).

---

## 2. Exact build commands that worked

```bash
# 0. toolchain (once); source env in every build shell (state does not persist)
git clone https://github.com/emscripten-core/emsdk.git
./emsdk/emsdk install 6.0.3 && ./emsdk/emsdk activate 6.0.3
source ./emsdk/emsdk_env.sh                 # puts emcc/emcmake 6.0.3 on PATH

# 1. sources
git clone https://github.com/VitalAudio/visage.git          # pin: 20de594...
#    FreeType mirror (see Obstacle #1) — gitlab.freedesktop.org is proxy-blocked
git clone --branch VER-2-14-1 --depth 1 \
    https://github.com/freetype/freetype.git freetype-src-mirror

# 2. configure (emcmake sets the Emscripten toolchain file)
cd visage
emcmake cmake -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVISAGE_BUILD_TESTS=OFF \
  -DFETCHCONTENT_SOURCE_DIR_FREETYPE="$PWD/../freetype-src-mirror" \
  -DCMAKE_C_FLAGS="-DEMSCRIPTEN=1" \
  -DCMAKE_CXX_FLAGS="-DEMSCRIPTEN=1"          # see Obstacle #2 (nvtt/posh.h)

# 3. build ONE example (target name = Example<Dir>)
cmake --build build-wasm --target ExampleShowcase
# output (self-contained, no .data sidecar):
#   build-wasm/examples/builds/Showcase/{index.html, index.js, index.wasm}
```

Notes:
- `emmake` is not needed once `emcmake` has set the toolchain; plain
  `cmake --build` uses emcc.
- `-DVISAGE_BUILD_TESTS=OFF` avoids fetching Catch2 and building test exes we
  can't run headless anyway. `ClapPlugin` and the CLAP SDK fetch are auto-skipped
  under Emscripten.
- Output sizes: `index.wasm` 4.5 MB (Release, all fonts/shaders/images embedded),
  `index.js` 172 KB, `index.html` 3.4 KB.

---

## 3. Serving requirements

**No special headers required.** This build is **single-threaded**
(`BGFX_CONFIG_MULTITHREADED=0` on Emscripten; no `-pthread` in the example link
flags), so it does **not** use `SharedArrayBuffer` and does **not** need
cross-origin isolation (COOP/COEP). `python3 -m http.server` is sufficient.

The one nicety worth having: serve `.wasm` as `application/wasm` so the browser
can use `WebAssembly.instantiateStreaming` (otherwise emscripten prints a warning
and falls back to a slower array-buffer instantiate — still works). The workspace
ships `serve.py` (threaded, sets `application/wasm`, `Cache-Control: no-store`,
and has an opt-in `--coi` flag to add COOP/COEP **if** we ever build with
pthreads). The only 404 observed is the browser's automatic `/favicon.ico` —
harmless.

---

## 4. Working Chromium flag set (WebGL2 headless as root)

Playwright `chromium.launch({ executablePath, headless:true, args:[…] })` with:

```
--no-sandbox                 # required: running as root
--disable-dev-shm-usage      # container /dev/shm is small
--use-gl=angle
--use-angle=swiftshader      # software GL via ANGLE→SwiftShader (no GPU here)
--enable-unsafe-swiftshader  # REQUIRED on modern Chromium to allow SwiftShader as a WebGL backend
--ignore-gpu-blocklist
--enable-webgl
--disable-gpu-sandbox
```

Result inside the page:
`WebGL 2.0 (OpenGL ES 3.0 Chromium)`, renderer
`ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)`.
`--enable-unsafe-swiftshader` is the load-bearing flag — without it Chromium 141
refuses SwiftShader for WebGL and the canvas comes up blank.

Playwright package is driven purely via `executablePath` pointing at
`/opt/pw-browsers/chromium-1194/chrome-linux/chrome`, so we never run
`playwright install` and the npm version (1.55) need not exactly match the
browser revision.

---

## 5. Timings

**Cold build (clean `build-wasm`, Release):**

| Phase | Wall time |
|---|---|
| configure (incl. bgfx + freetype fetch/populate) | **43 s** |
| compile + link `ExampleShowcase` (visage core + bgfx + bimg + freetype + example) | **61 s** |
| **cold total (configure → linked wasm)** | **104 s** |

(emsdk install, one-time, +47 s.)

**Incremental loop — edit one file (`ShapeColor` constant in
`examples_frame.cpp`), rebuild, re-screenshot (Release):**

| Iter | rebuild (ninja: 1 TU compile + wasm link) | total edit→new screenshot |
|---|---|---|
| 1 (→red)   | 22.9 s | 33.7 s |
| 2 (→blue)  | 28.5 s | 35.8 s |
| 3 (→amber) | 18.3 s | 25.3 s |
| **median** | **~23 s** | **~34 s** |

Every iteration recompiled exactly the one changed translation unit and relinked;
the `index.wasm` sha256 changed each time and the edit was visible in the
screenshot (the "Shapes" panel changed colour), so the loop propagates
source → pixels end-to-end. The `total − rebuild` delta (~7–11 s) is the driver:
Node + browser launch + page load + a fixed 2.5 s "let it animate" wait +
screenshot + pixel analysis.

The rebuild is ~90% **emscripten link** (Binaryen/wasm-opt + wasm-ld), not the
single-file C++ compile. Release optimises the wasm on every link, which is why
~20 s dominates a one-line change.

**Dev-config (`-O0` / Debug) loop — supplementary, to size the recommendation:**

| Metric | `-O0`/Debug | Release (`-O2`) |
|---|---|---|
| incremental rebuild (1 TU + link) | **6.8 s** | ~23 s |
| cold build | 84 s | 61 s |
| `index.wasm` size | 36 MB | 4.5 MB |

The dev config makes the **incremental link ~3.4× faster** (6.8 s vs ~23 s) — the
whole point, since the link is the loop's cost. Trade-off: `-O0` + DWARF bloats
the wasm to 36 MB (slower one-time page load, irrelevant for a local loop) and
the *cold* build is slower because emitting/linking debug info isn't free.
Net dev loop ≈ **6.8 s rebuild + ~7–11 s driver ≈ 14–18 s edit→screenshot**,
roughly half the Release loop. (Dropping `-g` while keeping `-O0` would shrink the
36 MB and trim the link further; not measured.)

---

## 6. Console output seen

- **0 page errors, 0 uncaught exceptions, no crash.** Canvas non-blank assertion
  passed (860×602, 353 distinct quantised colours, luma stddev 36.8; blank would
  be 1 colour / 0 stddev).
- ~140 **warnings**, all the same benign shape:
  `WebGL: INVALID_ENUM: getInternalformatParameter: invalid target / invalid
  internalformat` (a few `… when EXT_texture_norm16 is not enabled`). This is
  **bgfx probing GPU texture-format capabilities** at init; SwiftShader's WebGL2
  doesn't expose some formats, so the probes return `INVALID_ENUM`. bgfx treats
  these as "format unsupported" and carries on — cosmetic, not a fault. On real
  GPU hardware most of these disappear.
- 1 "error": `Failed to load resource: 404` = the browser's automatic
  `/favicon.ico` request. Harmless.

---

## 7. Obstacles hit + fixes

**#1 — `gitlab.freedesktop.org` is blocked by the egress proxy (403 CONNECT).**
Visage fetches FreeType from `https://gitlab.freedesktop.org/freetype/freetype.git`
via `FetchContent`; the proxy returns `CONNECT tunnel failed, response 403`
(host not on the allowlist). GitHub *is* allowed (bgfx fetched fine).
_Fix:_ clone FreeType at the identical tag from the **GitHub mirror**
(`github.com/freetype/freetype`, tag `VER-2-14-1`, same commit `526ec5c…`) and
point CMake at it with `-DFETCHCONTENT_SOURCE_DIR_FREETYPE=…`. This is CMake's
own override mechanism — no repo edit, no proxy bypass, no TLS disable.
_Implication for our harness:_ we must vendor/pin FreeType from an allowed
source; the raw gitlab URL will fail in CI behind the same policy.

**#2 — bgfx `bimg_encode` (nvtt) fails to compile for wasm: "POSH cannot
determine target CPU".** `VisageGraphics` hard-links `bimg_encode` (used by
visage's image/screenshot-save path). Its bundled NVIDIA-texture-tools code has a
portability header `nvcore/posh.h` whose Emscripten branch is gated on the **bare
`EMSCRIPTEN`** macro (CPU detect, endianness, 64-bit-int selection). Modern emcc
only defines `__EMSCRIPTEN__`, so CPU detection falls through to
`#error POSH cannot determine target CPU`, cascading into dozens of downstream
errors. This exact (visage HEAD × bgfx `v1.129.8958-499` × emsdk 6.0.3) combo hits
it. _Fix:_ compile with `-DEMSCRIPTEN=1` (restores the legacy macro the vendored
nvtt was written against). A one-line edit to `posh.h` would also work but the
global define is reproducible and touches no fetched source.
_Implication for our harness:_ bake `EMSCRIPTEN=1` into the wasm toolchain/preset,
or (cleaner long-term) build visage with image-encoding disabled if visage exposes
such a switch, since a browser UI never needs runtime BC6H/BC7 texture
compression.

No other blockers. Two workarounds total, both first-try; neither needed a second
attempt.

---

## 8. Recommendation — shape of `tools/ui-dev/`

The loop is real and fast enough to build a daily harness on. Concretely:

**A. Toolchain, pinned & reproducible.**
- Vendor the three pins above. Provide a bootstrap that installs emsdk `6.0.3`
  and `source`s `emsdk_env.sh`.
- Ship a CMake **preset** for the wasm dev target carrying the two fixes:
  `FETCHCONTENT_SOURCE_DIR_FREETYPE` (or a pinned mirror) **and**
  `EMSCRIPTEN=1`. Configure once per checkout.

**B. Dev server.** A tiny static server (the `serve.py` here is a fine seed):
`application/wasm` MIME, `no-store`, single-threaded is fine (no COOP/COEP).
Keep the `--coi` escape hatch documented in case we ever enable pthreads.

**C. Rebuild-watch.** Watch our UI sources → on change run
`cmake --build <dir> --target <example>` (ninja already does minimal-rebuild:
1 TU + link). **Use a dedicated `-O0`/Debug wasm config for the loop**, not
Release — the ~20 s Release link is almost all wasm-opt and is the whole cost of a
one-line change. Reserve Release for CI/publish. Trigger a browser reload via a
websocket/livereload ping after the link finishes (the page has no HMR; a full
reload is correct for a fresh wasm).

**D. Screenshot/verify harness.** Reuse `drive.js` as the reference:
Playwright via `executablePath` + the flag set in §4; wait for the shell's
`canvas.opacity==1` readiness signal; capture full-page (deliverable) **and**
canvas-only (assertion); assert non-blank via distinct-colour + luma-variance on
the canvas region only (the page's dotted background would otherwise fake
variance). Note: **the Showcase animates continuously**, so a screenshot pixel-diff
is not a stable "did it change" signal — verify propagation via the wasm hash /
ninja rebuild, and use the screenshot for *rendered-correctly / not-blank* +
human/vision review of taste.

**E. JS ↔ WASM bridge hooks.** The example link already exports
`ccall/cwrap/UTF8ToString` and `EXPORTED_FUNCTIONS=['_main','_pasteCallback']`
via `--bind` (embind). For our harness we'll want to add a couple of exported C
functions (e.g. set-parameter, set-viewport-size, force-redraw, drive a specific
widget state) and call them from Playwright with `page.evaluate` → `Module.ccall`,
so screenshots can be taken in deterministic, non-animated states. The shell HTML
(`emscripten_template.html`) is overridable per target via `--shell-file`; we
should fork a minimal shell that exposes a tiny ready-promise and a
state-injection entry point instead of relying on the opacity CSS transition.

**Visage-specific things to know before we build widgets on it:**
- **Draw/frame model.** App is `visage::ApplicationWindow`; you set
  `app.onDraw() = [](visage::Canvas&){…}` and every `visage::Frame` overrides
  `draw(Canvas&)`. Rendering is **dirty-region / on-demand**: a frame only
  repaints when something calls `redraw()`. Animated frames call `redraw()` at the
  end of their `draw()` to keep going (Showcase's bars/shapes/lines do this). Our
  widgets get animation "for free" by calling `redraw()`; static widgets cost
  nothing when idle — good for CPU and for deterministic screenshots (a widget
  that doesn't animate holds still).
- **Colours/theming.** `VISAGE_THEME_COLOR(Name, 0xAARRGGBB)` registers a themed
  colour id; `Palette::initWithDefaults()` seeds the live palette from those
  defaults, so a plain constant edit propagates to pixels (verified: editing
  `ShapeColor` recoloured the Shapes panel). Colours are `0xAARRGGBB`.
- **Fonts & assets are embedded at build time, not loaded at runtime.** The
  `add_embedded_resources` / `visage_embed_shaders` CMake helpers turn `.ttf` /
  `.svg` / `.png` / compiled shaders into C++ byte arrays under namespaces
  (`resources::fonts::Lato_Regular_ttf`, `visage::fonts::…`). That's why the wasm
  is self-contained with **no `.data` sidecar**. A `visage::Font` is constructed
  from an embedded byte array + pixel size. Our widget library should embed its
  own fonts/icons the same way (our own resource target), not fetch them.
- **Shaders.** bgfx `.sc` shaders are cross-compiled at build time by the
  **prebuilt `visage_graphics/bin/linux/shaderc`** (x86-64 ELF, ran fine here) to
  **ESSL `100_es`** for the Emscripten/WebGL target, then embedded. So our WebGL
  runtime uses GLSL-ES 1.00 shaders; any custom shader we add flows through the
  same `visage_embed_shaders` path and needs the platform `asm.js`/`100_es`
  profile.
- **Backend.** Rendering is bgfx→WebGL2 (single-threaded on wasm). Expect the
  benign `getInternalformatParameter` capability-probe warnings under SwiftShader
  in CI; they are not errors and shouldn't gate the screenshot check.

---

## 9. Deliverables in this workspace

- `screenshot.png` — cold Showcase render (light-green shapes), non-blank verified.
- `screenshot_cold.png` — identical backup of the above.
- `screenshot_iter1..3.png` — iteration renders (red / blue / amber shapes) proving
  edit→pixel propagation.
- `drive.js` — the final Playwright driver (launch flags, readiness wait,
  screenshot, pngjs non-blank assertion, console capture).
- `serve.py` — the static dev server.
- `build_cold.sh`, `iterate.sh`, `dev_o0.sh`, `env.sh` — the exact scripts used.
- `drive_result_*.json`, `*.log`, `iterate_results.txt` — raw evidence.
- `visage/`, `emsdk/`, `freetype-src-mirror/` — the pinned checkouts.
