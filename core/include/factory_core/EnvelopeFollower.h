#pragma once
//
// factory_core/EnvelopeFollower.h — a small one-pole peak envelope follower with
// independent attack / release ballistics on the rectified input. Header-only,
// JUCE-independent, allocation-free. Used by the Glue mode of the multiband
// enhancer (and reusable by future compressor-style cores).
//
// A finiteness guard makes a single NaN/Inf input self-healing: a non-finite
// sample is ignored (the state is held) rather than latched into the envelope.
//
#include "SmoothingCoeff.h"

#include <cmath>

namespace factory_core
{
    class EnvelopeFollower
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            fs = sampleRate;
            setTimes (attackMs, releaseMs);
            reset();
        }

        void setTimes (double attMs, double relMs) noexcept
        {
            attackMs  = attMs;
            releaseMs = relMs;
            attackCoeff  = coeffForMs (attackMs);
            releaseCoeff = coeffForMs (releaseMs);
        }

        void reset() noexcept { env = 0.0; }

        double process (double x) noexcept
        {
            if (! std::isfinite (x))
                return env;                    // hold on non-finite input (self-heal)
            const double d = std::abs (x);
            const double c = (d > env) ? attackCoeff : releaseCoeff;
            env = c * env + (1.0 - c) * d;
            return env;
        }

        double value() const noexcept { return env; }

    private:
        double coeffForMs (double ms) const noexcept { return onePoleCoeffForMs (ms, fs); }

        double fs          = 44100.0;
        double attackMs    = 5.0;
        double releaseMs   = 150.0;
        double attackCoeff = 0.0;
        double releaseCoeff = 0.0;
        double env         = 0.0;
    };
} // namespace factory_core
