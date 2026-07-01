#pragma once
//
// factory_core/Resampler.h — streaming arbitrary-ratio resampler (Catmull-Rom
// 4-point cubic). Used to run the NAM section at a fixed 48 kHz regardless of the
// host rate. Push input samples and pull all output samples currently producible;
// the fractional phase and the 4-sample history persist across calls so a stream
// resamples seamlessly block to block. Allocation-free in process().
//
// Interpolating at the centre of the 4-tap window (between s1 and s2) gives a fixed
// group delay of 2 input samples — deterministic, which lets the wrapper report an
// exact latency and align the dry path.
//
#include <cstddef>

namespace factory_core
{
    class Resampler
    {
    public:
        // step = inputRate / outputRate (input-time advance per output sample).
        void prepare (double inRate, double outRate) noexcept
        {
            step = (outRate > 0.0) ? inRate / outRate : 1.0;
            reset();
        }

        // frac starts at 1.0 so the first output consumes an input sample before
        // emitting; this makes the group delay a clean 2 input samples (see the 1:1
        // exact-delay test) rather than 3.
        void reset() noexcept { s0 = s1 = s2 = s3 = 0.0f; frac = 1.0; }

        double ratio() const noexcept { return step; }
        int groupDelayInputSamples() const noexcept { return 2; }

        // Produce as many output samples as possible from `n` input samples, writing
        // up to `outCap` outputs; returns the count produced. Input not yet needed is
        // retained via the phase accumulator for the next call. Size `outCap` >=
        // ceil(n / step) + 2 so production is never truncated.
        int process (const float* in, int n, float* out, int outCap) noexcept
        {
            int produced = 0, i = 0;
            while (produced < outCap)
            {
                while (frac >= 1.0)
                {
                    if (i >= n) return produced;          // need more input
                    s0 = s1; s1 = s2; s2 = s3; s3 = in[i++];
                    frac -= 1.0;
                }
                const float t  = (float) frac;
                const float a0 = -0.5f * s0 + 1.5f * s1 - 1.5f * s2 + 0.5f * s3;
                const float a1 =         s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
                const float a2 = -0.5f * s0             + 0.5f * s2;
                out[produced++] = ((a0 * t + a1) * t + a2) * t + s1;
                frac += step;
            }
            return produced;
        }

    private:
        double step = 1.0, frac = 0.0;
        float  s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    };
} // namespace factory_core
