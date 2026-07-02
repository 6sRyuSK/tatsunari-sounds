# CLAUDE.md — Autonomous Plugin Factory

## What this is
A factory that builds audio plugins with **JUCE 8 + CMake**. Plugins are composed
from a shared, versioned DSP core (`core/`) and a shared UI design system (`ui/`).
Correctness is verified automatically where it is objective; humans judge taste
and authorize shipping. A cross-platform Go TUI installer (`tools/installer/`)
delivers the built plugins to end users.

## 言語 / Communication
- Respond to the user in **Japanese** (ユーザーへの応答はすべて日本語で行う).
- Keep code, identifiers, and filenames in **English** — the codebase
  convention. PR / issue descriptions may be in Japanese.
- **Commit subjects**: an English Conventional-Commits prefix, then a Japanese
  description (this is the human-facing text that lands in release notes).
  - Prefix (English, required): `feat:` (new feature/param), `fix:` (bug fix),
    or one of `docs: / refactor: / perf: / test: / chore: / ci: / build: /
    style:`. An optional `(scope)` is allowed (e.g. `feat(dynamic-eq): …`).
  - Description (Japanese): keep **identifiers and technical terms in English**
    (`STFT`, `Nyquist`, function/file names) so `git log` stays greppable —
    e.g. `fix(resonance-suppressor): STFTオーダーをサンプルレート連動にして192kHz対応`.
  - Keep the version bump for a change in the **same commit** as the change it
    describes, so `plugin.toml` stays the single source of truth for what shipped.

## Repository layout
- `core/include/factory_core/` — shared, spec'd DSP primitives (filters,
  dynamics, delay lines, FFT/STFT, resamplers, …), **header-only** and versioned;
  treated as a stable API. `testing/DspInvariants.h` holds the reusable,
  oracle-free regression checks (use them — see below).
- `ui/include/factory_ui/` — shared **header-only** UI design system: the
  "kawaii" warm-white look (`FactoryLookAndFeel`) and paint helpers
  (`FactoryChrome.h`). Every plugin editor uses it so the fleet looks like one
  product. Don't fork per-plugin palettes.
- `plugins/<slug>/` — one plugin each, composed from `core/` + `ui/`. Holds
  `plugin.toml`, `Source/` (a thin `AudioProcessor`/`Editor` wrapper), `tests/`,
  and `CMakeLists.txt`. Nine plugins currently in progress (see the README catalog).
- `cmake/` — `FactoryHelpers.cmake` (`factory_read_version` reads `version` from
  `plugin.toml`) and `NamCore.cmake` (builds NeuralAmpModelerCore for NAM Player;
  see External dependencies).
- `tools/gen_catalog.py` — regenerates the README catalog from manifests; also
  `--emit-json` for the installer's `catalog.json`. `tools/installer/` — the
  Go/Charm TUI installer.
- `docs/regression-policy.md` — the catalogued bug classes and the invariant each
  new plugin/`core` change is gated on.
- `roadmap.toml` — planned plugins not yet started (rendered into the README's
  "Planned" list; remove an entry once it gets a `plugins/<slug>/plugin.toml`).
- Root `CMakeLists.txt` auto-includes every `plugins/*/CMakeLists.txt`, fetches
  JUCE (pinned `8.0.13`), and defines the `factory_core` / `factory_ui` INTERFACE
  libraries.

