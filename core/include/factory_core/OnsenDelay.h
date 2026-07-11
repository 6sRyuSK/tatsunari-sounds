#pragma once
//
// factory_core/OnsenDelay.h — a harmonic glide delay: the delay time steps
// through a cyclic three-step sequence (base, interval 1, interval 2) whose
// times are musical ratios of the base time (2^(-semitones/12)), and every
// time change glides through a one-pole lag, producing the pitch-bending
// repeats of a clocked BBD. Composes DelayLine + OnePole + Waveshaper.
// Header-only, JUCE-independent; prepare() allocates, process() does not.
//
// Loop-gain contract (regression policy): the feedback shaper is
// (1/kSatPregain)*tanh(kSatPregain*x) — unity slope at 0, compressive above,
// output bounded by 1/kSatPregain — and regen is clamped to kMaxRegen < 1, so
// the per-pass loop gain is < 1 at every in-range setting.
//
// Glide spec (the tests' Doppler oracle depends on these exact formulas):
//   tauMs        = kMinGlideTauMs + glide^2 * kMaxGlideTauMs
//   alpha        = onePoleAlphaForTauSamples(max(1, tauMs*1e-3*fs))
//   D[n+1]       = D[n] + alpha * (Dtarget - D[n])          (per sample)
//   pitch ratio  = 1 - dD/dt  (D and t in samples)
//
// Step clock: step k lasts round(baseMs * ratio(k) * fs / 1000) samples in
// Auto mode (halving the time doubles the repeats, like tapping Thermae);
// Manual mode holds the current step until triggerStep().
//
#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "OnePole.h"
#include "SmoothingCoeff.h"
#include "Waveshaper.h"

namespace factory_core
{
    class OnsenDelay
    {
    public:
        enum class Interval
        {
            OctaveDown = 0, FifthDown, FourthDown, Unison, FourthUp, FifthUp, OctaveUp
        };

        static constexpr int kNumIntervals = 7;

        static double semitonesFor (Interval iv) noexcept
        {
            switch (iv)
            {
                case Interval::OctaveDown: return -12.0;
                case Interval::FifthDown:  return  -7.0;
                case Interval::FourthDown: return  -5.0;
                case Interval::Unison:     return   0.0;
                case Interval::FourthUp:   return   5.0;
                case Interval::FifthUp:    return   7.0;
                case Interval::OctaveUp:   return  12.0;
            }
            return 0.0;
        }

        // Delay-time ratio: pitch up (positive semitones) = shorter time.
        static double timeRatioFor (Interval iv) noexcept
        {
            return std::exp2 (-semitonesFor (iv) / 12.0);
        }

        static constexpr int    kMaxChannels    = 2;
        static constexpr double kMinTimeMs      = 20.0;
        static constexpr double kMaxTimeMs      = 2000.0;
        static constexpr double kMaxRegen       = 0.95;
        static constexpr double kMinGlideTauMs  = 5.0;    // declick floor
        static constexpr double kMaxGlideTauMs  = 1500.0; // glide=1 -> bendy soup
        static constexpr double kSatPregain     = 1.5;
        static constexpr double kMinToneHz      = 500.0;
        static constexpr double kMaxToneHz      = 18000.0;
        static constexpr double kParamSmoothMs  = 20.0;

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            // Worst case: max base time at the slowest interval (octave down = x2).
            const int maxDelaySamples =
                (int) std::ceil (kMaxTimeMs * 0.001 * fs * timeRatioFor (Interval::OctaveDown)) + 8;

            for (int ch = 0; ch < channels; ++ch)
            {
                delay[ch].prepare (maxDelaySamples);
                tone[ch].reset();
            }

            shaper.setDrive (kSatPregain);
            shaper.setMix (1.0);
            shaper.setOutput (1.0 / kSatPregain);

