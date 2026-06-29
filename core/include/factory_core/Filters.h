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
} // namespace factory_core
