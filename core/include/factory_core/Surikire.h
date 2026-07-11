#pragma once
//
// factory_core/Surikire.h -- a lo-fi media-degradation engine (Generation Loss
// style): wow/flutter pitch wobble, generation-loss band shrinking, tape
// saturation, hiss, and dropouts, all DETERMINISTIC (fixed seeds; exactly
// reproducible after reset()). No feedback path -- the whole chain is
// feedforward. Composes WowFlutter + OnePole + Waveshaper. Header-only,
// JUCE-independent; prepare() allocates, process() does not (no locks, no
// syscalls, noexcept).
//
// Signal chain (wet path, per channel; the dry path is undelayed):
//   in -> [finite guard] -> WowFlutter -> HP(user) -> HP(gen) -> LP(gen)
//      -> LP(user) -> saturator -> (+ hiss) -> dropout gain -> wet
//   out = (1 - mix) * in + mix * wet
// The wet path is WowFlutter::kCenterDelayMs late, so a partial mix combs
// against the dry signal -- the intended "tape dub vs source" texture. The
// engine reports no latency.
//
// Filters: four OnePole sections per channel, in the chain order above.
// Steady-state cutoffs (tests verify in the z domain against the product of
// one-pole responses, H_lp = (1-a)/(1 - a e^{-jw}), H_hp = 1 - H_lp,
// a = exp(-2*pi*fc/fs)):
//   fHpGen(g) = 20 * 20^g        Hz   (g = 0 -> 20 Hz,     g = 1 -> 400 Hz)
//   fLpGen(g) = 20000 * 20^(-g)  Hz   (g = 0 -> 20000 Hz,  g = 1 -> 1000 Hz)
//   user HP clamped to [20, 2000] Hz, user LP clamped to [1000, 20000] Hz.
// The smoothed quantities are generation01 and the two user cutoffs in Hz;
// all four coefficients are recomputed every sample from the smoothed values,
// so the steady state matches the closed forms exactly and sweeps do not
// zipper. (All cutoffs stay below OnePole's 0.49*fs internal ceiling at the
// standard 44.1-192 kHz rates.)
//
// Saturator: Waveshaper with drive d = 1 + 7*s, mix = 1, output = 1/d, i.e.
//   y = (1/d) * tanh(d * x)
// Unity small-signal gain (slope 1 at x = 0) for every s; THD rises
// monotonically with s. Note the saturator is in-circuit at s = 0 too
// (y = tanh(x)).
//
// Hiss: per-channel white noise from the deterministic LCG below, shaped by a
// OnePole lowpass at kNoiseShapeHz, scaled by
//   amp = noise01Sm^2 * kNoiseMaxAmp
// (the smoothed quantity is noise01; the square is taken after smoothing) and
// added to the wet path BEFORE the dropout gain. Each channel's noise LCG
// advances exactly once per sample processed on that channel, regardless of
// the noise amount, so the stream position equals the samples processed since
// reset().
//
// Deterministic LCG (tests re-implement this spec independently):
//   state = state * 6364136223846793005 + 1442695040888963407   (uint64, mod 2^64)
//   u01   = (state >> 11) * (1.0 / 9007199254740992.0)   // top 53 bits -> [0,1)
//   white = 2*u01 - 1
//   seeds: kNoiseSeed[2] = { 0xA1B2C3D4E5F60101, 0xA1B2C3D4E5F60202 }   (L, R)
//          kDropoutSeed  = 0x5150D120D0FF5EED
// reset() reseeds every LCG state, so the post-reset output sequence is
// exactly identical.
//
// Dropout scheduler (channel-shared, deterministic): the event timeline is
// FIXED -- it does not depend on the failure setting. failure only scales the
// dip depth; at failure = 0 the timeline still advances and the gain is
// exactly 1. For event i the kDropoutSeed LCG draws, IN THIS ORDER:
// u_gap, u_depth, u_width, with
//   gapSec   = 0.25 + 1.75 * u_gap    (previous event end -> next event start;
//                                      the first gap is measured from t = 0)
//   depth    = failure01Sm(latched at the event start sample)
//              * (0.5 + 0.5 * u_depth) * kDropMaxDepth
//   widthSec = 0.03 + 0.05 * u_width
// Gap and width are rounded to whole samples with llround. During an event of
// W samples, the gain at its n-th sample (n = 0..W-1) is the raised-cosine dip
//   g(tau) = 1 - depth * 0.5 * (1 - cos(2*pi*tau)),   tau = n / W
// so the gain is exactly 1 again on the first sample after the event. The
// parameter smoothers advance at the top of every sample, before the
// scheduler ticks, so an event starting on sample n latches failure01Sm as of
// sample n. Event i's triple is drawn when event i-1 ends (event 0's at
// reset()), which on the dedicated dropout LCG is indistinguishable from
// drawing all triples up front.
//
// Safety / state rules:
//   - Input finite guard: a non-finite input sample is treated as 0 on both
//     the wet and dry paths.
//   - Wet-node finite guard: if the wet sample is non-finite after the chain,
//     the whole wet signal path is reset (WowFlutter, the four tone filters
//     and the noise-shaping filters of ALL channels) and the sample is output
//     with wet = 0 -- self-recovery from a single NaN/Inf. The LCGs and the
//     dropout timeline are audio-independent and are NOT reset by recovery.
//   - reset() reinitializes everything deterministically: WowFlutter (lines +
//     phases), filter states, all LCG seeds, the dropout scheduler (event 0 is
//     re-drawn), and every parameter smoother (snapped to its target).
//   - prepare() performs all allocation (inside WowFlutter::prepare); the
//     process path allocates nothing.
//   - Continuous-parameter rule: every setter clamps its argument and the
//     value is smoothed by a 20 ms one-pole (kParamSmoothMs).
//   - Peak bound (long-hold invariant): the saturator output magnitude is
//     bounded by 1/d <= 1 for ANY input, hiss adds at most kNoiseMaxAmp, and
//     the dropout gain lies in (0, 1]; hence |wet| <= 1 + kNoiseMaxAmp = 1.02,
//     and since the output is a convex blend, |out| <= max(|in|, |wet|)
//     <= 1.02 < 1.25 for |in| <= 1. Typical program material sits near
//     tanh(1) + hiss, i.e. wet <= ~0.9.
//
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "OnePole.h"
#include "SmoothingCoeff.h"
#include "Waveshaper.h"
#include "WowFlutter.h"

