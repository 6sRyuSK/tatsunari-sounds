#pragma once
//
// factory_core/GranularDelay.h — a granular delay engine. Input feeds a delay
// line; short Hann-windowed grains read from it at jittered positions and
// per-grain playback rates (pitch), are panned into a stereo cloud, summed, and
// fed back. Header-only, JUCE-independent, headless-testable.
//
// Real-time safe: the grain pool is fixed and preallocated; randomness comes
// from an inlined xorshift generator. The only allocation is the delay buffer
// in prepare().
//
// Feedback is taken from the pre-pan mono grain sum, so the feedback path is
// independent of stereo panning (the repeats decay by exactly the feedback
// gain when the grains reconstruct the delayed signal).
//
#include "DelayLine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace factory_core
{
    // Convert a musical length in beats to seconds at a given tempo. Pure.
    inline double tempoSyncSeconds (double bpm, double beats) noexcept
    {
        return (bpm > 0.0) ? beats * 60.0 / bpm : 0.0;
    }

    class GranularDelay
    {
    public:
        static constexpr int kMaxGrains = 64;

        void prepare (double sampleRate, double maxDelaySeconds)
        {
            fs = sampleRate;
            maxGrainSamples = (int) (kMaxGrainMs * 1.0e-3 * fs) + 4;
            const int bufLen = (int) (maxDelaySeconds * fs) + maxGrainSamples + 8;
            delay.prepare (bufLen);
            reset();
        }

        void reset() noexcept
        {
            delay.reset();
            for (auto& g : grains) g.active = false;
            schedulePhase = 0.0;
        }

        // --- parameters ---
        void setDelaySamples       (double s) noexcept { delaySamples = s; }
        void setFeedback           (double f) noexcept { feedback = f; }
        void setGrainSizeMs        (double ms) noexcept { grainSizeMs = ms; }
        void setDensityHz          (double d) noexcept { densityHz = std::max (0.1, d); }
        void setPositionJitterMs   (double ms) noexcept { posJitterMs = ms; }
        void setPitchSemitones     (double st) noexcept { pitchSemis = st; }
        void setPitchRandomSemis   (double st) noexcept { pitchRandSemis = st; }
        void setSpread             (double s) noexcept { spread = std::clamp (s, 0.0, 1.0); }
        void setMix                (double m) noexcept { mix = std::clamp (m, 0.0, 1.0); }

        void processStereo (double& l, double& r) noexcept
        {
            const double inMono = 0.5 * (l + r);

            double wetMono = 0.0, wetL = 0.0, wetR = 0.0;
            const double maxDelayRead = (double) delay.getSize() - 2.0;

            for (auto& g : grains)
            {
                if (! g.active) continue;

                const double phase = g.n / g.duration;          // 0..1
                const double win = 0.5 - 0.5 * std::cos (2.0 * kPi * phase);
                double d = g.d0 + g.n * (1.0 - g.rate);
                d = std::clamp (d, 1.0, maxDelayRead);

                const double s = g.gain * win * delay.readInterpolated (d);
                wetMono += s;
                wetL += s * g.gL;
                wetR += s * g.gR;

                g.n += 1.0;
                if (g.n >= g.duration) g.active = false;
            }

            delay.write (inMono + feedback * wetMono);

            // Schedule new grains.
            schedulePhase += 1.0;
            const double interval = fs / densityHz;
            while (schedulePhase >= interval)
            {
                schedulePhase -= interval;
                spawnGrain (interval);
            }

            l = (1.0 - mix) * l + mix * wetL;
            r = (1.0 - mix) * r + mix * wetR;
        }

        // Snapshot of an active grain for the editor's cloud visualizer.
        struct GrainView { float age01; float pan; float pitch; };
        int activeGrains() const noexcept
        {
            int c = 0;
            for (const auto& g : grains) if (g.active) ++c;
            return c;
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;
        static constexpr double kMaxGrainMs = 500.0;

        struct Grain
        {
            bool   active = false;
            double n = 0.0, duration = 1.0;
            double d0 = 0.0, rate = 1.0;
            double gL = 0.0, gR = 0.0, gain = 1.0;
        };

        double nextRand() noexcept // [-1, 1)
        {
            rngState ^= rngState << 13;
            rngState ^= rngState >> 17;
            rngState ^= rngState << 5;
            return (double) rngState / 2147483648.0 - 1.0;
        }

        void spawnGrain (double interval) noexcept
        {
            Grain* slot = nullptr;
            for (auto& g : grains)
                if (! g.active) { slot = &g; break; }
            if (slot == nullptr) return; // pool full: drop this grain

            const double dur = std::max (4.0, grainSizeMs * 1.0e-3 * fs);
            const double jitter = (posJitterMs * 1.0e-3 * fs) * nextRand();
            const double maxD0 = (double) delay.getSize() - dur - 2.0;

            slot->active = true;
            slot->n = 0.0;
            slot->duration = dur;
            slot->d0 = std::clamp (delaySamples + jitter, 1.0, std::max (1.0, maxD0));

            const double semis = pitchSemis + pitchRandSemis * nextRand();
            slot->rate = std::pow (2.0, semis / 12.0);

            const double pan = std::clamp (spread * nextRand(), -1.0, 1.0);
            const double theta = (pan + 1.0) * 0.25 * kPi; // 0..pi/2
            slot->gL = std::cos (theta);
            slot->gR = std::sin (theta);

            // Normalize level for overlap: 50% overlap (interval = dur/2) -> 1.
            slot->gain = std::clamp (2.0 * interval / dur, 0.0, 4.0);
        }

        double fs = 44100.0;
        int maxGrainSamples = 0;

        double delaySamples = 11025.0;
        double feedback = 0.0;
        double grainSizeMs = 120.0;
        double densityHz = 20.0;
        double posJitterMs = 0.0;
        double pitchSemis = 0.0;
        double pitchRandSemis = 0.0;
        double spread = 0.0;
        double mix = 0.5;

        double schedulePhase = 0.0;
        std::uint32_t rngState = 0x9e3779b9u;

        DelayLine delay;
        std::array<Grain, kMaxGrains> grains;
    };
} // namespace factory_core
