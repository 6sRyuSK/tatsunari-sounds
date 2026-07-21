# CLAUDE.md — Autonomous Plugin Factory

## What this is
A factory that builds audio plugins with **CMake**. Most plugins ship as
**JUCE 8** VST3/AU; `resonance-suppressor` ships **CLAP-first** (clap-wrapper's
`make_clapfirst` → CLAP + wrapper VST3, AUv2 on Apple) with a JUCE-free
**Visage** editor. All compose a shared, versioned DSP core (`core/`), shared
parameter/preset models (`params/`, `presets/`), and the shared UI design
systems (`ui/`). Correctness is verified automatically where it is objective;
humans judge taste and authorize shipping. A cross-platform Go TUI installer
(`tools/installer/`) delivers the built plugins to end users.

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
- `factory-ui` — the design-system API and editor conventions (JUCE editors).
- `visage-ui` — the Visage layer: `factory_ui_visage` (`ui/visage/`), the RS
  editor conventions, theme JSON, the visage core API, and the `tools/ui-dev`
  loop.
- `core-primitives` — catalog of `core/` primitives to compose from.
- `release` — versioning + release/installer pipeline mechanics.
- `pluginval-debug` — diagnosing CI pluginval failures.
- `installer-dev` — the Go TUI installer module.

No skill covers the CLAP shell layer yet: for `shell/` read that layer's own
headers plus `docs/migration/s2-clap-first.md` (SDK pins and gotchas);
`docs/migration/s1-wasm-loop.md` keeps the WASM-loop pins the `visage-ui`
skill builds on.

## Repository layout
- `core/include/factory_core/` — shared, spec'd DSP primitives, **header-only**,
  treated as a stable API; `testing/DspInvariants.h` holds the reusable
  regression checks.
