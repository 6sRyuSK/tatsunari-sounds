#pragma once
//
// factory_core/Biquad.h — shared DSP primitive: a normalized biquad and the
// RBJ peaking-EQ coefficient design. Header-only, JUCE-independent, and safe to
// exercise headless. No allocation, locks, or syscalls in the audio path.
//
#include <cmath>
#include <limits>

namespace factory_core
{
    // Flush a subnormal (denormalized) value to zero with pure arithmetic that is
    // INDEPENDENT of the CPU's FTZ/DAZ rounding mode. A stable IIR fed a decaying
    // tail into digital silence otherwise pins its feedback state in the subnormal
    // range, where every multiply is microcoded and costs ~80x a normal op. Only
    // values that are ALREADY subnormal (|x| < the smallest normal double) are
    // touched, so every normal-range result — and thus every existing oracle — is
    // bit-identical; the guard just stops a decaying feedback node from getting
    // stuck churning denormals. See docs/regression-policy.md class V (denormal storm).
    inline double flushDenormalToZero (double x) noexcept
    {
        return (std::abs (x) < std::numeric_limits<double>::min()) ? 0.0 : x;
    }

    // Biquad coefficients, already normalized so that a0 == 1.
    struct BiquadCoeffs
    {
        double b0 { 1.0 };
        double b1 { 0.0 };
        double b2 { 0.0 };
        double a1 { 0.0 };
        double a2 { 0.0 };
    };

    // RBJ "peaking EQ" (Audio EQ Cookbook), Q parameterization.
    //   A     = 10^(gainDb / 40)
    //   w0    = 2*pi*freqHz / sampleRate
    //   alpha = sin(w0) / (2*Q)
    // Coefficients are normalized by a0 before returning.
    inline BiquadCoeffs designPeaking (double freqHz, double gainDb, double Q, double sampleRate) noexcept
    {
        constexpr double pi = 3.14159265358979323846;

        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * pi * freqHz / sampleRate;
        const double cosw0 = std::cos (w0);
        const double sinw0 = std::sin (w0);
        const double alpha = sinw0 / (2.0 * Q);

        const double b0 = 1.0 + alpha * A;
        const double b1 = -2.0 * cosw0;
        const double b2 = 1.0 - alpha * A;
        const double a0 = 1.0 + alpha / A;
        const double a1 = -2.0 * cosw0;
        const double a2 = 1.0 - alpha / A;

        BiquadCoeffs c;
        c.b0 = b0 / a0;
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }

    // Single-channel biquad in transposed direct form II. State is kept in
    // double so low-frequency filters stay accurate even with float audio.
    class Biquad
    {
    public:
        void setCoeffs (const BiquadCoeffs& c) noexcept { coeffs = c; }

        void reset() noexcept { z1 = 0.0; z2 = 0.0; }

        inline double processSample (double x) noexcept
        {
            const double y = coeffs.b0 * x + z1;
            // Flush the feedback state (z1, z2) when it decays into the subnormal
            // range, so a tail fading into silence cannot pin the filter in a
            // denormal storm on hosts that do not set FTZ around the callback.
            // No effect on any normal-range signal (bit-identical): see
            // flushDenormalToZero above.
            z1 = flushDenormalToZero (coeffs.b1 * x - coeffs.a1 * y + z2);
            z2 = flushDenormalToZero (coeffs.b2 * x - coeffs.a2 * y);
            return y;
        }

        template <typename Sample>
        void process (Sample* samples, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
                samples[i] = static_cast<Sample> (processSample (static_cast<double> (samples[i])));
        }

    private:
        BiquadCoeffs coeffs {};
        double z1 { 0.0 };
        double z2 { 0.0 };
    };
} // namespace factory_core
