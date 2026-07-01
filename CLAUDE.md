# CLAUDE.md — Autonomous Plugin Factory

## What this is
A factory that builds audio plugins with **JUCE 8 + CMake**. Plugins are composed
from a shared, versioned DSP core. Correctness is verified automatically where it
is objective; humans judge taste and authorize shipping.

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

## Releasing
- One consolidated GitHub Release per run, tagged `<year>.<n>` (e.g. `2026.1`),
  built by `.github/workflows/release.yml` (manual `workflow_dispatch` only).
- A run **rebuilds only the plugins whose `plugin.toml` version changed** since
  the previous release (compared against that release's `manifest.json`);
  unchanged plugins are **carried over** from the previous release's assets
  verbatim (no rebuild). Bump `version` to ship a plugin — that is the trigger.
- Assets: a per-OS everything bundle (`tatsunari-plugins-<tag>-macOS.zip` /
  `-Windows.zip`), a per-plugin zip each (`<slug>-<version>-<os>.zip`), and
  `manifest.json`. The release notes list per-plugin version transitions.

## Repository layout
- `core/` — shared, spec'd DSP primitives (filters, dynamics, …). Versioned;
  treated as a stable API.
- `plugins/<slug>/` — one plugin each, composed from `core/`. Holds `plugin.toml`,
  source, and tests.
- `tools/gen_catalog.py` — regenerates the README catalog from manifests.
- `roadmap.toml` — planned plugins not yet started.

## Architecture rules
- DSP lives in a plain C++ class **separable from `juce::AudioProcessor`** and
  testable headless — no GUI and no plugin host needed to exercise the DSP. The
  `AudioProcessor` is a thin wrapper around the core.
- Build plugins by composing `core/` primitives, not by reinventing DSP per plugin.

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
  44.1/48. A gate that stops at 48 kHz hides high-rate regressions.
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

## Catalog & versioning
- `plugins/<slug>/plugin.toml` is the **single source of truth**. CMake reads
  `version` from it, so catalog == binary == release.
- The README catalog between `<!-- BEGIN:CATALOG -->` / `<!-- END:CATALOG -->` is
  **generated** — never hand-edit it; run `python tools/gen_catalog.py`.
- Bump `version` by semver: P0/P1 fix → patch; new feature/param → minor; breaks
  state/preset compatibility → major.

## CI gate (a PR is NOT done until)
- It builds on the matrix, unit tests pass (CTest), and **pluginval strictness 5,
  headless** passes for every built format.

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