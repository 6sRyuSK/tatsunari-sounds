# CLAUDE.md — Autonomous Plugin Factory

## What this is
A factory that builds audio plugins with **JUCE 8 + CMake**. Plugins are composed
from a shared, versioned DSP core (`core/`) and a shared UI design system (`ui/`).
Correctness is verified automatically where it is objective; humans judge taste
and authorize shipping. A cross-platform Go TUI installer (`tools/installer/`)
delivers the built plugins to end users.

## 言語 / Communication
- Respond to the user in **Japanese**. Keep code, identifiers, and filenames in
  **English**; PR / issue descriptions may be in Japanese.
- **Commit subjects**: English Conventional-Commits prefix (`feat: / fix: /
  docs: / refactor: / perf: / test: / chore: / ci: / build: / style:`, optional
  `(scope)`), then a Japanese description with identifiers and technical terms
  kept in English — e.g. `fix(resonance-suppressor): STFTオーダーをサンプルレート
  連動にして192kHz対応`. Do **not** bump `plugin.toml` versions per commit: leave
  the version at baseline during branch work and bump **once, when opening the
  PR** (plugin PRs are squash-merged, so intermediate bumps are noise; a missing
  bump means the plugin won't ship — verify before the PR).

## Skills (read these INSTEAD of other sources as reference)
The detailed conventions live in `.claude/skills/` — do not open other plugins'
sources (or re-read workflow files / ui headers) to copy patterns:
- `new-plugin` — scaffold + full workflow for a new plugin.
- `add-param` — end-to-end wiring for a new parameter on an existing plugin.
- `add-preset` — end-to-end wiring for factory presets on an existing plugin.
- `write-dsp-test` — test structure, `DspInvariants.h` API, oracle rules.
- `factory-ui` — the design-system API and editor conventions.
- `core-primitives` — catalog of `core/` primitives to compose from.
- `release` — versioning + release/installer pipeline mechanics.
- `pluginval-debug` — diagnosing CI pluginval failures.
- `installer-dev` — the Go TUI installer module.

## Repository layout
- `core/include/factory_core/` — shared, spec'd DSP primitives, **header-only**,
  treated as a stable API; `testing/DspInvariants.h` holds the reusable
  regression checks. `ui/include/factory_ui/` — the shared **header-only**
  "kawaii" warm-white design system; don't fork per-plugin palettes.
- `presets/include/factory_presets/` — shared **header-only** preset/program
  model: `PresetBank.h` (JUCE-free tables), `ProgramAdapter.h` (JUCE program
  API + the `stateToXml`/`applyStateXml` state helpers every processor uses).
- `params/include/factory_params/` — shared **header-only** parameter model:
  `ParamDesc.h`/`Range.h`/`Text.h`/`ParamStore.h`/`UndoStack.h` (JUCE-free),
  `juce/ApvtsAdapter.h` (generates the APVTS layout from a `ParamDesc` table).
- `plugins/<slug>/` — one plugin each: `plugin.toml`, `Source/` (thin
  `AudioProcessor`/`Editor` wrapper), `tests/`, `CMakeLists.txt`.
- `cmake/FactoryHelpers.cmake` (`factory_read_version`), `cmake/NamCore.cmake`
  (NAM Player's only-extra dependency; its header comments are load-bearing).
- `tools/gen_catalog.py` — regenerates the README catalog (+ `--emit-json`);
  `tools/scaffold_plugin.py` — new-plugin generator; `tools/installer/` — TUI installer.
- `docs/regression-policy.md` — catalogued bug classes and their gate invariants.
- `roadmap.toml` — planned plugins (remove an entry once it gets a `plugin.toml`).
- Root `CMakeLists.txt` auto-includes `plugins/*/CMakeLists.txt` and pins JUCE `8.0.13`.

## Building & testing locally
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # configure (fetches JUCE first run)
cmake --build build                                  # build all plugins + test exes
ctest --test-dir build --output-on-failure           # run all headless DSP tests
```
DSP tests link only `factory_core` (no JUCE, headless); each test exe takes the
sample rate as argv[1] and CTest registers one case per standard rate.
On Windows without `-G Ninja`, CMake defaults to the Visual Studio
**multi-config** generator, which ignores `CMAKE_BUILD_TYPE` — pick the config
at build/test time instead: `cmake --build build --config Release` and
`ctest --test-dir build -C Release`.
On a fresh Linux box the JUCE configure step needs the X11/ALSA dev packages
first (`libasound2-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev
libxinerama-dev libxrandr-dev libxrender-dev libfreetype-dev
libfontconfig1-dev`); Linux builds are for local verification only — it is not
a shipping target.

## Architecture rules
- DSP lives in a plain C++ class **separable from `juce::AudioProcessor`**,
  testable headless; the `AudioProcessor` is a thin wrapper.
- Compose `core/` primitives instead of reinventing DSP; editors compose
  `factory_ui` instead of bespoke look-and-feel.
- Don't hand-write the shared plumbing: state save/load rides
  `factory_presets::stateToXml`/`applyStateXml`, and the editor's preset
  selector + host program sync ride `factory_ui::PresetSelectorController`
  (the scaffold emits both; details in the `add-preset` skill).

## Adding / changing a plugin
1. New plugin: `python tools/scaffold_plugin.py <slug> --name "…" --category …
   --reference "…"` — never hand-copy an existing plugin (see `new-plugin` skill).
2. Regenerate the catalog: `python tools/gen_catalog.py` (never hand-edit the
   README block between the CATALOG markers; `--check` gates CI).
3. Any change under `core/` must run the full test suite of **all** plugins.

## Verification philosophy (hard rules — details in `write-dsp-test`)
- Test against an explicit **spec** with an **independent oracle** (a separate
  code path — never derive expected values from the implementation under test).
- Filters: evaluate expectations in the **z-domain**, never the analog prototype.
  Assert **formula-independent invariants** too (e.g. unity at DC/Nyquist).
- Measure real signal through the core (linear: impulse → FFT is exact).
- **Full sample-rate matrix**: 44.1–192 kHz via
  `factory_core::testing::kStandardSampleRates()` / `sampleRatesFromArgs()` —
  never a hand-written narrower set.
- **Resolution follows the sample rate**: FFT/STFT orders come from
  `factory_core::fftOrderForSampleRate`, with bin-width / window-length
  invariants asserted at the top rate. Fixed orders are forbidden.

## Real-time safety (hard rule)
`processBlock` and everything it calls must not allocate, lock, block, or make
syscalls; preallocate in `prepareToPlay`; parameter delivery is lock-free.
pluginval's allocation checks are part of the gate — do not suppress them.

## Regression-prevention invariants (hard rule)
The bug classes from issues #22–44 must not recur; gates are catalogued in
`docs/regression-policy.md` and implemented via `testing/DspInvariants.h`:
- Feedback loop gain < 1 at **every** in-range setting
  (`impulseResponseNonIncreasing()` at the worst case, all rates).
- Bounded resonance (absolute z-domain ceiling on cascades).
- Finite guards on every feedback node; realistic peak bounds over long holds.
- Worst-case buffer sizing; state reset on prepare/bypass/channel transitions.
- `SmoothedValue` on continuous params; atomics on GUI/audio-shared scalars;
  absolute floor on detectors (no phantom reduction on silence).
- Tests exercise the **bug path** (worst case) with an independent static oracle.
- Tightening is welcome; **loosening any tolerance / oracle is "Ask a human"**.

## Catalog & versioning
- `plugins/<slug>/plugin.toml` is the **single source of truth**; CMake reads
  `version` from it, so catalog == binary == release.
- Semver: P0/P1 fix → patch; new feature/param → minor; breaks state/preset
  compatibility → major.

## Releasing (mechanics in the `release` skill)
One consolidated GitHub Release per run (tag `<year>.<n>`, manual
`workflow_dispatch` only). A run rebuilds **only** plugins whose `plugin.toml`
version changed since the previous release's `manifest.json`; unchanged plugins
carry over verbatim — bumping `version` is the ship trigger. `installer.yml`
attaches the TUI installer + `catalog.json` after publish.

## CI gate (a PR is NOT done until)
- **JUCE/plugins** (`ci.yml`, path-scoped to build-affecting sources): macOS +
  Windows builds, CTest across the full rate matrix, and **pluginval strictness
  5 headless** for every built format (VST3 both OS, AU on macOS). Linux is not
  a supported target.
- **Installer** (`installer-ci.yml`, `tools/installer/**` only): `go test` / `go vet`.

## Ask a human — do NOT decide these autonomously
1. **Sonic / taste** judgments ("does it sound good / right").
2. Changes to **test infrastructure, tolerances, oracles, or `disabled-tests`**
   — loosening a gate to go green is not allowed.
3. **Shipping**: releases, code-signing, notarization — anything that reaches a user.

## Scope discipline
Implement exactly what the task specifies. Do not expand scope (no extra bands,
formats, or features unless asked).
