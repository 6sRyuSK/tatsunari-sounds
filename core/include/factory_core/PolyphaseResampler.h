#pragma once
//
// factory_core/PolyphaseResampler.h — streaming arbitrary-ratio resampler with a
// band-limiting windowed-sinc (Kaiser) FIR. Drop-in for the same streaming
// contract as factory_core::Resampler (prepare/process, phase + history persist
// across blocks, allocation-free process), but — unlike the Catmull-Rom
// Resampler — it has a real anti-alias / anti-image low-pass, so it is safe on
// rate-conversion paths that wrap a NON-LINEAR section (the NAM amp) whose output
// contains energy above the lower Nyquist.
//
// Design:
//   * Prototype = sinc(2*fc*t) * Kaiser(beta, t / D) over |t| <= D input samples,
//     with cutoff fc = 0.5 * min(1, outRate/inRate) cycles/input-sample so the
//     stopband starts at 0.5 * min(inRate, outRate) — the lower Nyquist — for both
//     decimation and interpolation.
//   * Kaiser beta = 0.1102*(A-8.7) = 7.857 for A = 80 dB stopband attenuation.
//   * D = kHalfTaps = 31 input samples half-width => 63 taps, an ODD length whose
//     group delay D is an integer number of input samples. groupDelayInputSamples()
//     returns D; the wrapper's round-trip latency uses base = D (host + up-stage
//     model delay), matching factory_core::resamplerRoundTripLatency.
//   * Coefficients are a dense prototype table (kDensity samples per input sample,
//     i.e. 512-phase equivalent) built in prepare(); process() reads it with linear
//     interpolation between phases. Every output is normalised by its tap-weight sum
//     so the pass-band amplitude is flat (unity DC gain) at any fractional phase.
//
// Latency is a real causal delay of D input samples: at ratio 1:1, out[k] == in[k-D]
// (the initial output phase starts at -D). An impulse peaks D samples late.
//
// NOTE (buffer sizing, same contract as Resampler): process() emits at most
// outCap samples and DISCARDS any input it could not turn into output within that
// cap, so callers must size outCap >= ceil(n / step) + 2 (step = inRate/outRate)
// so production is never truncated. RateBracket sizes its scratch with ample margin.
//
#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class PolyphaseResampler
    {
    public:
        static constexpr int    kHalfTaps = 31;      // D: half-width in input samples (=> 63 taps)
        static constexpr int    kDensity  = 512;     // prototype samples per input sample (phase resolution)
        static constexpr double kBeta     = 7.857;   // Kaiser beta for ~80 dB stopband

        void prepare (double inRate, double outRate)
        {
            step = (outRate > 0.0) ? inRate / outRate : 1.0;

            // Cutoff (cycles per input sample) at the CENTRE of the transition band:
            // pass-band edge 0.4*min, stop-band edge 0.5*min (min = lower rate), so
            // the -6 dB point sits at 0.45*min and full ~80 dB attenuation is reached
            // by the lower Nyquist — leaving the transition room a cutoff placed
            // exactly at Nyquist would not (a near-unity decimation like 48->44.1 kHz
            // otherwise only reaches ~20 dB just past Nyquist).
            const double fc = 0.45 * std::min (1.0, (inRate > 0.0 ? outRate / inRate : 1.0));

            // Dense prototype over t in [0, D]; symmetric, so store the positive side.
            const int tableMax = kHalfTaps * kDensity;
            table.assign ((size_t) (tableMax + 2), 0.0);
            const double i0b = besselI0 (kBeta);
            for (int j = 0; j <= tableMax; ++j)
            {
                const double t = (double) j / (double) kDensity;    // input-sample offset
                const double win = (t <= (double) kHalfTaps)
                    ? besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - (t / kHalfTaps) * (t / kHalfTaps)))) / i0b
                    : 0.0;
                table[(size_t) j] = sinc (2.0 * fc * t) * win;
            }

            // History covers the widest tap reach: 2*D plus a fractional-phase carry.
            histLen = 2 * kHalfTaps + (int) std::ceil (std::max (1.0, step)) + 8;
            hist.assign ((size_t) histLen, 0.0f);
            reset();
        }

        void reset() noexcept
        {
            std::fill (hist.begin(), hist.end(), 0.0f);
            outPos = -(double) kHalfTaps;   // causal group delay of D input samples
        }

        double ratio() const noexcept { return step; }
        int    groupDelayInputSamples() const noexcept { return kHalfTaps; }

        // Produce as many output samples as possible from `n` input samples (up to
        // outCap). The fractional phase and the last histLen inputs persist across
        // calls so a stream resamples seamlessly. Allocation-free.
        int process (const float* in, int n, float* out, int outCap) noexcept
        {
            if (n <= 0) return 0;
            const int tableMax = kHalfTaps * kDensity;

            int produced = 0;
            while (produced < outCap)
            {
                const int iRight = (int) std::floor (outPos + (double) kHalfTaps);
                if (iRight > n - 1) break;                       // need more input
                const int iLeft = (int) std::ceil (outPos - (double) kHalfTaps);

                double acc = 0.0, wsum = 0.0;
                for (int i = iLeft; i <= iRight; ++i)
                {
                    const double tau = std::abs (outPos - (double) i);
                    const double idx = tau * (double) kDensity;
                    const int    j   = (int) idx;
                    double w = 0.0;
                    if (j < tableMax)
                    {
                        const double fr = idx - (double) j;
                        w = table[(size_t) j] + fr * (table[(size_t) (j + 1)] - table[(size_t) j]);
                    }
                    const float xi = (i < 0) ? hist[(size_t) (i + histLen)] : in[i];
                    acc  += w * (double) xi;
                    wsum += w;
                }
                out[produced++] = (float) (wsum != 0.0 ? acc / wsum : 0.0);
                outPos += step;
            }

            // Retain the last histLen inputs of [hist ++ in] as the next left context.
            // Ascending k is in-place safe: for c<0 the read index (n+k) is > write (k).
            for (int k = 0; k < histLen; ++k)
            {
                const int c = n - histLen + k;
                hist[(size_t) k] = (c >= 0) ? in[c] : hist[(size_t) (c + histLen)];
            }
            outPos -= (double) n;
            return produced;
        }

    private:
        static double sinc (double x) noexcept
        {
            if (std::abs (x) < 1.0e-9) return 1.0;
            const double px = 3.14159265358979323846 * x;
            return std::sin (px) / px;
        }

        static double besselI0 (double x) noexcept
        {
            double sum = 1.0, term = 1.0;
            const double y = x * x / 4.0;
            for (int k = 1; k < 60; ++k)
            {
                term *= y / (double) (k * k);
                sum  += term;
                if (term < 1.0e-14 * sum) break;
            }
            return sum;
        }

        std::vector<double> table;      // prototype, positive side, kDensity/input-sample
        std::vector<float>  hist;       // left context: coordinates [-histLen .. -1]
        double step = 1.0, outPos = 0.0;
        int    histLen = 0;
    };
} // namespace factory_core