namespace factory_core
{
    class Surikire
    {
    public:
        static constexpr int    kMaxChannels     = 2;
        static constexpr double kParamSmoothMs   = 20.0;

        // Saturator: drive = 1 + kSatDriveRange * saturate01.
        static constexpr double kSatDriveRange   = 7.0;

        // Hiss: amp = noise01^2 * kNoiseMaxAmp, shaped by a lowpass at kNoiseShapeHz.
        static constexpr double kNoiseMaxAmp     = 0.02;
        static constexpr double kNoiseShapeHz    = 8000.0;

        // User tone-filter ranges (silently exceeding values are clamped by the setters).
        static constexpr double kMinUserHpHz     = 20.0;
        static constexpr double kMaxUserHpHz     = 2000.0;
        static constexpr double kMinUserLpHz     = 1000.0;
        static constexpr double kMaxUserLpHz     = 20000.0;

        // Dropout scheduler: gapSec = base + span * u_gap, widthSec likewise,
        // depth = failure * (kDropDepthBase + kDropDepthSpan * u_depth) * kDropMaxDepth.
        static constexpr double kDropGapBaseSec   = 0.25;
        static constexpr double kDropGapSpanSec   = 1.75;
        static constexpr double kDropWidthBaseSec = 0.03;
        static constexpr double kDropWidthSpanSec = 0.05;
        static constexpr double kDropDepthBase    = 0.5;
        static constexpr double kDropDepthSpan    = 0.5;
        static constexpr double kDropMaxDepth     = 0.9;

        // Deterministic seeds (see the LCG spec in the header comment).
        static constexpr std::uint64_t kNoiseSeed[2] = { 0xA1B2C3D4E5F60101ULL,
                                                         0xA1B2C3D4E5F60202ULL };
        static constexpr std::uint64_t kDropoutSeed  = 0x5150D120D0FF5EEDULL;

