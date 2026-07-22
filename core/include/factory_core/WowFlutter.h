#pragma once
//
// factory_core/WowFlutter.h -- tape-style pitch wobble: two sine LFOs (wow =
// slow / deep, flutter = fast / shallow) modulate the read position of a short
// delay line. Composes DelayLine. Header-only, JUCE-independent; prepare()
// allocates, everything on the audio path does not (no locks, no syscalls).
//
// Contract (tests' oracles depend on these exact formulas):
//   - prepare(fs, numChannels):
//       centerDelaySamples = round(kCenterDelayMs * fs / 1000)   (an INTEGER
//         sample count, so at depth == 0 the linear interpolation degenerates
//         to an exact copy delayed by centerDelaySamples)
//       delay-line length  = centerDelaySamples
//           + ceil((kMaxWowDepthMs + kMaxFlutterDepthMs) * fs / 1000) + 8
//         (worst-case allocation; the modulated delay is NEVER clamped while
//         parameters are in range)
//   - LFO phases: double radians; reset() -> 0 (sin(0) = 0, so the initial
//     delay is exactly center). Each sample the delay D is evaluated at the
//     CURRENT phases, then phase += 2*pi*rate/fs and is wrapped into
//     [0, 2*pi). Both channels SHARE the same phases (one mechanical
//     transport); the delay lines are per channel.
//   - Per sample:
//       D = centerDelaySamples + (wowDepthMsSm * sin(wowPhase)
//             + flutterDepthMsSm * sin(flutterPhase)) * fs / 1000
//     and per channel: write(x) FIRST, then readInterpolated(D). The minimum
//     D is (kCenterDelayMs - kMaxWowDepthMs - kMaxFlutterDepthMs) ms
//     = 1.55 ms of samples > 0, so the write-then-read order is safe.
//   - Depths: setWowDepth01 / setFlutterDepth01 clamp to [0, 1]; depth in ms
//     = value * kMaxWowDepthMs / kMaxFlutterDepthMs, smoothed by a 20 ms
//     one-pole (kParamSmoothMs). The depth smoothers advance once at the top
//     of each tick(), before D is evaluated; reset() snaps them to their
//     targets. Rates: setWowRateHz / setFlutterRateHz clamp to
//     [kMinRateHz, kMaxRateHz] and apply unsmoothed -- the phase is continuous
//     across a rate change, so D stays continuous and cannot click. Non-finite
//     values are ignored (the previous target is kept).
//   - Input finite guard: a non-finite input sample is written as 0.
//   - reset(): clears the delay lines, phases -> 0, depth smoothers snap to
//     their targets.
//
// Math (test rationale): a sine-modulated delay is phase modulation. For an
// input sin(2*pi*f0*t) and one LFO at f_m with depth depthMs, the modulation
// index is
//     beta = 2*pi * f0 * depthMs / 1000        (sample-rate INDEPENDENT)
// and the output spectrum has Bessel lines J_k(beta) at f0 +- k*f_m. With
// both LFOs active the carrier amplitude scales as
// J0(beta_wow) * J0(beta_flutter). Instantaneous pitch ratio = 1 - dD/dt
// (D and t in samples); maximum deviation = 2*pi * f_m * depthMs / 1000.
//
// Composition API (used by the Surikire engine): call tick() exactly once per
// sample frame, then processSample(ch, x) once for each prepared channel.
// process() is the equivalent in-place block form. prepare() must have been
// called before any processing.
//
#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "SmoothingCoeff.h"

namespace factory_core
{
    class WowFlutter
    {
    public:
        static constexpr int    kMaxChannels          = 2;
        static constexpr double kCenterDelayMs        = 12.0;
        static constexpr double kMaxWowDepthMs        = 10.0;
        static constexpr double kMaxFlutterDepthMs    = 0.45;
        static constexpr double kDefaultWowRateHz     = 0.55;
        static constexpr double kDefaultFlutterRateHz = 9.73;
        static constexpr double kMinRateHz            = 0.05;
        static constexpr double kMaxRateHz            = 16.0;
        static constexpr double kParamSmoothMs        = 20.0;

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            center = (int) std::llround (kCenterDelayMs * fs / 1000.0);
            const int modSpan =
                (int) std::ceil ((kMaxWowDepthMs + kMaxFlutterDepthMs) * fs / 1000.0);
            for (int ch = 0; ch < channels; ++ch)
                line[ch].prepare (center + modSpan + 8);

