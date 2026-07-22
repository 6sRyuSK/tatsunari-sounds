#pragma once
//
// factory_core/VariPolyphaseResampler.h -- a VARIABLE-ratio band-limited
// streaming resampler. Same Kaiser windowed-sinc mathematics as
// factory_core::PolyphaseResampler (kHalfTaps = 31, kDensity = 512,
// kBeta = 7.857 for ~80 dB stopband), but the conversion ratio may change
// BETWEEN (and smoothly ACROSS) process() calls: setTargetRatio() is
// allocation-free and preserves the fractional phase and the input history,
// and the actual step ramps toward the target per output sample, so a CLOCK
// sweep resamples without zipper or clicks. The existing fixed-ratio
// PolyphaseResampler bakes its anti-alias cutoff into the prototype table at
// prepare() time and is NOT modified by this class; this is a separate,
// composable primitive (approved 2026-07-12 for the Madoromi engine and the
// future shared tape-multi CLOCK module).
//
// Design contract (tests depend on these exact formulas):
//   * Prototype table (built ONCE in prepare(), never on the audio thread):
//       proto(t) = sinc(2 * kFc1 * t) * Kaiser(kBeta, t / kHalfTaps),
//       t in [0, kHalfTaps] input samples, kFc1 = 0.45, sampled at kDensity
//       points per input sample and read back with linear interpolation.
//     This is the ratio-1 kernel of PolyphaseResampler (pass-band edge at
//     0.4, stop-band edge at 0.5 of the input Nyquist, -6 dB at 0.45).
//   * Kernel index-stretch (the variable-ratio mechanism): for the current
//     step s (= inRate / outRate), let stretch = max(1, s). A tap at offset
//     tau input samples from the output phase has weight
//       w(tau) = proto(tau / stretch),   |tau| <= kHalfTaps * stretch,
//     and every output is normalised by its tap-weight sum, so:
//       - effective cutoff = kFc1 / stretch cycles per input sample
//                          = 0.45 * min(1, outRate / inRate),
//         i.e. the -6 dB point tracks the ratio CONTINUOUSLY and the full
//         ~80 dB attenuation is reached by the lower Nyquist, exactly like
//         the fixed-ratio prototype placement;
//       - pass-band gain is unity (tap-sum normalisation) at any phase and
//         any stretch; DC gain is exactly 1.
//     Expected stopband for tones above the lower Nyquist: ~75..80 dB
//     (Kaiser A = 80 dB minus the dense-table linear-interpolation error).
//     Gate tests conservatively at >= 40 dB.
//   * Step ramp (zipper-free sweeps): after EACH produced output sample,
//       step += kRampAlpha * (targetStep - step),   kRampAlpha = 1/512,
//     i.e. a one-pole ramp with a time constant of 512 output samples.
//     setTargetRatio() only moves the target; the caller provides the
//     musical smoothing (e.g. Madoromi's tau = 100 ms CLOCK smoother) and
//     this ramp removes the per-call staircase.
//   * Group delay (latency): the kernel is centred, so the causal delay is
//       D(r) = kHalfTaps * max(1, r) input samples at ratio r
//     (groupDelayInputSamples(r) below). reset() starts the output phase at
//     -D(current ratio). When the ratio changes mid-stream the in-flight
//     delay drifts accordingly; a wrapper that needs a CONSTANT reported
//     latency must absorb the drift with an output FIFO prefill sized for
//     the worst-case swing (see factory_core::Madoromi's latency contract).
//   * Streaming contract (same as PolyphaseResampler): the fractional phase
//     and the last histLen inputs persist across calls; process() emits at
//     most outCap samples and DISCARDS input it could not convert within
//     that cap, so callers must size outCap >= ceil(n / minExpectedStep) + 2.
//   * prepare(inRate, outRate, maxRatio) allocates for the WORST CASE ratio
//     the caller will ever set (maxRatio >= any future inRate/outRate);
//     setTargetRatio() clamps into [kMinRatio, maxRatio]. reset() and
//     process() are allocation/lock/syscall-free and noexcept.
//
#include "KaiserBessel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class VariPolyphaseResampler
    {
    public:
        static constexpr int    kHalfTaps  = 31;      // ratio-1 half-width D (input samples)
        static constexpr int    kDensity   = 512;     // prototype samples per input sample
        static constexpr double kBeta      = 7.857;   // Kaiser beta for ~80 dB stopband
        static constexpr double kFc1       = 0.45;    // ratio-1 cutoff (cycles/input sample)
        static constexpr double kRampAlpha = 1.0 / 512.0; // per-output step ramp pole
        static constexpr double kMinRatio  = 1.0e-3;  // setTargetRatio lower clamp

        // maxRatio: the largest inRate/outRate this instance will ever be asked
        // to run at (history and tap reach are sized for it here, once).
        void prepare (double inRate, double outRate, double maxRatio)
        {
            const double initial = (outRate > 0.0 && inRate > 0.0) ? inRate / outRate : 1.0;
            maxStep    = std::max ({ 1.0, maxRatio, initial });
            targetStep = std::clamp (initial, kMinRatio, maxStep);

            // Dense ratio-1 prototype over t in [0, kHalfTaps]; symmetric, so the
            // positive side is stored. Identical maths to PolyphaseResampler.
            const int tableMax = kHalfTaps * kDensity;
            table.assign ((size_t) (tableMax + 2), 0.0);
            const double i0b = besselI0 (kBeta);
            for (int j = 0; j <= tableMax; ++j)
            {
                const double t = (double) j / (double) kDensity;   // input-sample offset
                const double win = (t <= (double) kHalfTaps)
                    ? besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - (t / kHalfTaps) * (t / kHalfTaps)))) / i0b
                    : 0.0;
                table[(size_t) j] = sinc (2.0 * kFc1 * t) * win;
            }

            // Worst-case tap reach is 2 * D * stretchMax plus the fractional carry.
            const double stretchMax = std::max (1.0, maxStep);
            histLen = 2 * (int) std::ceil ((double) kHalfTaps * stretchMax)
                    + (int) std::ceil (maxStep) + 8;
            hist.assign ((size_t) histLen, 0.0f);
            reset();
        }

        // Deterministic: zeroes the history, snaps the ramp to the target ratio
        // and restarts the output phase at the causal delay -D(ratio).
        void reset() noexcept
        {
            std::fill (hist.begin(), hist.end(), 0.0f);
            step   = targetStep;
            outPos = -(double) kHalfTaps * std::max (1.0, step);
        }

        // Allocation-free; may be called between process() calls on the audio
        // thread. Non-finite values are ignored (house rule).
        void setTargetRatio (double inOverOut) noexcept
        {
            if (! std::isfinite (inOverOut)) return;
            targetStep = std::clamp (inOverOut, kMinRatio, maxStep);
        }

        double ratio()       const noexcept { return step; }        // current (ramping)
        double targetRatio() const noexcept { return targetStep; }
        double maxRatioPrepared() const noexcept { return maxStep; }

        // Causal group delay in input samples at ratio r (pure).
        static double groupDelayInputSamples (double r) noexcept
        {
            return (double) kHalfTaps * std::max (1.0, r);
        }

        // Produce as many output samples as possible from `n` inputs (up to
        // outCap). Phase, ramp state and history persist across calls.
        int process (const float* in, int n, float* out, int outCap) noexcept
        {
            if (n <= 0) return 0;
            const int tableMax = kHalfTaps * kDensity;

            int produced = 0;
            while (produced < outCap)
            {
                const double stretch = std::max (1.0, step);
                const double reach   = (double) kHalfTaps * stretch;
                const int iRight = (int) std::floor (outPos + reach);
                if (iRight > n - 1) break;                        // need more input
                const int iLeft  = (int) std::ceil (outPos - reach);

                const double idxScale = (double) kDensity / stretch; // index-stretch
                double acc = 0.0, wsum = 0.0;
                for (int i = iLeft; i <= iRight; ++i)
                {
                    const double tau = std::abs (outPos - (double) i);
                    const double idx = tau * idxScale;
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
                step   += kRampAlpha * (targetStep - step);       // per-output ramp
            }

            // Retain the last histLen inputs of [hist ++ in] as the left context.
            // Ascending k is in-place safe (read index > write index for c < 0).
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

        std::vector<double> table;      // ratio-1 prototype, positive side
        std::vector<float>  hist;       // left context: coordinates [-histLen .. -1]
        double step = 1.0, targetStep = 1.0, maxStep = 1.0, outPos = 0.0;
        int    histLen = 0;
    };
} // namespace factory_core
