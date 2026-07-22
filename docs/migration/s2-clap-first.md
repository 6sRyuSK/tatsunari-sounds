> Migration spike **S2** (CLAP-first plugin format), captured 2026-07-17 during Phase P2 planning. Preserved verbatim in-repo for its target-name rules + dependency pins.

# S2 Spike Report — clap-first build pipeline on Linux

**Goal (recap):** prove a minimal CLAP plugin built via clap-wrapper's
`make_clapfirst_plugins` produces **CLAP + VST3**, validated by **clap-validator**
and **pluginval** with the same flags our CI uses; record exact target names +
artifact layout for later CI/release work.

**Result: PASS.** A minimal stereo-gain CLAP was assembled into CLAP + VST3 on
Linux x86_64. clap-validator: 16/16 applicable tests pass (0 fail). pluginval:
`SUCCESS` at strictness 5 headless. State round-trips through the wrapper. The
optional latency probe (0 ↔ 256 via an `hq` param) works end-to-end.

Everything below was done entirely in the scratch workspace
`…/scratchpad/s2-clap/`. **The repo `/home/user/tatsunari-sounds` was not touched.**

---

## 1. Pins (record these)

| Component | Pin | Notes |
|---|---|---|
| **free-audio/clap** | tag **`1.2.10`** (commit `195b42a004144fab0b3cf95e9c067187d15365b7`) | header-only `clap` INTERFACE target |
| **free-audio/clap-wrapper** | main HEAD **`35f524b771ec09f54c164720bb90f271273b37d3`** | project version `0.15.1` |
| **steinbergmedia/vst3sdk** | tag **`v3.8.0_build_66`** | exactly what clap-wrapper's own CPM path pins; parsed version `3.8.0`. Only submodules `base public.sdk pluginterfaces cmake` are needed |
| **apple/AudioUnitSDK** | tag **`AudioUnitSDK-1.1.0`** | AUv2 wrapper (APPLE only); exactly what clap-wrapper's own CPM path pins — handed via `AUDIOUNIT_SDK_ROOT` since our downloads stay OFF (added at the 3.0.0 cutover: `guarantee_auv2sdk()` otherwise fails the mac configure) |
| **clap-validator** | release **`0.3.2`** (src commit `db38eb85f3a9ec08a1d45ad1dbe5d4f6b4737b30`) | built from source here (see gotcha #3); real CI downloads the prebuilt |
| **pluginval** | release **`v1.0.4`** (src commit `ed19c2c16b57a6d94db391bea3ef4a80b769d5bf`; bundled JUCE submodule `5179f4e720d8406ebd1b5401c86aea8da6cc83c9`) | built from source here; real CI downloads the prebuilt |

**Environment:** cmake 3.28.3, ninja 1.11.1, g++ 13.3.0, cargo/rustc 1.94.1,
Linux x86_64, headless. X11/ALSA/freetype/fontconfig dev headers already present
(needed by the VST3 SDK + JUCE/pluginval). `ladspa-sdk` had to be apt-installed
for pluginval (see gotcha #6).

---

## 2. The `make_clapfirst_plugins(...)` call that worked (verbatim)

```cmake
make_clapfirst_plugins(
    TARGET_NAME s2hello
    IMPL_TARGET s2hello-impl

    OUTPUT_NAME "S2 Hello"

    ENTRY_SOURCE src/s2hello_entry.cpp

    BUNDLE_IDENTIFIER "jp.tatsunari-sounds.s2-hello"
    BUNDLE_VERSION ${PROJECT_VERSION}

    COPY_AFTER_BUILD FALSE

    PLUGIN_FORMATS CLAP VST3

    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/s2hello_assets
)
```

### The dependency wiring around it (the load-bearing part)

`make_clapfirst_plugins` and `target_add_*_wrapper` only exist **after**
clap-wrapper is `add_subdirectory`'d, and clap-wrapper needs a `clap` target and
a VST3 SDK root. The order that worked:

```cmake
cmake_minimum_required(VERSION 3.24)
project(s2-hello VERSION 0.9.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # impl static lib is linked into MODULE libs
include(FetchContent)

# 1. CLAP SDK — MakeAvailable creates the header-only `clap` INTERFACE target.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10 GIT_SHALLOW ON)
FetchContent_MakeAvailable(clap)          # guarantee_clap() then sees TARGET clap and aliases it

# 2. VST3 SDK — populate-only (clap-wrapper compiles base-sdk-vst3 itself);
#    hand it the root via VST3_SDK_ROOT. Limit submodules to the 4 that matter.
FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.0_build_66
    GIT_SUBMODULES base public.sdk pluginterfaces cmake
    GIT_SHALLOW ON GIT_PROGRESS ON)
FetchContent_GetProperties(vst3sdk)
if(NOT vst3sdk_POPULATED)
    FetchContent_Populate(vst3sdk)
endif()
set(VST3_SDK_ROOT "${vst3sdk_SOURCE_DIR}" CACHE PATH "" FORCE)

# 3. clap-wrapper — downloads OFF so its vendored CPM bootstrap never runs.
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG 35f524b771ec09f54c164720bb90f271273b37d3)
FetchContent_MakeAvailable(clap-wrapper)

# 4. Our impl static lib + the tiny entry source, then assemble formats.
add_library(s2hello-impl STATIC src/s2hello_clap.cpp)
target_link_libraries(s2hello-impl PUBLIC clap clap-wrapper-extensions)
make_clapfirst_plugins(... as above ...)
```

**How clap-wrapper resolves the SDKs** (from `cmake/base_sdks.cmake`):
`guarantee_clap()` / `guarantee_vst3sdk()` each try, in order: (1) an explicit
`<FOO>_SDK_ROOT`, (2) if `CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON`, a CPM download,
(3) a search of `libs/<name>`, `<name>`, `../<name>`. `guarantee_clap()` also
short-circuits if a `clap` target already exists — which is why
`FetchContent_MakeAvailable(clap)` is enough and we don't even set
`CLAP_SDK_ROOT`. We deliberately avoid path (2) — see gotcha #2.

**Project source layout** (3 files, mirrors clap-wrapper's `tests/clap-first-example`):
- `src/s2hello_clap.cpp` — the whole plugin against the CLAP C API + factory +
  the 3 entry hooks (`s2hello_entry_init/deinit/get_factory`). This is the
  `IMPL_TARGET` static lib.
- `src/s2hello_entry.cpp` — the `ENTRY_SOURCE`: exports the one `clap_entry`
  symbol, recompiled per format by the wrapper.
- `src/s2hello_entry.h` — declares the 3 extern hooks.

---

## 3. Generated CMake/ninja target names

The naming rule is **`<TARGET_NAME>_<format>`** plus an aggregate
**`<TARGET_NAME>_all`** custom target:

| Target | Kind | Produces |
|---|---|---|
| `s2hello-impl` | STATIC (our IMPL_TARGET) | `libs2hello-impl.a` |
| `s2hello_clap` | MODULE | `s2hello_assets/S2 Hello.clap` |
| `s2hello_vst3` | MODULE | `s2hello_assets/S2 Hello.vst3/…/S2 Hello.so` |
| `s2hello_vst3-clap-wrapper-vst3-lib` | STATIC | per-target VST3 wrapper glue (`.a`) |
| `s2hello_all` | custom (aggregate) | depends on `s2hello_clap` + `s2hello_vst3` |

Wrapper-internal targets also created (shared, built on demand): `clap`
(INTERFACE, aliased to `base-sdk-clap`), `clap-wrapper-extensions` (INTERFACE),
`clap-wrapper-shared-detail` (STATIC), `base-sdk-vst3` (STATIC),
`clap-wrapper-compile-options[-public]`, `clap-wrapper-sanitizer-options`, and a
`vst3_validator` **custom target that reconfigures the whole VST3 SDK** — do NOT
build it (it is not in `all`; only reached if you build `all` or it explicitly).

**On macOS**, the same call additionally yields `s2hello_auv2` (and, under
`-G Xcode`, `s2hello_auv3` + `s2hello_auv3_standalone`). We passed only
`CLAP VST3`, and `make_clapfirst` guards AUv2/AUv3/AAX behind `APPLE` anyway.

**Build command:** `ninja s2hello_all` (or `s2hello_clap` / `s2hello_vst3`).
58 compile/link steps, ~12 s after deps were fetched/first-built.

---

## 4. Artifact tree (exact, Linux)

`ASSET_OUTPUT_DIRECTORY` = `build/s2hello_assets`:

```
build/s2hello_assets/
├── S2 Hello.clap                                    # 24K  ELF .so w/ .clap ext
└── S2 Hello.vst3/                                   # VST3 3.7 bundle folder
    └── Contents/
        └── x86_64-linux/
            └── S2 Hello.so                          # 904K ELF shared object
```

Key facts for CI globbing:
- **Linux CLAP is a bare shared object** renamed `<OUTPUT_NAME>.clap` — *not* a
  bundle directory. `file` reports `ELF … shared object`.
- **VST3 is always a bundle folder**; the Linux binary lives at
  `<OUTPUT_NAME>.vst3/Contents/x86_64-linux/<OUTPUT_NAME>.so`.
- **OUTPUT_NAME may contain a space** ("S2 Hello") — quote paths in CI/scripts.
- Cross-platform placement inside `ASSET_OUTPUT_DIRECTORY` (from
  `make_clapfirst.cmake` / `wrap_vst3.cmake`):
  - **Linux/macOS:** `<assets>/<name>.clap` and `<assets>/<name>.vst3`
  - **Windows:** `<assets>/CLAP/<name>.clap` and `<assets>/VST3/<name>.vst3`
    (nested per-format subfolders)
  - macOS CLAP/VST3 are `.clap` / `.vst3` **bundle dirs** (`Contents/MacOS/…`);
    Windows VST3 binary is under `Contents/x86_64-win/…` (set
    `WINDOWS_FOLDER_VST3 TRUE` — the wrapper example notes the Windows VST3
    validator needs the folder form to pass).

---

## 5. clap-validator results

Command (the natural CI invocation — default is out-of-process, one subprocess
per test):

```
clap-validator validate "build/s2hello_assets/S2 Hello.clap"
```

**`21 tests run, 16 passed, 0 failed, 5 skipped, 0 warnings` — exit 0.**

Passed (highlights): `descriptor-consistency`, `features-categories`,
`features-duplicates`, `param-conversions`, `param-fuzz-basic`,
`param-set-wrong-namespace`, `process-audio-out-of-place-basic`,
`state-invalid`, `state-reproducibility-basic`, `state-reproducibility-flush`,
`state-reproducibility-null-cookies`, `state-buffered-streams`,
`create-id-with-trailing-garbage`, `query-factory-nonexistent`,
`scan-rtld-now`, `scan-time` (0 ms).

The 5 **SKIPPED** are all expected for a plain audio effect and are **not**
failures:
- 3× `preset-discovery-*` → no `clap.preset-discovery-factory` (we ship no presets)
- 2× `process-note-*` → no `clap.note-ports` (no MIDI/note input)

Benign noise: `DEBUG … TODO: Handle 'clap_host_params::rescan()'` — clap-validator
just logs that it doesn't act on our `rescan()` from `state.load`. Not a failure.

---

## 6. pluginval results (on the wrapper-built VST3)

Exact CI flags:

```
pluginval --strictness-level 5 --skip-gui-tests --validate-in-process \
          --validate "build/s2hello_assets/S2 Hello.vst3"
```

**`SUCCESS` — exit 0.** Discovered `VST3-S2 Hello Gain` (`tatsunari-sounds:
S2 Hello Gain v0.9.0`), AUDIO in 1 / out 1.

Sections all completed with no failures at strictness 5:
Scan · Open (cold) · Open (warm) · Plugin info · Plugin programs ·
**Audio processing** (44100/48000/96000 Hz × block 64/128/256/512/1024) ·
**Plugin state** · Automation (same rate×block matrix, sub-block 32) ·
Automatable Parameters · Basic bus · Listing/Enabling/Disabling buses ·
Restoring default layout.

- `Reported latency: 0` — matches the plugin's hq=off default.
- `vst3 validator` sub-test = *skipped* ("validator path hasn't been set") —
  that's Steinberg's separate binary, optional, unrelated to our gate.
- Buses: `Named layouts: Stereo`, main in/out = 2/2 — correct stereo I/O
  surfaced through the wrapper.

---

## 7. State round-trip sanity (through the wrapper)

Confirmed on **both** formats:
- **VST3 (through the wrapper):** pluginval `Plugin state` section at strictness 5
  (lines `Starting tests in: pluginval / Plugin state...` →
  `Completed tests in pluginval / Plugin state`) — at level 5 this randomises
  parameters, saves the VST3 state, reloads, and asserts the parameters match.
  Passed. Our two CLAP params (`gain`, `hq`) survive the CLAP↔VST3 state bridge.
- **Native CLAP:** clap-validator `state-reproducibility-basic` / `-flush` /
  `-null-cookies` / `state-buffered-streams` all passed (save → recreate →
  reload → identical params & re-serialised bytes).

---

## 8. Latency probe — what it showed

**Implemented** (documented honestly): reports a **fixed 0** by default; a second
bool/stepped param **`hq` (id=2)** flips the *reported* latency **0 → 256**.
Contract followed (`clap/ext/latency.h`: "latency is only allowed to change during
activate; if activated, call `host->request_restart()`"):
- Latency value is latched **only inside `activate()`** from the current `hq`.
- A `hq` change arriving in `process()` (audio thread) calls
  `host->request_restart()` (thread-safe) so the host deactivates→reactivates and
  re-queries `get()`.
- It is a **reporting-only** probe — no real delay line is inserted (all S2 needs
  is to prove the latency plumbing survives the wrapper + validators).

Verified with an out-of-tree CLAP host harness (`tools/latency_probe.cpp`,
dlopen + drive the C API):

```
latency (hq=off, default): 0
process() with hq=1 event → restart requested by plugin: YES
latency (hq=on, after restart): 256
PROBE RESULT: PASS
```

RT-safety: `process()` and everything it calls does no alloc/lock/syscall; the
gain/hq/latency scalars shared between audio and main threads are `std::atomic`
(lock-free). pluginval's in-process run at strictness 5 raised no allocation/
threading complaints.

---

## 9. Gotchas (every one that cost time)

1. **GitHub egress policy blocks release-asset & API downloads.** Through the
   session proxy, `api.github.com` and `github.com/<o>/<r>/releases/download/…`
   return **403** ("GitHub access to this repository is not enabled … Use
   add_repo"). The README says do not retry 403s. **git clone / git submodule /
   FetchContent-over-git all WORK** (the proxy rewrites to an internal git host,
   e.g. `http://127.0.0.1:PORT/git/<o>/<r>`). Consequence in *this sandbox*: we
   could not `curl` the prebuilt clap-validator / pluginval / `CPM.cmake`, and
   built the two validators from source. **In real GitHub-Actions CI these
   release downloads are fine** — this is a sandbox-only constraint, except for #2.

2. **`CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON` relies on a GitHub *release*
   download even for CPM itself.** clap-wrapper's `guarantee_cpm()` includes a
   **vendored `cmake/external/CPM.cmake` that is only a bootstrap** — it does
   `file(DOWNLOAD https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake)`.
   That URL is blocked here, so the whole download path is unusable in the
   sandbox. **Fix / recommended pattern:** don't use `DOWNLOAD_DEPENDENCIES`;
   fetch clap + vst3sdk yourself with `git`/FetchContent and pass them in
   (`clap` target already present, `VST3_SDK_ROOT` set). This is also better for
   reproducibility/pinning and mirrors how the root repo already pins JUCE.

3. **clap-validator 0.3.2 & pluginval sources have 2024-era Rust/JUCE deps.**
   clap-validator's `Cargo.lock` pins `time 0.3.20`, which **fails to compile
   under rustc 1.94** (`error[E0282]: type annotations needed for Box<_>`). A
   surgical `cargo update -p time@0.3.20` only reached `0.3.26` (still fails).
   **What worked:** a full `cargo update` (refresh the whole lockfile to latest
   semver-compatible) before `cargo build --release`. N/A for real CI (uses the
   prebuilt binary).

4. **Nothing in the plugin build needed `-Werror` relief.** clap-wrapper builds
   *its own* targets with `-Wall -Wextra -Wpedantic -Werror`, but our
   `IMPL_TARGET` links only `clap` + `clap-wrapper-extensions` (both INTERFACE,
   no compile flags), so our plugin code compiles with default flags. Good to
   know: strictness on our code is opt-in, not inherited.

5. **Benign VST3 warning:** `RESOURCE_DIRECTORY defined, but not (yet) supported
   for Unix VST3s` — `make_clapfirst` forwards an empty `RESOURCE_DIRECTORY`;
   ignore it (or omit the arg).

6. **pluginval (JUCE) needs `ladspa.h`.** JUCE's `juce_audio_processors` compiles
   `juce_LADSPAPluginFormat.cpp` which `#include <ladspa.h>`. Fixed with
   `apt-get install -y ladspa-sdk`. (The X11/ALSA/freetype/fontconfig headers
   were already present.) pluginval ran headless fine with `--skip-gui-tests`
   `--validate-in-process` — **no X server / xvfb needed** in this container.

7. **VST3 SDK submodules:** fetch **only** `base public.sdk pluginterfaces cmake`
   (matches clap-wrapper's own list). Skipping `vstgui4` etc. keeps the clone
   small and `base-sdk-vst3` compiles ~25 headless TUs (no GUI). `GIT_SHALLOW ON`
   + limited submodules made the whole configure ≈ seconds.

8. **Required CMake vars for a clap-first consumer:** `CMAKE_POSITION_INDEPENDENT_CODE ON`
   (static impl → MODULE libs), `CMAKE_CXX_STANDARD ≥ 17`. clap-wrapper defaults
   `CMAKE_OSX_DEPLOYMENT_TARGET` to 10.13 and static-links the MSVC runtime — fine
   to leave to the wrapper, or override in our toolchain files.

---

## 10. Recommendations for the repo

### `cmake/FactoryClapPlugin.cmake` (new wrapper macro)

- **Centralise the SDK pins once** (one CLAP tag, one clap-wrapper commit, one
  VST3 tag), the same way the root `CMakeLists.txt` pins JUCE `8.0.13`. Put the
  three `FetchContent` blocks (clap → MakeAvailable; vst3sdk → populate-only +
  `VST3_SDK_ROOT`; clap-wrapper → MakeAvailable, `DOWNLOAD_DEPENDENCIES OFF`)
  behind an include-once guard (`if(NOT TARGET clap-wrapper-extensions)`).
- Provide `factory_clap_plugin(<slug> …)` that:
  - reads `version` from `plugins/<slug>/plugin.toml` via the existing
    `factory_read_version` (keep plugin.toml the single source of truth);
  - creates `<slug>-impl` STATIC from the plugin's `Source/`, linking
    `clap clap-wrapper-extensions` **plus** our header-only `factory_core` /
    `factory_params` / `factory_presets` / `factory_ui` INTERFACE libs (the DSP
    stays JUCE-free and headless-testable exactly as today);
  - emits the tiny `ENTRY_SOURCE` from a template (or ship one shared entry .cpp
    that references fixed-name `*_entry_init/deinit/get_factory` hooks the
    scaffold generates), then calls `make_clapfirst_plugins(TARGET_NAME <slug>
    IMPL_TARGET <slug>-impl OUTPUT_NAME "<Name>" ENTRY_SOURCE … BUNDLE_IDENTIFIER
    jp.tatsunari-sounds.<slug> BUNDLE_VERSION <ver> PLUGIN_FORMATS CLAP VST3
    [AUV2 if APPLE] ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/<slug>_assets)`.
- **Formats:** `CLAP VST3` everywhere; add `AUV2` only on `APPLE`. AUv3/AAX are
  out of scope (AUv3 needs `-G Xcode`). This keeps parity with the current macOS
  + Windows CI matrix (Linux stays local-only).
- The headless DSP test story is unchanged: keep the plain C++ DSP class + its
  `ctest` exe linking only `factory_core`. The clap-first macro is *only* the
  packaging layer — the `AudioProcessor`-style thin wrapper is replaced by the
  thin CLAP C-API wrapper in the impl lib.

### `ci.yml` / `release.yml` globs & steps

- **Build target:** `<slug>_all` (or `<slug>_clap` + `<slug>_vst3`). **Never
  build `all`** — it would drag in the `vst3_validator` custom target that
  reconfigures the entire VST3 SDK.
- **Artifact globs** (per-plugin `ASSET_OUTPUT_DIRECTORY = build/<slug>_assets`):
  - CLAP: `build/<slug>_assets/*.clap`
    (Linux/macOS) or `build/<slug>_assets/CLAP/*.clap` (Windows).
  - VST3: `build/<slug>_assets/*.vst3`
    (Linux/macOS) or `build/<slug>_assets/VST3/*.vst3` (Windows) — collect the
    **whole `.vst3` bundle dir**, not just the inner `.so`/`.vst3` binary.
  - A robust cross-platform glob: `build/<slug>_assets/**/*.clap` and
    `…/**/*.vst3` (zip the matched bundle roots).
- **Validation steps:**
  - **VST3 → pluginval, unchanged flags:** `--strictness-level 5 --skip-gui-tests
    --validate-in-process --validate "<…/name.vst3>"`. Confirmed working against
    the wrapper VST3 (§6).
  - **CLAP → clap-validator (new step):** `clap-validator validate "<…/name.clap>"`;
    download the prebuilt from clap-validator releases (0.3.2). Treat `SKIPPED`
    as pass; require exit 0. Recommended to run on VST3 (pluginval) **and** CLAP
    (clap-validator) so both wrapper outputs are gated.
  - Keep pluginval's existing macOS(AU+VST3)/Windows(VST3) matrix; add the CLAP
    (clap-validator) leg on both OSes.
- **Windows VST3:** pass `WINDOWS_FOLDER_VST3 TRUE` (folder-form bundle) — the
  clap-wrapper example notes the Windows VST3 validator needs it to pass.
- **Version/ship trigger** is unchanged conceptually: `plugin.toml` `version`
  read by CMake feeds `BUNDLE_VERSION`; catalog == binary == release still holds.

---

## 11. Deliverables in this workspace

```
scratchpad/s2-clap/
├── s2-report.md                     # this file
├── project/                         # the building clap-first project
│   ├── CMakeLists.txt               # the wiring in §2
│   ├── src/s2hello_clap.cpp         # the CLAP (impl static lib)
│   ├── src/s2hello_entry.cpp/.h     # the ENTRY_SOURCE + hooks
│   └── build/s2hello_assets/        # S2 Hello.clap  +  S2 Hello.vst3/…
├── tools/latency_probe.cpp(+bin)    # out-of-tree host that verified 0↔256
└── deps-src/                        # git clones used for reading + building the
    ├── clap/ clap-wrapper/          #   two validators from source (proxy blocked
    ├── clap-validator/ (built)      #   the prebuilt release binaries)
    └── pluginval/     (built)
```

Reproduce (from `project/`):
```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build s2hello_all
clap-validator validate "build/s2hello_assets/S2 Hello.clap"
pluginval --strictness-level 5 --skip-gui-tests --validate-in-process \
          --validate "build/s2hello_assets/S2 Hello.vst3"
```