- `ui/include/factory_ui/` — the shared **header-only** JUCE "kawaii" warm-white
  design system (all JUCE plugins + RS's test oracles); don't fork per-plugin
  palettes. `ui/visage/` — `factory_ui_visage`, the **compiled Visage** design
  system used by the RS editor (widgets, `theme/factory-default.json`, fonts,
  own tests); it owns the pinned + sandbox-patched visage dependency.
- `shell/include/factory_shell/` — framework-free CLAP glue over the CLAP C API
  (`ClapShellPlugin<Policy>`, param/state bridges, `ClapEditor`, `DenormalGuard`);
  `shell/cmake/FactoryClapPlugin.cmake` pins the CLAP / VST3 / clap-wrapper
  (+ AudioUnitSDK) SDKs and wraps `make_clapfirst_plugins`.
- `presets/include/factory_presets/` — shared **header-only** preset/program
  model: `PresetBank.h` (JUCE-free tables), `ProgramAdapter.h` (JUCE program
  API + the `stateToXml`/`applyStateXml` state helpers every JUCE processor
  uses), and the JUCE-free `StateCodec.h`/`PresetSession.h`/
  `UserPresetStore(Fs).h` (state/session/user-preset model, ridden by the CLAP
  shell and the shared preset selector).
- `params/include/factory_params/` — shared **header-only** parameter model:
  `ParamDesc.h`/`Range.h`/`Text.h`/`ParamStore.h`/`UndoStack.h` (JUCE-free),
  `juce/ApvtsAdapter.h` (generates the APVTS layout from a `ParamDesc` table).
- `plugins/<slug>/` — one plugin each: `plugin.toml`, `Source/` (thin
  `AudioProcessor`/`Editor` wrapper), `tests/`, `CMakeLists.txt`.
  resonance-suppressor deviates (clap-first): `RsCore.h` (framework-free core),
  `ui/` (JUCE-free Visage editor), `shell/` (CLAP entry); its `Source/` JUCE
  processor remains **only** as the byte-equivalence test oracle, never shipped.
- `cmake/FactoryHelpers.cmake` (`factory_read_version`), `cmake/NamCore.cmake`
  (NAM Player's only-extra dependency; its header comments are load-bearing).
- `tools/gen_catalog.py` — regenerates the README catalog (+ `--emit-json`);
  `tools/release_plan.py` — the unit-tested decision core of `release.yml`
  (kind `juce`|`clap`); `tools/tests/` — stdlib unittests for both;
  `tools/scaffold_plugin.py` — new-plugin generator; `tools/installer/` — TUI
  installer; `tools/ui-dev/` — local WASM Visage UI dev harness (own README,
  not in CI); `tools/vst3-probe/` — dev-only Windows VST3 host probe;
  `tools/build-clap.ps1` / `tools/install.ps1` — Windows local build/install helpers.
- `docs/regression-policy.md` — catalogued bug classes and their gate invariants;
  `docs/migration/` — the S1 (WASM UI loop) / S2 (clap-first) spike reports,
  preserved for their dependency pins + gotchas; `docs/plans/` — design plans.
- `roadmap.toml` — planned plugins (remove an entry once it gets a `plugin.toml`).
- Root `CMakeLists.txt` auto-includes `plugins/*/CMakeLists.txt`, pins JUCE
  `8.0.13`, and takes `-DFACTORY_PLUGINS=<slugs>` (comma/semicolon-separated)
  to configure a subset; whenever RS is in the configured set it also assembles
  the clap-first shell, fetching the pinned CLAP/VST3/clap-wrapper + Visage SDKs
  (`FACTORY_RS_CLAP` is a legacy no-op).

## Building & testing locally
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # configure (fetches JUCE first run)
cmake --build build                                  # build all plugins + test exes
ctest --test-dir build --output-on-failure           # run all headless DSP tests
```
DSP tests link only `factory_core` (no JUCE, headless); each test exe takes the
sample rate as argv[1] and CTest registers one case per standard rate (the
shared `core`/`params`/`presets` model suites register from the root too).
`-DFACTORY_PLUGINS=<slug>[,<slug>]` narrows the configure to a subset (CI uses
it as well); any set including resonance-suppressor also fetches the CLAP/VST3/
clap-wrapper + Visage SDKs on first configure.
On Windows without `-G Ninja`, CMake defaults to the Visual Studio
**multi-config** generator, which ignores `CMAKE_BUILD_TYPE` — pick the config
at build/test time instead: `cmake --build build --config Release` and
`ctest --test-dir build -C Release`. CI builds RS's clap-first assembly with
Ninja; `tools/build-clap.ps1` reproduces that path locally (VS toolchain
bootstrap included).
On a fresh Linux box the JUCE configure step needs the X11/ALSA dev packages
first (`libasound2-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev
libxinerama-dev libxrandr-dev libxrender-dev libfreetype-dev
libfontconfig1-dev`, plus `mesa-common-dev libgl1-mesa-dev` for RS's Visage
GUI); Linux builds are for local verification only — it is not a shipping target.

## Architecture rules
- DSP lives in a plain C++ class **separable from the plugin framework**,
  testable headless; the `AudioProcessor` (or the CLAP shell's Policy) is a
  thin wrapper.
- Compose `core/` primitives instead of reinventing DSP; JUCE editors compose
  `factory_ui`, the Visage editor composes `factory_ui_visage` — no bespoke
  look-and-feel either way.
- resonance-suppressor ships clap-first: the binaries come from the
  `make_clapfirst` shell (CLAP + wrapper VST3, AUv2 on Apple) embedding the
  Visage editor. Its `Source/` JUCE processor exists **only** as the oracle for
  `rscore_equiv_test` (byte-identical output vs `rs_core::RsCore`) — change
  both sides in lockstep; never package it.
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
version changed since the previous release's `manifest.json` (the decision core
is `tools/release_plan.py`); unchanged plugins carry over verbatim — bumping
`version` is the ship trigger. RS builds as kind `clap`, but its release zips
ship **VST3 + AU parity only** (bundling the native `.clap` waits on installer
support). `installer.yml` attaches the TUI installer + `catalog.json` after publish.

## CI gate (a PR is NOT done until)
- **Plugins** (`ci.yml`, path-scoped to build-affecting sources): macOS +
  Windows builds (RS clap-first included), CTest across the full rate matrix,
  and **pluginval strictness 5 headless** for every built format — VST3 both
  OS, AU on macOS, RS's wrapper VST3/AU among them. Linux is not a supported
  target.
- **CLAP** (`clap.yml`, scoped to shell/core/params/presets + RS shell): Linux
  build of the RS clap-first assembly + **clap-validator** on the native
  `.clap` — the one signal ci.yml doesn't produce.
- **Factory tools** (`factory-tools-ci.yml`, scoped to tools/tomls/README):
  `gen_catalog.py --check` (README catalog freshness) + the `tools/tests`
  unittest suite.
- **Installer** (`installer-ci.yml`, `tools/installer/**` only): `go test` / `go vet`.

## Ask a human — do NOT decide these autonomously
1. **Sonic / taste** judgments ("does it sound good / right").
2. Changes to **test infrastructure, tolerances, oracles, or `disabled-tests`**
   — loosening a gate to go green is not allowed.
3. **Shipping**: releases, code-signing, notarization — anything that reaches a user.

## Scope discipline
Implement exactly what the task specifies. Do not expand scope (no extra bands,
formats, or features unless asked).
