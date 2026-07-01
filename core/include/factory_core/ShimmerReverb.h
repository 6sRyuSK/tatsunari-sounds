#pragma once
//
// factory_core/ShimmerReverb.h — a shimmer reverb: an 8-line feedback delay
// network (FDN) with per-line damping and a normalized Hadamard feedback
// matrix, plus two pitch shifters in the feedback path that lift the tail by an
// interval (e.g. +12 and +7), tone-shaped by low/high cut. Pre-delay, tail LFO
// modulation, freeze, and dry/wet. Header-only, JUCE-independent, headless.
//
// Real-time safe: all buffers are allocated in prepare(); processStereo is
// allocation/lock/syscall free.
//
#include "DelayLine.h"
#include "OnePole.h"
#include "PitchShifter.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace factory_core
{
    class ShimmerReverb
    {
    public:
        static constexpr int kLines = 8;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            const double scale = fs / 44100.0;
            for (int i = 0; i < kLines; ++i)
            {
                baseLen[i] = kBaseLen[i] * scale;
                const int maxLen = (int) (baseLen[i] * kMaxSize) + kModMax + 8;
                lines[i].prepare (maxLen);
                damp[i].reset();
            }
            preDelay.prepare ((int) (kMaxPreDelayMs * 1.0e-3 * fs) + 8);
            pitchA.prepare (fs);
            pitchB.prepare (fs);
            lowCut.reset();
            highCut.reset();
            reset();
            updateDamping();
            updateCuts();
        }

        void reset() noexcept
        {
            for (auto& l : lines) l.reset();
            for (auto& d : damp) d.reset();
            preDelay.reset();
            pitchA.reset();
            pitchB.reset();
            lowCut.reset();
            highCut.reset();
            lfoPhase = 0.0;
            shimmerFb = 0.0;
        }

        // --- parameters ---
        void setSize       (double s) noexcept { size = std::clamp (s, 0.3, kMaxSize); }
        void setDecaySec   (double s) noexcept { decaySec = std::max (0.1, s); }
        void setDamping    (double d) noexcept { damping = std::clamp (d, 0.0, 1.0); updateDamping(); }
        void setShimmer    (double s) noexcept { shimmer = std::clamp (s, 0.0, 0.95); }
        void setPitchASemis (double st) noexcept { pitchA.setRatio (PitchShifter::semitonesToRatio (st)); }
        void setPitchBSemis (double st) noexcept { pitchB.setRatio (PitchShifter::semitonesToRatio (st)); }
        void setVoiceBMix  (double m) noexcept { voiceBMix = std::clamp (m, 0.0, 1.0); }
        void setPreDelayMs (double ms) noexcept { preDelaySamples = std::clamp (ms, 0.0, kMaxPreDelayMs) * 1.0e-3 * fs; }
        void setLowCutHz   (double hz) noexcept { lowCutHz = hz; updateCuts(); }
        void setHighCutHz  (double hz) noexcept { highCutHz = hz; updateCuts(); }
        void setFreeze     (bool f) noexcept { freeze = f; }
        void setModRateHz  (double hz) noexcept { modRate = hz; }
        void setModDepth   (double d) noexcept { modDepth = std::clamp (d, 0.0, 1.0); }
        void setMix        (double m) noexcept { mix = std::clamp (m, 0.0, 1.0); }

        void processStereo (double& l, double& r) noexcept
        {
            const double dryL = l, dryR = r;
            const double inMono = 0.5 * (l + r);

            preDelay.write (inMono);
            const double pre = preDelay.readInterpolated (preDelaySamples);
            // Freeze must cut the shimmer feedback too: the FDN is lossless while
            // frozen (write-back gain 1.0, damping bypassed), so injecting the
            // pitch-shifted tail every sample drives the loss-free loop to
            // infinity. Gate the whole external input, not just `pre`.
            const double rin = freeze ? 0.0 : (pre + shimmerFb);

            // Read tails (with LFO modulation), then damp. Guard against a
            // non-finite state (denormal/overflow): flush the line so one Inf/NaN
            // cannot poison the feedback loop permanently.
            std::array<double, kLines> s {};
            for (int i = 0; i < kLines; ++i)
            {
                const double len = baseLen[i] * size + modDepth * kModMax * std::sin (2.0 * kPi * lfoPhase + i * 0.7);
                double sv = lines[(size_t) i].readInterpolated (std::max (1.0, len));
                if (! std::isfinite (sv)) { sv = 0.0; lines[(size_t) i].reset(); }
                s[(size_t) i] = sv;
            }

            std::array<double, kLines> h {};
            for (int i = 0; i < kLines; ++i)
                h[(size_t) i] = freeze ? s[(size_t) i] : damp[(size_t) i].lp (s[(size_t) i]);

            hadamard8 (h.data());

            for (int i = 0; i < kLines; ++i)
            {
                const double gain = freeze ? 1.0 : decayGain (i);
                lines[(size_t) i].write (gain * h[(size_t) i] + rin * 0.5);
            }

            lfoPhase += modRate / fs;
            if (lfoPhase >= 1.0) lfoPhase -= 1.0;

            const double wetL = (s[0] + s[2] + s[4] + s[6]) * 0.5;
            const double wetR = (s[1] + s[3] + s[5] + s[7]) * 0.5;
            const double wetMono = 0.5 * (wetL + wetR);

            // Shimmer: pitch-shift the tail, tone-shape, scale, feed back.
            const double pA = pitchA.process (wetMono);
            const double pB = pitchB.process (wetMono);
            double voice = (1.0 - voiceBMix) * pA + voiceBMix * pB;
            voice = highCut.lp (lowCut.hp (voice));
            shimmerFb = shimmer * voice;
            if (! std::isfinite (shimmerFb)) shimmerFb = 0.0;

            l = (1.0 - mix) * dryL + mix * wetL;
            r = (1.0 - mix) * dryR + mix * wetR;
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;
        static constexpr double kMaxSize = 1.6;
        static constexpr int    kModMax = 24; // max LFO depth in samples
        static constexpr double kMaxPreDelayMs = 250.0;

        // Freeverb-style mutually-prime base comb lengths (samples @ 44.1k).
        static constexpr double kBaseLen[kLines] =
            { 1116.0, 1356.0, 1422.0, 1617.0, 1188.0, 1277.0, 1491.0, 1557.0 };

        static void hadamard8 (double* x) noexcept
        {
            for (int len = 1; len < kLines; len <<= 1)
                for (int i = 0; i < kLines; i += (len << 1))
                    for (int j = i; j < i + len; ++j)
                    {
                        const double a = x[j], b = x[j + len];
                        x[j] = a + b;
                        x[j + len] = a - b;
                    }
            for (int i = 0; i < kLines; ++i)
                x[i] *= 0.35355339059327373; // 1/sqrt(8)
        }

        double decayGain (int i) const noexcept
        {
            const double len = baseLen[i] * size;
            return std::pow (10.0, -3.0 * len / (decaySec * fs));
        }

        void updateDamping() noexcept
        {
            const double cutoff = 18000.0 * std::pow (1500.0 / 18000.0, damping); // 18k..1.5k
            for (auto& d : damp) d.setCutoff (cutoff, fs);
        }

        void updateCuts() noexcept
        {
            lowCut.setCutoff (lowCutHz, fs);
            highCut.setCutoff (highCutHz, fs);
        }

        double fs = 44100.0;
        std::array<double, kLines> baseLen {};
        std::array<DelayLine, kLines> lines;
        std::array<OnePole, kLines> damp;
        DelayLine preDelay;
        PitchShifter pitchA, pitchB;
        OnePole lowCut, highCut;

        double size = 1.0;
        double decaySec = 2.5;
        double damping = 0.3;
        double shimmer = 0.0;
        double voiceBMix = 0.0;
        double preDelaySamples = 0.0;
        double lowCutHz = 120.0;
        double highCutHz = 9000.0;
        bool   freeze = false;
        double modRate = 0.3;
        double modDepth = 0.0;
        double mix = 0.3;

        double lfoPhase = 0.0;
        double shimmerFb = 0.0;
    };
} // namespace factory_core
