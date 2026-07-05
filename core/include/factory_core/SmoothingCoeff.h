#pragma once
//
// factory_core/SmoothingCoeff.h — the two one-pole smoothing-coefficient formulas
// shared across the dynamics and enhancer cores. Header-only, JUCE-independent,
// allocation-free. Both are exact-value helpers: callers pass their existing rate
// / tau expression unchanged, so substituting a copy for a call is bit-identical.
//
//   onePoleCoeffForMs     — the ballistics-style pole for a time constant in ms at
//                           a given sample (or frame) rate, y += (1-c)*(x-y) with
//                           c = exp(-1/(ms*1e-3 * rate)); rate<=0 or ms<=0 -> 0.
//   onePoleAlphaForTauSamples — the smoother-style rate alpha 1 - exp(-1/tau) for a
//                           time constant already expressed in samples (the caller
//                           keeps its own clamp on tau, e.g. std::max(1.0, ...)).
//
#include <cmath>

namespace factory_core
{
    inline double onePoleCoeffForMs (double ms, double rate) noexcept
    {
        const double t = ms * 0.001;
        if (t <= 0.0 || rate <= 0.0) return 0.0;
        return std::exp (-1.0 / (t * rate));
    }

    inline double onePoleAlphaForTauSamples (double tauSamples) noexcept
    {
        return 1.0 - std::exp (-1.0 / tauSamples);
    }
} // namespace factory_core