        // Generation-loss cutoff closed forms: fHpGen(g) = 20 * 20^g,
        // fLpGen(g) = 20000 * 20^(-g). Published so the mapping is part of the API.
        static double genHpCutoffHz (double g) noexcept { return 20.0 * std::pow (20.0, g); }
        static double genLpCutoffHz (double g) noexcept { return 20000.0 * std::pow (20.0, -g); }

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            wowFlutter.prepare (fs, channels);

            shaper.setMix (1.0); // drive / output follow the smoothed saturate01 per sample

            for (int ch = 0; ch < kMaxChannels; ++ch)
                noiseLp[ch].setCutoff (kNoiseShapeHz, fs);

            paramAlpha = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);
            reset();
        }

        void reset() noexcept
        {
            resetWetPath();

            // Snap every smoother to its target.
            genSm     = gen;
            userHpSm  = userHpHz;
            userLpSm  = userLpHz;
            satSm     = saturate;
            noiseSm   = noise;
            failureSm = failure;
            mixSm     = mix;
            applyFilterCutoffs();

            // Reseed the deterministic generators and restart the dropout timeline.
            noiseLcg[0].seed (kNoiseSeed[0]);
            noiseLcg[1].seed (kNoiseSeed[1]);
            dropLcg.seed (kDropoutSeed);
            inDropout = false;
            dropPos   = 0;
            dropDepth = 0.0;
            scheduleNextDropout(); // draws event 0's (u_gap, u_depth, u_width)
        }

        // -- parameters (call on the audio thread between process calls) --------
        void setWow01        (double v) noexcept { wowFlutter.setWowDepth01 (v); }
        void setFlutter01    (double v) noexcept { wowFlutter.setFlutterDepth01 (v); }
        void setGeneration01 (double v) noexcept { gen      = std::clamp (v, 0.0, 1.0); }
        void setUserHpHz     (double hz) noexcept { userHpHz = std::clamp (hz, kMinUserHpHz, kMaxUserHpHz); }
        void setUserLpHz     (double hz) noexcept { userLpHz = std::clamp (hz, kMinUserLpHz, kMaxUserLpHz); }
        void setSaturate01   (double v) noexcept { saturate = std::clamp (v, 0.0, 1.0); }
        void setNoise01      (double v) noexcept { noise    = std::clamp (v, 0.0, 1.0); }
        void setFailure01    (double v) noexcept { failure  = std::clamp (v, 0.0, 1.0); }
        void setMix01        (double v) noexcept { mix      = std::clamp (v, 0.0, 1.0); }

        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            for (int i = 0; i < numSamples; ++i)
            {
                // 1) 20 ms parameter smoothers (continuous-parameter rule).
                genSm     += paramAlpha * (gen      - genSm);
                userHpSm  += paramAlpha * (userHpHz - userHpSm);
                userLpSm  += paramAlpha * (userLpHz - userLpSm);
                satSm     += paramAlpha * (saturate - satSm);
                noiseSm   += paramAlpha * (noise    - noiseSm);
                failureSm += paramAlpha * (failure  - failureSm);
                mixSm     += paramAlpha * (mix      - mixSm);

                // 2) Coefficients from the smoothed values (steady state == closed forms).
                applyFilterCutoffs();
                const double drive = 1.0 + kSatDriveRange * satSm;
                shaper.setDrive (drive);
                shaper.setOutput (1.0 / drive);
                const double noiseAmp = noiseSm * noiseSm * kNoiseMaxAmp;

                // 3) Shared per-sample sequencers (both channels see the same
                //    LFO phases and the same dropout gain).
                wowFlutter.tick();
                const double dropGain = tickDropout();

                for (int ch = 0; ch < nCh; ++ch)
                {
                    // Input finite guard: NaN/Inf must reach neither path.
                    const double raw = (double) audio[ch][i];
                    const double in  = std::isfinite (raw) ? raw : 0.0;

                    double wet = wowFlutter.processSample (ch, in);
                    wet = hpUser[ch].hp (wet);
                    wet = hpGen [ch].hp (wet);
                    wet = lpGen [ch].lp (wet);
                    wet = lpUser[ch].lp (wet);
                    wet = shaper.processSample (wet);
                    wet += noiseAmp * noiseLp[ch].lp (noiseLcg[ch].nextWhite());
                    wet *= dropGain;

                    // Wet-node finite guard: self-recover from corrupt state.
                    if (! std::isfinite (wet))
                    {
                        resetWetPath();
                        wet = 0.0;
                    }

                    audio[ch][i] = (float) ((1.0 - mixSm) * in + mixSm * wet);
                }
            }
        }

    private:
        static constexpr double kTwoPi = 6.283185307179586476925286766559;

        // The deterministic generator specified in the header comment.
        struct Lcg
        {
            std::uint64_t state = 0;

            void seed (std::uint64_t s) noexcept { state = s; }

            double next01() noexcept
            {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                return (double) (state >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
            }

            double nextWhite() noexcept { return 2.0 * next01() - 1.0; } // [-1, 1)
        };

        // Reset the wet signal path only (used by reset() and by NaN recovery).
        // LCGs, the dropout timeline and the smoothers are NOT touched here.
        void resetWetPath() noexcept
        {
            wowFlutter.reset();
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                hpUser[ch].reset();
                hpGen [ch].reset();
                lpGen [ch].reset();
                lpUser[ch].reset();
                noiseLp[ch].reset();
            }
        }

        void applyFilterCutoffs() noexcept
        {
            const double fHpGen = genHpCutoffHz (genSm);
            const double fLpGen = genLpCutoffHz (genSm);
            for (int ch = 0; ch < channels; ++ch)
            {
                hpUser[ch].setCutoff (userHpSm, fs);
                hpGen [ch].setCutoff (fHpGen,   fs);
                lpGen [ch].setCutoff (fLpGen,   fs);
                lpUser[ch].setCutoff (userLpSm, fs);
            }
        }

        // Draw the next event's (u_gap, u_depth, u_width) -- exactly three LCG
        // draws per event, in this order -- and arm its gap and width.
        void scheduleNextDropout() noexcept
        {
            const double uGap   = dropLcg.next01();
            const double uDepth = dropLcg.next01();
            const double uWidth = dropLcg.next01();

            gapRemaining     = std::llround ((kDropGapBaseSec + kDropGapSpanSec * uGap) * fs);
            dropDepthFactor  = (kDropDepthBase + kDropDepthSpan * uDepth) * kDropMaxDepth;
            dropWidthSamples = std::llround ((kDropWidthBaseSec + kDropWidthSpanSec * uWidth) * fs);
            inDropout        = false;
            dropPos          = 0;
        }

        // Advance the channel-shared dropout timeline by one sample and return
        // this sample's gain. failure01Sm is latched on the event's first sample.
        double tickDropout() noexcept
        {
            if (! inDropout)
            {
                if (gapRemaining > 0)
                {
                    --gapRemaining;
                    return 1.0;
                }
                inDropout = true;
                dropPos   = 0;
                dropDepth = failureSm * dropDepthFactor;
            }

            const double tau  = (double) dropPos / (double) dropWidthSamples;
            const double gain = 1.0 - dropDepth * 0.5 * (1.0 - std::cos (kTwoPi * tau));

            if (++dropPos >= dropWidthSamples)
                scheduleNextDropout();

            return gain;
        }

        double fs       = 44100.0;
        int    channels = 2;

        WowFlutter wowFlutter;
        OnePole    hpUser[kMaxChannels];
        OnePole    hpGen[kMaxChannels];
        OnePole    lpGen[kMaxChannels];
        OnePole    lpUser[kMaxChannels];
        OnePole    noiseLp[kMaxChannels];
        Waveshaper shaper;
        Lcg        noiseLcg[kMaxChannels];
        Lcg        dropLcg;

        // parameter targets (wow / flutter live inside WowFlutter)
        double gen      = 0.35;
        double userHpHz = kMinUserHpHz;
        double userLpHz = kMaxUserLpHz;
        double saturate = 0.2;
        double noise    = 0.1;
        double failure  = 0.0;
        double mix      = 1.0;

        // smoothed / runtime state
        double paramAlpha = 1.0;
        double genSm = 0.35, userHpSm = kMinUserHpHz, userLpSm = kMaxUserLpHz;
        double satSm = 0.2, noiseSm = 0.1, failureSm = 0.0, mixSm = 1.0;

        // dropout scheduler state
        long long gapRemaining     = 0;
        long long dropWidthSamples = 1;
        long long dropPos          = 0;
        double    dropDepthFactor  = 0.0;
        double    dropDepth        = 0.0;
        bool      inDropout        = false;
    };
} // namespace factory_core
