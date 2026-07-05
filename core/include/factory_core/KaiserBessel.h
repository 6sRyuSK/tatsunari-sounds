#pragma once
//
// factory_core/KaiserBessel.h — the zeroth-order modified Bessel function I0(x),
// the single shared implementation behind every Kaiser-window design in the core
// (Oversampler, PolyphaseResampler, LinearPhaseCrossover5). Header-only,
// JUCE-independent, allocation-free.
//
// Power series I0(x) = sum_k ( (x^2/4)^k / (k!)^2 ). The loop cap is 64, but the
// early-out (term < 1e-14 * running-sum) fires far below that for every Kaiser
// beta the core uses (<= 9.0), so the result is bit-identical to the historical
// per-header copies (which capped at 60 or 64 — the cap is never reached).
//
#include <cmath>

namespace factory_core
{
    inline double besselI0 (double x) noexcept
    {
        double s = 1.0, term = 1.0;
        const double y = x * x / 4.0;
        for (int k = 1; k < 64; ++k)
        {
            term *= y / (double) (k * k);
            s += term;
            if (term < 1.0e-14 * s) break;
        }
        return s;
    }
} // namespace factory_core