            paramAlpha = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);
            updatePhaseIncrements();
            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
                line[ch].reset();
            wowPhase     = 0.0;
            flutterPhase = 0.0;
            wowDepthMsSm     = wowDepth01 * kMaxWowDepthMs;
            flutterDepthMsSm = flutterDepth01 * kMaxFlutterDepthMs;
            delaySamples = (double) center;
        }

        // -- parameters (call on the audio thread between process calls) --------
        // Non-finite values are ignored (the previous target is kept).
        void setWowDepth01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            wowDepth01 = std::clamp (v, 0.0, 1.0);
        }

        void setFlutterDepth01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            flutterDepth01 = std::clamp (v, 0.0, 1.0);
        }

        void setWowRateHz (double hz) noexcept
        {
            if (! std::isfinite (hz)) return;
            wowRateHz = std::clamp (hz, kMinRateHz, kMaxRateHz);
            updatePhaseIncrements();
        }

        void setFlutterRateHz (double hz) noexcept
        {
            if (! std::isfinite (hz)) return;
            flutterRateHz = std::clamp (hz, kMinRateHz, kMaxRateHz);
            updatePhaseIncrements();
        }

        int centerDelaySamples() const noexcept { return center; }

        // -- per-sample composition API ------------------------------------------
        // Advance the shared LFO phases and depth smoothers, and evaluate the
        // modulated delay for this sample frame. Call once per frame.
        void tick() noexcept
        {
            wowDepthMsSm     += paramAlpha * (wowDepth01 * kMaxWowDepthMs         - wowDepthMsSm);
            flutterDepthMsSm += paramAlpha * (flutterDepth01 * kMaxFlutterDepthMs - flutterDepthMsSm);

            delaySamples = (double) center
                         + (wowDepthMsSm * std::sin (wowPhase)
                            + flutterDepthMsSm * std::sin (flutterPhase)) * fs / 1000.0;

            wowPhase += wowPhaseInc;
            if (wowPhase >= kTwoPi) wowPhase -= kTwoPi;
            flutterPhase += flutterPhaseInc;
            if (flutterPhase >= kTwoPi) flutterPhase -= kTwoPi;
        }

        // Write x (finite-guarded) and read at the delay evaluated by the last
        // tick(). Precondition: 0 <= ch < the channel count given to prepare().
        double processSample (int ch, double x) noexcept
        {
            const double in = std::isfinite (x) ? x : 0.0;
            line[ch].write (in);
            return line[ch].readInterpolated (delaySamples);
        }

        // -- in-place block form ---------------------------------------------------
        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            for (int i = 0; i < numSamples; ++i)
            {
                tick();
                for (int ch = 0; ch < nCh; ++ch)
                    audio[ch][i] = (float) processSample (ch, (double) audio[ch][i]);
            }
        }

    private:
        static constexpr double kTwoPi = 6.283185307179586476925286766559;

        void updatePhaseIncrements() noexcept
        {
            wowPhaseInc     = kTwoPi * wowRateHz / fs;
            flutterPhaseInc = kTwoPi * flutterRateHz / fs;
        }

        double fs       = 44100.0;
        int    channels = 2;
        int    center   = 0;

        DelayLine line[kMaxChannels];

        // parameter targets
        double wowDepth01     = 0.0;
        double flutterDepth01 = 0.0;
        double wowRateHz      = kDefaultWowRateHz;
        double flutterRateHz  = kDefaultFlutterRateHz;

        // smoothed / runtime state
        double paramAlpha       = 1.0;
        double wowDepthMsSm     = 0.0;
        double flutterDepthMsSm = 0.0;
        double wowPhase         = 0.0;
        double flutterPhase     = 0.0;
        double wowPhaseInc      = 0.0;
        double flutterPhaseInc  = 0.0;
        double delaySamples     = 0.0;
    };
} // namespace factory_core
