#pragma once
//
// factory_core/OnePole.h — a one-pole filter usable as a lowpass (lp) or, by
// complement, a highpass (hp). Used for reverb damping and shimmer-path tone
// shaping. Header-only, JUCE-independent, allocation-free.
//
#include <algorithm>
#include <cmath>

namespace factory_core
{
    class OnePole
    {
    public:
        void setCutoff (double hz, double sampleRate) noexcept
        {
            constexpr double pi = 3.14159265358979323846;
            const double fc = std::clamp (hz, 1.0, 0.49 * sampleRate);
            a = std::exp (-2.0 * pi * fc / sampleRate);
        }

        void reset() noexcept { z = 0.0; }

        // One-pole lowpass.
        double lp (double x) noexcept
        {
            z = (1.0 - a) * x + a * z;
            return z;
        }

        // Complementary highpass (x minus the lowpass).
        double hp (double x) noexcept { return x - lp (x); }

    private:
        double a = 0.0;
        double z = 0.0;
    };
} // namespace factory_core
