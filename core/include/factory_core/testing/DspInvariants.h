#pragma once
//
// factory_core/testing/DspInvariants.h — shared, header-only, JUCE-independent
// helpers for the headless DSP tests (every plugins/<slug>/tests/dsp_test.cpp).
//
// This file exists to make the regression classes catalogued in
// docs/regression-policy.md structurally hard to reintroduce. Concretely it
// centralises the two things that individual tests kept getting wrong:
//
//   1. The **sample-rate matrix**. Issue #33: seven of eight plugins silently
//      stopped their default rate loop at 96 kHz, so every high-rate (88.2 /
//      176.4 / 192 kHz) regression went ungated. `kStandardSampleRates` and
//      `sampleRatesFromArgs()` are the single source of truth for the rate set
//      — a test that uses them cannot narrow the matrix by hand.
//
//   2. The **numeric-safety invariants** (finite, bounded, non-increasing tail,
//      resolution-follows-rate). Issues #28/#29/#36/#38 were all "a feedback or
//      resonant path grows without bound / turns to NaN"; #16 was "a fixed FFT
//      order loses resolution at high rates". These are objective, oracle-free
//      invariants — assert them directly rather than re-deriving expected values
//      from the implementation under test.
//
// Only test code includes this header; no plugin/runtime source does. It adds no
// DSP and changes no behaviour.
//
#include "factory_core/StftResolution.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace factory_core::testing
{
    // ---- Sample-rate matrix (CLAUDE.md hard rule) ---------------------------
    //
    // The full standard set every plugin's DSP must be tested across. Do NOT
    // hand-write a narrower literal in a test — use this, or drop a rate only
    // with an explicit, reviewed reason (loosening a gate is "Ask a human").
    inline const std::vector<double>& kStandardSampleRates()
    {
        static const std::vector<double> rates {
            44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
        };
        return rates;
    }

    // Parse the CTest per-rate argument: `dsp_test <fs>` runs one rate (CMake
    // registers one test per rate so failures name the rate), while no argument
    // runs the full matrix. Centralised so no test can accidentally ship a
    // reduced default set again (issue #33).
    inline std::vector<double> sampleRatesFromArgs (int argc, char** argv)
    {
        if (argc > 1)
            return { std::atof (argv[1]) };
        return kStandardSampleRates();
    }

    // ---- Numeric-safety invariants ------------------------------------------

    // True iff every sample is finite (no NaN, no Inf). Issue #38: one Inf/NaN
    // in a feedback node corrupts the state permanently, so "stayed finite over
    // a long hold" is a first-class assertion, not an afterthought.
    inline bool allFinite (const std::vector<double>& x)
    {
        for (double v : x)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    inline double peakAbs (const std::vector<double>& x)
    {
        double p = 0.0;
        for (double v : x)
            p = std::max (p, std::abs (v));
        return p;
    }

    // True iff NO element is subnormal (denormalized). A stable feedback node fed
    // a decaying tail into silence must not get pinned in the subnormal range,
    // where every op is microcoded and costs ~80x a normal one — two such nodes in
    // series (e.g. two resonance-suppressor instances, stage 1's tail underflowing
    // into stage 2) then run catastrophically over the real-time budget on any
    // host that does not force FTZ/DAZ around the callback. The DSP cores are
    // FP-mode-agnostic, so this must hold from pure arithmetic, independent of the
    // CPU rounding mode. Pair with allFinite(): finite AND subnormal-free == every
    // value is a normal double or an exact zero. (regression-policy class V.)
    inline bool noSubnormals (const std::vector<double>& x)
    {
        for (double v : x)
            if (std::fpclassify (v) == FP_SUBNORMAL)
                return false;
        return true;
    }

    inline double windowEnergy (const std::vector<double>& x, std::size_t start, std::size_t len)
    {
        double e = 0.0;
        for (std::size_t i = start; i < start + len && i < x.size(); ++i)
            e += x[i] * x[i];
        return e;
    }

    // Feedback stability without an oracle: drive `process` (one mono sample in
    // -> one mono sample out) with a unit impulse, then verify the impulse
    // response energy is non-increasing window-over-window — i.e. the effective
    // loop gain is < 1 and the tail cannot run away. This is the invariant that
    // issues #28/#29 violated (freeze+shimmer / sparse-overlap grain gain drove
    // loop gain past 1); test the *sparse* / worst-case setting, not just the
    // COLA / 50%-overlap point that happens to sit at exactly unity (#34).
    //
    // `tolerance` allows a small per-window rise for modulation/ripple; keep it
    // tight (e.g. 1.05) — a genuinely divergent path blows past any sane bound.
    template <typename Process>
    bool impulseResponseNonIncreasing (Process&& process,
                                       double    sampleRate,
                                       double    tailSeconds = 4.0,
                                       double    windowSeconds = 0.25,
                                       double    tolerance = 1.05)
    {
        const std::size_t total  = (std::size_t) (tailSeconds * sampleRate);
        const std::size_t window = (std::size_t) (windowSeconds * sampleRate);
        if (window == 0 || total <= window)
            return true;

        std::vector<double> y (total);
        for (std::size_t n = 0; n < total; ++n)
            y[n] = process (n == 0 ? 1.0 : 0.0);

        if (! allFinite (y))
            return false;

        double prev = windowEnergy (y, 0, window);
        for (std::size_t start = window; start + window <= total; start += window)
        {
            const double cur = windowEnergy (y, start, window);
            if (cur > prev * tolerance)
                return false;             // tail growing -> loop gain >= 1
            prev = std::max (prev, 1e-300);
        }
        return true;
    }

    // ---- Resolution-follows-sample-rate invariants (issue #16) --------------
    //
    // Any analysis FFT/STFT order must come from fftOrderForSampleRate(), and
    // the resulting resolution must stay in range at the *top* rate — a fixed
    // order silently degrades (order 11 => 94 Hz bins at 192 kHz). Assert both
    // the bin width (fs / N) and the window length (N / fs) so the class of bug
    // #16 cannot reappear in any plugin's analyser.
    inline double binWidthHz (double sampleRate, int fftOrder)
    {
        return sampleRate / (double) (1 << fftOrder);
    }

    inline double windowLengthSec (double sampleRate, int fftOrder)
    {
        return (double) (1 << fftOrder) / sampleRate;
    }

    // At `sampleRate`, the order picked by fftOrderForSampleRate() must keep the
    // bin width no coarser than `maxBinHz` and the window no shorter than
    // `minWindowSec`. Defaults track the ~23 Hz / ~43 ms reference the core
    // holds through 192 kHz.
    inline bool resolutionFollowsSampleRate (double sampleRate,
                                             double maxBinHz     = 100.0,
                                             double minWindowSec = 0.010)
    {
        const int order = fftOrderForSampleRate (sampleRate);
        return binWidthHz (sampleRate, order) <= maxBinHz
            && windowLengthSec (sampleRate, order) >= minWindowSec;
    }
} // namespace factory_core::testing
