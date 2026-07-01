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
  - The release changelog groups by the prefix — `feat:` → 機能, `fix:` → 修正,
    everything else → その他 — and strips the prefix from the shown line, so the
    prefix keeps notes accurate while the Japanese description reads naturally.
  - Keep the version bump for a change in the **same commit** as the change it
    describes, so it shows in that plugin's notes.

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