            paramAlpha = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);
            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                delay[ch].reset();
                tone[ch].reset();
            }
            step        = 0;
            stepElapsed = 0;
            pendingStep = false;

            delaySamples = targetDelaySamples();
            regenSm      = regen;
            mixSm        = mix;
            toneHzSm     = toneHz;
            updateGlideAlpha();
            updateTone();
        }

        // -- parameters (call on the audio thread between process calls) --------
        void setBaseTimeMs (double ms) noexcept
        {
            baseMs = std::clamp (ms, kMinTimeMs, kMaxTimeMs);
        }

        void setIntervals (Interval i1, Interval i2) noexcept { int1 = i1; int2 = i2; }

        void setGlide (double zeroToOne) noexcept
        {
            glide = std::clamp (zeroToOne, 0.0, 1.0);
            updateGlideAlpha();
        }

        void setRegen (double zeroToOne) noexcept
        {
            regen = std::clamp (zeroToOne, 0.0, 1.0) * kMaxRegen;
        }

        void setToneHz (double hz) noexcept
        {
            toneHz = std::clamp (hz, kMinToneHz, kMaxToneHz);
        }

        void setMix (double zeroToOne) noexcept { mix = std::clamp (zeroToOne, 0.0, 1.0); }

        void setManualStep (bool manual) noexcept { manualStep = manual; }

        // In Manual mode: advance the sequence at the next sample.
        void triggerStep() noexcept { pendingStep = true; }

        int  currentStep() const noexcept { return step; }
        double currentDelaySamples() const noexcept { return delaySamples; }

        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            for (int i = 0; i < numSamples; ++i)
            {
                advanceSequencer();

                // Smoothed controls (continuous-parameter regression rule).
                regenSm  += paramAlpha * (regen  - regenSm);
                mixSm    += paramAlpha * (mix    - mixSm);
                toneHzSm += paramAlpha * (toneHz - toneHzSm);
                updateTone();

                // Glide: one-pole lag toward the current step's delay time.
                delaySamples += glideAlpha * (targetDelaySamples() - delaySamples);

                for (int ch = 0; ch < nCh; ++ch)
                {
                    // Finite guard, input side: a NaN/Inf sample must not enter
                    // the buffer or pass through the dry path.
                    const double raw = (double) audio[ch][i];
                    const double in  = std::isfinite (raw) ? raw : 0.0;

                    double wet = delay[ch].readInterpolated (delaySamples);

                    // Finite guard, feedback side: self-recover from corrupt state.
                    if (! std::isfinite (wet))
                    {
                        delay[ch].reset();
                        tone[ch].reset();
                        wet = 0.0;
                    }

                    const double fb = regenSm * shaper.processSample (tone[ch].lp (wet));
                    delay[ch].write (in + fb);

                    audio[ch][i] = (float) ((1.0 - mixSm) * in + mixSm * wet);
                }
            }
        }

    private:
        double stepRatio (int s) const noexcept
        {
            return s == 0 ? 1.0 : timeRatioFor (s == 1 ? int1 : int2);
        }

        double targetDelaySamples() const noexcept
        {
            return baseMs * 0.001 * fs * stepRatio (step);
        }

        long long stepDurationSamples() const noexcept
        {
            return (long long) std::llround (baseMs * 0.001 * fs * stepRatio (step));
        }

        void advanceSequencer() noexcept
        {
            if (manualStep)
            {
                if (pendingStep)
                {
                    pendingStep = false;
                    step        = (step + 1) % 3;
                    stepElapsed = 0;
                }
                return;
            }

            pendingStep = false;
            if (++stepElapsed >= stepDurationSamples())
            {
                step        = (step + 1) % 3;
                stepElapsed = 0;
            }
        }

        void updateGlideAlpha() noexcept
        {
            const double tauMs = kMinGlideTauMs + glide * glide * kMaxGlideTauMs;
            glideAlpha = onePoleAlphaForTauSamples (std::max (1.0, tauMs * 0.001 * fs));
        }

        void updateTone() noexcept
        {
            for (int ch = 0; ch < channels; ++ch)
                tone[ch].setCutoff (toneHzSm, fs);
        }

        double fs       = 44100.0;
        int    channels = 2;

        DelayLine  delay[kMaxChannels];
        OnePole    tone[kMaxChannels];
        Waveshaper shaper;

        // parameter targets
        double   baseMs = 350.0;
        Interval int1   = Interval::Unison;
        Interval int2   = Interval::Unison;
        double   glide  = 0.25;
        double   regen  = 0.4 * kMaxRegen;
        double   toneHz = 6000.0;
        double   mix    = 0.35;
        bool     manualStep = false;

        // smoothed / runtime state
        double    delaySamples = 0.0;
        double    glideAlpha   = 1.0;
        double    paramAlpha   = 1.0;
        double    regenSm = 0.0, mixSm = 0.0, toneHzSm = 6000.0;
        int       step        = 0;
        long long stepElapsed = 0;
        bool      pendingStep = false;
    };
} // namespace factory_core