## Building & testing locally
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # configure (fetches JUCE first run)
cmake --build build                                  # build all plugins + test exes
ctest --test-dir build --output-on-failure           # run all headless DSP tests
```
- Each plugin's DSP test is a standalone exe taking the **sample rate as argv[1]**;
  CTest registers one case per rate in the standard matrix (`dynamic_eq_dsp_44100`
  … `_192000`). Run one directly: `./build/plugins/dynamic-eq/dynamic_eq_dsp_test 192000`.
- The DSP tests link only `factory_core` — no JUCE, no host — so they build and
  run fast and headless.

## Architecture rules
- DSP lives in a plain C++ class **separable from `juce::AudioProcessor`** and
  testable headless — no GUI and no plugin host needed to exercise the DSP. The
  `AudioProcessor` is a thin wrapper around the core.
- Build plugins by composing `core/` primitives, not by reinventing DSP per plugin.
- Editors compose `factory_ui` (`FactoryLookAndFeel`, `factory_ui::paintBackground`
  / `paintCard` / knob helpers), not bespoke look-and-feel.

## Adding / changing a plugin
1. Create `plugins/<slug>/` with `plugin.toml` (`name`, `slug`, `category`,
   `status`, `version`, `formats`, `reference`), `Source/`, `tests/dsp_test.cpp`,
   and a `CMakeLists.txt`. Copy an existing plugin's CMake as the template:
   - `factory_read_version(... <VER>)` then `juce_add_plugin(... VERSION ${<VER>})`
     — CMake reads the version from `plugin.toml`, never hard-code it.
   - `FORMATS VST3 Standalone` with `AU` appended only `if(APPLE)`.
   - Link `factory_core` + `factory_ui` (+ `juce::juce_dsp` etc.); PUBLIC
     `juce::juce_recommended_config_flags` and `..._warning_flags`.
   - Register the DSP test with the `foreach(_fs 44100 48000 88200 96000 176400
     192000)` sample-rate loop.
2. Regenerate the catalog: `python tools/gen_catalog.py` (never hand-edit the
   README block between `<!-- BEGIN:CATALOG -->` / `<!-- END:CATALOG -->`).
3. Any change under `core/` must run the full test suite of **all** dependent
   plugins.

## Verification philosophy (this is the point of the project)
- Test against an explicit **spec**, with an **independent oracle**. The oracle is
  a *separate code path* from the implementation. **Never** derive expected values
  from the implementation under test — that only tests the code against itself.
- For filters, evaluate the expected response in the **z-domain** (the discrete
  transfer function `H(e^jω)`), never the analog prototype `H(jΩ)`: bilinear
  warping otherwise produces false failures near Nyquist.
- Where **formula-independent invariants** exist (e.g. a peaking EQ's gain at f0
  equals its gain parameter; unity at DC and Nyquist), assert those too — they
  catch bugs in the coefficient formulas themselves, not just the wiring.
- Measure the implementation by running real signal through the DSP core. For a
  linear filter, **impulse → FFT** is exact and sufficient; reserve swept-sine +
  deconvolution for non-linear plugins.
- **Sample-rate matrix (hard rule).** Test every plugin's DSP across the full
  standard rate set — **44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz** — not just
  44.1/48. Use `factory_core::testing::kStandardSampleRates()` /
  `sampleRatesFromArgs()` — do not hand-write a narrower literal. A gate that
  stops at 48 (or 96) kHz hides high-rate regressions.
- **Resolution must follow the sample rate.** Any FFT/STFT/analyzer size must be
  **derived from the sample rate**, never a fixed order — use
  `factory_core::fftOrderForSampleRate`. A fixed order silently degrades at high
  rates (at 192 kHz an order-11 window has 94 Hz bins, so the analyzer loses
  everything below ~94 Hz and the detector window shrinks 4×). Assert the
  **resolution invariants** — bin width (`fs/N`) and window length (`N/fs`) stay
  in range at the top rate — so this class of bug can't reappear in any plugin.

## Real-time safety (hard rule)
- `processBlock` and everything it calls must not allocate, lock, block, or make
  syscalls. Preallocate in `prepareToPlay`. Deliver parameter changes to the audio
  thread lock-free (atomics or a lock-free FIFO).
- pluginval's allocation checks are part of the gate — do not suppress them.

## Regression-prevention invariants (hard rule)
The bug classes surfaced by issues #22–44 must not recur. Every new plugin and
every `core/` change is gated on the invariants catalogued in
**`docs/regression-policy.md`**; the reusable, oracle-free checks live in
**`core/include/factory_core/testing/DspInvariants.h`** (use them — do not hand-write
a narrower sample-rate set). The non-negotiables:
- **Feedback loop gain < 1 at every in-range setting.** COLA-style "unity at one
  point" formulas must clamp so they never exceed 1 off that point. Verify with
  `impulseResponseNonIncreasing()` at the *worst-case* setting, all rates — not the
  one point that happens to be stable.
- **Bounded resonance.** High-order filter cascades (slope × Q) cap the peak-stage
  Q; assert a known absolute ceiling (z-domain), not just "peak > 0 dB".
- **finite guards on every feedback node** so a single NaN/Inf self-heals; assert
  finiteness *and* a realistic peak bound over a long hold (never a `1e6`
  "not-NaN" tolerance).
- **Worst-case buffer sizing** (max delay × modulation headroom) — no silent
  clamp-to-wrong-value.
- **State reset** in `prepareToPlay` and on bypass-release / channel-mode
  transitions (`reset()` or crossfade).
- **Smooth continuous parameters** (`SmoothedValue`, allocated in `prepareToPlay`).
- **Full sample-rate matrix + resolution-follows-rate** (already hard rules above).
- **Tests must exercise the bug path** (worst case, not a lucky point), with an
  **independent static oracle** for quantitative checks and a real tolerance.
- **Atomic** for any GUI/audio-shared scalar; **absolute floor** on detectors so
  silence produces no phantom reduction.
- Tightening these is welcome; **loosening any tolerance / oracle is "Ask a human".**

## External dependencies
- JUCE is the default dependency, pinned in the root `CMakeLists.txt` and fetched
  via `FetchContent`.
- **NAM Player** is the only plugin with a dependency beyond JUCE. `cmake/NamCore.cmake`
  builds Steven Atkinson's NeuralAmpModelerCore (v0.5.4) + Eigen (pinned to the exact
  commit NAM's submodule points at) into an **OBJECT** library `nam_core`. Read the
  header comments before touching it — the OBJECT (not STATIC) choice, the pinned
  Eigen commit, the `NAM_SAMPLE_FLOAT` define, and PIC are all load-bearing and were
  "learned the hard way". Only `plugins/nam-player` links it.

## Catalog & versioning
- `plugins/<slug>/plugin.toml` is the **single source of truth**. CMake reads
  `version` from it (`factory_read_version`), so catalog == binary == release.
- The README catalog between `<!-- BEGIN:CATALOG -->` / `<!-- END:CATALOG -->` is
  **generated** — never hand-edit it; run `python tools/gen_catalog.py`
  (`--check` fails CI if the README is stale).
- Bump `version` by semver: P0/P1 fix → patch; new feature/param → minor; breaks
  state/preset compatibility → major.

## Releasing
- One consolidated GitHub Release per run, tagged `<year>.<n>` (e.g. `2026.1`),
  built by `.github/workflows/release.yml` (manual `workflow_dispatch` only).
- A run **rebuilds only the plugins whose `plugin.toml` version changed** since
  the previous release (compared against that release's `manifest.json`);
  unchanged plugins are **carried over** from the previous release's assets
  verbatim (no rebuild). Bump `version` to ship a plugin — that is the trigger.
- Assets: a per-OS everything bundle (`tatsunari-sounds-<tag>-macOS.zip` /
  `-Windows.zip`), a per-plugin zip each (`<slug>-<version>-<os>.zip`),
  `manifest.json`, and `SHA256SUMS.txt`. The release notes list per-plugin
  version transitions.
- After `release.yml` publishes, `installer.yml` (on `release: published`)
  cross-compiles the TUI installer binaries and `catalog.json` and attaches them
  to the same release. It never touches `release.yml` or `manifest.json`.

## The TUI installer (`tools/installer/`)
- A single-binary, cross-platform (macOS + Windows) Go/Charm installer that
  discovers the latest release, lets users pick plugins + formats, verifies
  downloads against `SHA256SUMS.txt`, and installs to the standard folders with
  update-in-place via a receipt. Bilingual (日本語 / English) by OS locale.
- It runs **unelevated**; system-scope installs stage into a `0700` temp dir and
  re-launch once as the hidden `__apply` subcommand under the OS elevation
  mechanism, which re-validates every path against an allowlist. See its
  `README.md` for the elevation model and the Windows binary-naming caveat
  (the shipped binary is `tatsunari`, avoiding `install`/`setup`/`update` so UAC
  doesn't force a prompt).
- Develop from `tools/installer/`: `go test ./...`, `go vet ./...`. This is a
  self-contained Go module gated by its own `installer-ci.yml` — kept separate
  from the JUCE gate.

## CI gate (a PR is NOT done until)
- **JUCE/plugins** (`.github/workflows/ci.yml`): it builds on the macOS/Windows
  matrix (Ninja + ccache/sccache), unit tests pass (CTest across the full rate
  matrix), and **pluginval strictness 5, headless** passes for every built format
  (VST3 on every OS, AU on macOS). Linux is not a supported target yet, so it is
  not in the matrix. The workflow is also path-scoped: it runs only when a
  build-affecting source changes (`plugins/ core/ ui/ cmake/ CMakeLists.txt
  ci.yml`), so installer/docs-only PRs skip it.
- **Installer** (`.github/workflows/installer-ci.yml`): `go test` / `go vet`,
  triggered only when `tools/installer/**` changes.

## Ask a human — do NOT decide these autonomously
Open these for a human verdict rather than resolving them yourself:
1. **Sonic / taste** judgments ("does it sound good / right").
2. Changes to **test infrastructure, tolerances, oracles, or `disabled-tests`**.
   These are protected: loosening a gate to go green is not allowed.
3. **Shipping**: releases, code-signing, notarization — anything that reaches a user.
- Any change under `core/` must run the full test suite of **all** dependent plugins.

## Scope discipline
Implement exactly what the task specifies. Do not expand scope (no extra bands,
formats, or features unless asked).
