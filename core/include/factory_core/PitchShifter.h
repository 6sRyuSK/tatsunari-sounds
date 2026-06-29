#pragma once
//
// factory_core/PitchShifter.h — a delay-line crossfade ("rotating head") pitch
// shifter. Two read heads offset by half a window sweep the delay buffer at a
// rate set by the pitch ratio; their Hann windows sum to unity so the crossfade
// is seamless. No FFT, allocation-free in process. Header-only, headless.
//
// A read head's absolute buffer position advances at `ratio` samples per output
// sample, so the output frequency is the input frequency times `ratio`
// (ratio 2 = +12 semitones).
//
#include "DelayLine.h"

#include <cmath>

namespace factory_core
{
    class PitchShifter
    {
    public:
        void prepare (double sampleRate, double windowMs = 80.0)
        {
            fs = sampleRate;
            window = std::max (8.0, windowMs * 1.0e-3 * fs);
            buffer.prepare ((int) (window * 2.0) + 8);
            reset();
        }

        void reset() noexcept
        {
            buffer.reset();
            phase = window * 0.5;
        }

        void setRatio (double r) noexcept { ratio = r; }
        static double semitonesToRatio (double semis) noexcept { return std::pow (2.0, semis / 12.0); }

        double process (double x) noexcept
        {
            buffer.write (x);

            const double d1 = phase;
            double d2 = phase + window * 0.5;
            if (d2 >= window) d2 -= window;

            const double w1 = 0.5 - 0.5 * std::cos (2.0 * kPi * d1 / window);
            const double w2 = 0.5 - 0.5 * std::cos (2.0 * kPi * d2 / window);

            const double out = w1 * buffer.readInterpolated (d1) + w2 * buffer.readInterpolated (d2);

            phase -= (ratio - 1.0);
            while (phase < 0.0)        phase += window;
            while (phase >= window)    phase -= window;
            return out;
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;

        double fs = 44100.0;
        double window = 3528.0;
        double ratio = 2.0;
        double phase = 0.0;
        DelayLine buffer;
    };
} // namespace factory_core
