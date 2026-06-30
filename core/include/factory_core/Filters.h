#pragma once
//
// factory_core/Filters.h — RBJ "Audio EQ Cookbook" biquad designs for the band
// types used by the parametric EQ: bell (peaking), low/high shelf, high/low
// pass. All return coefficients normalized so a0 == 1. Header-only,
// JUCE-independent, evaluated/tested in the z-domain.
//
#include "Biquad.h"

#include <cmath>

namespace factory_core
{
    enum class BandType
    {
        Bell = 0,
        LowShelf,
        HighShelf,
        HighPass,
        LowPass
    };

    inline BiquadCoeffs designFilter (BandType type, double freqHz, double gainDb,
                                      double Q, double sampleRate) noexcept
    {
        // Bell reuses the peaking design.
        if (type == BandType::Bell)
            return designPeaking (freqHz, gainDb, Q, sampleRate);

        constexpr double pi = 3.14159265358979323846;
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * pi * freqHz / sampleRate;
        const double cw    = std::cos (w0);
        const double sw    = std::sin (w0);
        const double alpha = sw / (2.0 * Q);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type)
        {
            case BandType::LowShelf:
            {
                const double tsa = 2.0 * std::sqrt (A) * alpha;
                b0 =      A * ((A + 1.0) - (A - 1.0) * cw + tsa);
                b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
                b2 =      A * ((A + 1.0) - (A - 1.0) * cw - tsa);
                a0 =          (A + 1.0) + (A - 1.0) * cw + tsa;
                a1 = -2.0 *   ((A - 1.0) + (A + 1.0) * cw);
                a2 =          (A + 1.0) + (A - 1.0) * cw - tsa;
                break;
            }
            case BandType::HighShelf:
            {
                const double tsa = 2.0 * std::sqrt (A) * alpha;
                b0 =      A * ((A + 1.0) + (A - 1.0) * cw + tsa);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
                b2 =      A * ((A + 1.0) + (A - 1.0) * cw - tsa);
                a0 =          (A + 1.0) - (A - 1.0) * cw + tsa;
                a1 =  2.0 *   ((A - 1.0) - (A + 1.0) * cw);
                a2 =          (A + 1.0) - (A - 1.0) * cw - tsa;
                break;
            }
            case BandType::HighPass:
            {
                b0 =  (1.0 + cw) * 0.5;
                b1 = -(1.0 + cw);
                b2 =  (1.0 + cw) * 0.5;
                a0 =   1.0 + alpha;
                a1 =  -2.0 * cw;
                a2 =   1.0 - alpha;
                break;
            }
            case BandType::LowPass:
            {
                b0 =  (1.0 - cw) * 0.5;
                b1 =   1.0 - cw;
                b2 =  (1.0 - cw) * 0.5;
                a0 =   1.0 + alpha;
                a1 =  -2.0 * cw;
                a2 =   1.0 - alpha;
                break;
            }
            case BandType::Bell:
            default:
                break;
        }

        BiquadCoeffs c;
        c.b0 = b0 / a0;
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }

    // --- Variable-slope Butterworth high/low-pass (cascaded biquads) --------
    //
    // A 12*numStages dB/oct high/low-pass is a cascade of `numStages` second-
    // order sections (overall order 2*numStages). Each section is the RBJ
    // HP/LP design at the same cutoff but with its own Butterworth Q so the
    // cascade is maximally flat with the overall -3 dB point at the cutoff.
    //
    // Resonance: the peak (last) section's Q is scaled by userQ / (1/sqrt2),
    // so userQ == 0.7071 gives a flat Butterworth and a larger userQ adds a
    // resonant peak at the cutoff. For numStages == 1 this reduces exactly to
    // designFilter(type, f, 0, userQ, Fs) — the plain 12 dB/oct biquad.

    // Butterworth section Q for `stage` (0-based) of an order-2*numStages filter.
    inline double butterworthSectionQ (int stage, int numStages) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        return 1.0 / (2.0 * std::cos ((2.0 * stage + 1.0) * pi / (4.0 * numStages)));
    }

    // Coefficients for one section of the HP/LP cascade. `type` must be
    // HighPass or LowPass; `gain` is unused by those designs.
    inline BiquadCoeffs designHpLpStage (BandType type, double freqHz, double userQ,
                                         int stage, int numStages, double sampleRate) noexcept
    {
        constexpr double butterQ = 0.70710678118654752440; // 1/sqrt(2)
        double q = butterworthSectionQ (stage, numStages);
        if (stage == numStages - 1)
            q *= userQ / butterQ; // resonance on the peak section
        return designFilter (type, freqHz, 0.0, q, sampleRate);
    }
} // namespace factory_core
