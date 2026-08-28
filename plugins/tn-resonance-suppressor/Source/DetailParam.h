#pragma once
//
// DetailParam.h — the pure math behind the "detail" macro parameter (v2.1) and
// its state migration. Header-only and JUCE-free ON PURPOSE: PluginProcessor
// uses these mappings on the audio/message threads, and the headless DSP test
// (tests/dsp_test.cpp, factory_core-only) asserts them directly — the same
// single definition serves both, so the plugin and its oracle can never drift.
//
// detail (0..100 %, default 50) replaces the legacy "sharpness"/"selectivity"
// pair as the DSP driver (the legacy parameters stay REGISTERED for VST3
// automation compatibility but are no longer read):
//
//   sharpOct    = 0.15 + 0.85 * d/100        (identical to the old sharpness map)
//   selectivity = d/100                       (identical to the old selectivity map)
//   smoothOct   = (1/12) * 2^((50 - d)/50), clamped to [1/24, 1/6]
//
// d = 50 reproduces today's defaults BIT-exactly (sharp 0.575 / sel 0.5 /
// smoothing exactly 1.0/12.0 — the engine default the plugin previously never
// overrode), so a fresh v2.1 instance sounds identical to v2.0.1. The
// smoothing curve is a LISTENING CHECKPOINT (tune by ear, not by test).
//
// Migration: a pre-detail state derives detail from the legacy pair as their
// mean, clamp((sharpness% + selectivity%)/2, 0, 100) — the inverse of the two
// linear maps above at their shared percentage scale.
//
#include <algorithm>
#include <cmath>

namespace resonance_suppressor_detail
{
    // Engine sharpness (envelope half-width, octaves) for a detail percentage.
    inline double sharpOctForDetail (double detailPct) noexcept
    {
        return 0.15 + detailPct / 100.0 * 0.85; // 0.15..1.0 octave
    }

    // Engine selectivity (0..1) for a detail percentage.
    inline double selectivityForDetail (double detailPct) noexcept
    {
        return detailPct / 100.0;
    }

    // Engine reduction-smoothing width (octaves) for a detail percentage.
    // d = 50 -> exactly 1.0/12.0 (2^0 == 1.0, so the product is bit-exact);
    // d = 0 -> 1/6 (broadest, softest), d = 100 -> 1/24 (narrowest, most
    // surgical). The clamp equals the formula's own endpoints, so it only
    // guards pathological inputs.
    inline double gainSmoothOctForDetail (double detailPct) noexcept
    {
        const double oct = (1.0 / 12.0) * std::exp2 ((50.0 - detailPct) / 50.0);
        return std::clamp (oct, 1.0 / 24.0, 1.0 / 6.0);
    }

    // F2: Depth knob (%) -> engine depth. Soft mode tops out at 1.0 (100 % =
    // "flatten to the envelope", never below it at profile 1 — see the
    // engineSoftDepthBoundTest oracle); Hard keeps the historical 1.5 span
    // (there Depth doubles as the absolute-threshold sweep).
    inline double engineDepthForPct (double depthPct, bool hardMode) noexcept
    {
        return depthPct / 100.0 * (hardMode ? 1.5 : 1.0);
    }

    // State migration: detail equivalent of a legacy sharpness/selectivity pair
    // (both in %; absent parameters default to 50 at the call site).
    inline double detailFromLegacy (double sharpnessPct, double selectivityPct) noexcept
    {
        return std::clamp ((sharpnessPct + selectivityPct) / 2.0, 0.0, 100.0);
    }
} // namespace resonance_suppressor_detail
