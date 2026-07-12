#pragma once
//
// factory_core/Madoromi.h -- a MOOD-MKII-style micro-loop + ambient-wash
// engine. Two worlds share one variable master CLOCK: the "live" world runs
// the input through an FDN reverb wash (ShimmerReverb with its pitch
// component silenced), the "loop" world (MicroLooper) continuously records
// the wash output and can freeze the most recent LENGTH window into a
// bit-exact loop. The CLOCK knob is the INTERNAL SAMPLE RATE: the internal
// section is prepared once at a fixed kInternalRateHz = 48000 and a
// variable-ratio bracket (VariPolyphaseResampler, host <-> internal) "turns
// its clock pin" -- every internal time constant, the wash tone and the
// frozen loop pitch/period scale by C / 48000 in host terms, and lowering
// CLOCK band-limits the whole wet world to C / 2 (MOOD semantics).
//
// Signal chain (per host sample; stereo lanes, mono input is duplicated):
//   inHost -> [finite guard] -> down-resample (ratio fsHost / C)
//       washOut = ShimmerReverb(live), internal mix = kWashMixMax * wash01
//                 (wash01 = 0 -> washOut == internal input, transparent)
//       loopOut = MicroLooper(washOut recorded continuously)
//       wetInt  = (1 - balance) * washOut + balance * loopOut
//   -> up-resample (ratio C / fsHost) -> output FIFO -> wetHost
//   out = (1 - mix) * dryDelayed + mix * wetHost
// The dry path is delayed INSIDE the engine by exactly latencySamples()
// integer samples, so the reported latency is correct at every mix setting
// (mix = 0 emits a bit-exact L-sample-delayed copy of the input).
//
// CLOCK / repitch contract:
//   setClockHz clamps to [kClockMinHz, kClockMaxHz] = [8000, 48000] and is
//   smoothed with a one-pole of tau = kClockTauMs = 100 ms (host time), so
//   repitching glides like tape. A frozen loop of P internal samples plays
//   in P / C host seconds and a tone frozen at clock C1 replays at
//   f * C2 / C1 when the clock moves to C2 (both period and pitch scale by
//   C2 / C1). Anti-aliasing: the down/up conversions are band-limited by
//   VariPolyphaseResampler (Kaiser ~80 dB design; see that header), so at
//   clock C input energy above C / 2 is suppressed by the stopband.
//
// Latency contract (published formula; D = VariPolyphaseResampler::kHalfTaps
// = 31, all quantities in host samples, fsHost = prepared host rate):
//   P0 = kFifoMargin + ceil(2 * D * (fsHost / kClockMinHz
//                                    - max(1, fsHost / kInternalRateHz)))
//   L  = lround(2 * D * max(1, fsHost / kInternalRateHz)) + P0
//      ( == kFifoMargin + 2 * D * fsHost / kClockMinHz up to rounding )
// (latencyForRate() / fifoPrefillForRate() below are the pure functions.)
// The engine reports L, delays the dry path by exactly L integer samples,
// and prefills the wet output FIFO with L + kFifoSafetyPad zeros. Because
// the FIFO is pulled at exactly the input rate, the total wet transit is
// CONSTANT at every clock: transit = prefill, while the FIFO occupancy
// breathes as prefill minus the bracket's in-flight group delay
// 2 * D * max(1, fsHost / C). At the clock floor C = kClockMinHz the
// occupancy reaches its minimum,
//   prefill - (L - kFifoMargin) = kFifoMargin + kFifoSafetyPad,
// which safely covers the fractional-phase oscillation of the two streaming
// stages (bounded by ~2 * (fsHost / kClockMinHz + 1) host samples) -- the
// FIFO can never underrun at any in-range clock. Net timing: dry and the
// reported latency are exactly L; the wet world plays kFifoSafetyPad
// samples behind the dry world at every clock (a fixed, sub-1.5 ms offset
// inside an ambient wash; no gate depends on it). P0 is the published
// occupancy component of L at the C = kClockMaxHz reference.
//
// Wash / tone mappings (published, monotone; ShimmerReverb in-range only,
// so its loop-gain < 1 contract is inherited -- setShimmer(0) silences the
// pitch-shifter feedback entirely and setFreeze(false) keeps the FDN lossy):
//   decaySec(v) = kWashDecayBaseSec * pow(kWashDecayRatio, v)
//               = 0.4 * 20^v seconds            (v = wash01 in [0, 1])
//   reverbMix(v) = kWashMixMax * v = 0.9 * v    (0 -> bit-transparent live)
//   damping(T)  = clamp(ln(18000 / T) / ln(12), 0, 1)   (T = toneHz)
//     which sets ShimmerReverb's per-line damping cutoff to exactly T Hz
//     (its own map is cutoff = 18000 * (1500/18000)^d); tone is clamped to
//     [kToneMinHz, kToneMaxHz] = [1500, 18000] Hz, the exact invertible
//     range. Internal Hz map to host Hz times C / 48000 (clock semantics).
//
// Real-time / safety rules (house standard):
//   - prepare() performs ALL allocation, worst-case sized: looper capacity
//     (kMaxLoopSec * 48000), resampler histories for ratio fsHost/8000,
//     fixed internal chunking (kHostChunk host samples per slice -> at most
//     ceil(kHostChunk * 48000 / fsHost) + 16 internal samples), up-scratch
//     for ratio fsHost/8000, FIFO for prefill + one worst-case slice, and an
//     L-sample dry line. process() allocates nothing, takes no locks, makes
//     no syscalls, and is noexcept regardless of the caller's block size
//     (any n is processed as fixed-size slices).
//   - Input finite guard on every sample (NaN/Inf -> 0 on both paths); wet
//     node finite guard: a non-finite wet sample resets the wash, the
//     looper and the resampler/FIFO states (wet = 0 for that sample, dry
//     path untouched) -- self-recovery. Every non-bool setter ignores
//     non-finite values: if (!std::isfinite(v)) return;
//   - No feedback path is created here: the only recirculation is INSIDE
//     ShimmerReverb, used strictly in-range.
//   - Peak bound rationale (long-hold gate: |out| <= 1.5 for |in| <= 1):
//     wetInt is a convex blend of washOut and loopOut; loopOut is a table of
//     past washOut values whose burn/splice operations are convex blends
//     (no gain), and the freeze crossfade is equal-power (worst-case factor
//     sqrt(2) for 15 ms overlaps); washOut = (1-m)*x + m*wet with m <= 0.9
//     and the FDN tail bounded by its < 1 loop gain; the resamplers are
//     unity-passband FIRs (transient overshoot only). The 1.5 ceiling is
//     the spec'd realistic envelope over that composition.
//   - reset() reinitialises everything deterministically (two runs from
//     reset() with identical inputs and calls are bit-identical) and leaves
//     total silence (residue 0).
//
#include "MicroLooper.h"
#include "ShimmerReverb.h"
#include "SmoothingCoeff.h"
#include "VariPolyphaseResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace factory_core
{
    class Madoromi
    {
    public:
        static constexpr int    kMaxChannels     = 2;
        static constexpr double kInternalRateHz  = 48000.0;
        static constexpr double kClockMinHz      = 8000.0;
        static constexpr double kClockMaxHz      = 48000.0;
        static constexpr double kClockTauMs      = 100.0;
        static constexpr double kParamSmoothMs   = 20.0;
        static constexpr double kToneMinHz       = 1500.0;
        static constexpr double kToneMaxHz       = 18000.0;
        static constexpr double kWashMixMax      = 0.9;
        static constexpr double kWashDecayBaseSec = 0.4;
        static constexpr double kWashDecayRatio  = 20.0;
        static constexpr int    kHostChunk       = 512;   // fixed internal slice (host samples)
        static constexpr int    kFifoMargin      = 16;    // occupancy floor at the clock floor
        static constexpr int    kFifoSafetyPad   = 64;    // extra prefill: covers the two
                                                          // stages' fractional-phase slop

        // -- published pure mappings / latency (tests re-derive these) ------
        static double washDecaySeconds (double v) noexcept
        {
            return kWashDecayBaseSec * std::pow (kWashDecayRatio, v);
        }

        static double toneToDamping (double toneHzIn) noexcept
        {
            return std::clamp (std::log (18000.0 / toneHzIn) / std::log (12.0), 0.0, 1.0);
        }

        static int fifoPrefillForRate (double fsHost) noexcept
        {
            const double d = (double) VariPolyphaseResampler::kHalfTaps;
            return kFifoMargin
                 + (int) std::ceil (2.0 * d * (fsHost / kClockMinHz
                                               - std::max (1.0, fsHost / kInternalRateHz)));
        }

        static int latencyForRate (double fsHost) noexcept
        {
            const double d = (double) VariPolyphaseResampler::kHalfTaps;
            return (int) std::lround (2.0 * d * std::max (1.0, fsHost / kInternalRateHz))
                 + fifoPrefillForRate (fsHost);
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            prefill  = fifoPrefillForRate (fs);
            latency  = latencyForRate (fs);
            fifoFill = latency + kFifoSafetyPad;   // wet FIFO prefill (see header)

            // Worst-case scratch sizes (fixed slices -> no n-dependent sizing).
            intCap = (int) std::ceil ((double) kHostChunk * kClockMaxHz / fs) + 16;
            upCap  = (int) std::ceil ((double) intCap * fs / kClockMinHz)
                   + VariPolyphaseResampler::kHalfTaps + 64;

            const double maxDownRatio = fs / kClockMinHz;
            const double maxUpRatio   = kClockMaxHz / fs;
            for (int lane = 0; lane < 2; ++lane)
            {
                down[(size_t) lane].prepare (fs, clockHz, maxDownRatio);
                up[(size_t) lane].prepare (clockHz, fs, maxUpRatio);

                hostIn[(size_t) lane].assign  ((size_t) kHostChunk, 0.0f);
                intBuf[(size_t) lane].assign  ((size_t) intCap, 0.0f);
                loopBuf[(size_t) lane].assign ((size_t) intCap, 0.0f);
                upOut[(size_t) lane].assign   ((size_t) upCap, 0.0f);
                wetHost[(size_t) lane].assign ((size_t) kHostChunk, 0.0f);

                fifo[(size_t) lane].prepare (fifoFill + upCap + kHostChunk + 64);
            }

            dryCap = latency + 8;
            for (int ch = 0; ch < kMaxChannels; ++ch)
                dryBuf[ch].assign ((size_t) dryCap, 0.0);

            // Internal section: fixed 48 kHz machine, clocked by the bracket.
            wash.prepare (kInternalRateHz);
            wash.setShimmer (0.0);          // pitch component OFF (in-range)
            wash.setFreeze (false);         // FDN stays lossy: loop gain < 1
            wash.setPitchASemis (0.0);
            wash.setPitchBSemis (0.0);
            wash.setVoiceBMix (0.0);
            wash.setPreDelayMs (0.0);
            wash.setModDepth (0.0);
            wash.setSize (1.0);

            looper.prepare (kInternalRateHz, 2);

            aHost = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);
            aInt  = 1.0 - onePoleCoeffForMs (kParamSmoothMs, kInternalRateHz);
            reset();
        }

        void reset() noexcept
        {
            clockSm = clockHz;
            washSm  = wash01;
            toneSm  = toneHz;
            balSm   = balance;
            mixSm   = mix;

            for (int lane = 0; lane < 2; ++lane)
            {
                down[(size_t) lane].setTargetRatio (fs / clockSm);
                up[(size_t) lane].setTargetRatio (clockSm / fs);
                down[(size_t) lane].reset();
                up[(size_t) lane].reset();
                fifo[(size_t) lane].reset();
                fifo[(size_t) lane].pushZeros (fifoFill);
            }

            wash.reset();
            applyWashParams();

            looper.reset();
            looper.setFrozen (frozenParam);   // params survive reset (house rule)

            for (int ch = 0; ch < kMaxChannels; ++ch)
                std::fill (dryBuf[ch].begin(), dryBuf[ch].end(), 0.0);
            dryPos = 0;
        }

        // -- parameters (audio thread, between process calls) ---------------
        // Non-finite values are ignored (house rule; bools excluded).
        void setClockHz (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            clockHz = std::clamp (v, kClockMinHz, kClockMaxHz);
        }

        void setWash01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            wash01 = std::clamp (v, 0.0, 1.0);
        }

        void setToneHz (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            toneHz = std::clamp (v, kToneMinHz, kToneMaxHz);
        }

        void setLengthSeconds (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            looper.setLengthSeconds (v);      // MicroLooper clamps [0.05, 2.0]
        }

        void setFrozen (bool f) noexcept
        {
            frozenParam = f;
            looper.setFrozen (f);
        }

        void setBalance01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            balance = std::clamp (v, 0.0, 1.0);
        }

        void setMix01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            mix = std::clamp (v, 0.0, 1.0);
        }

        int latencySamples() const noexcept { return latency; }

        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            int offset = 0;
            while (offset < numSamples)
            {
                const int h = std::min (kHostChunk, numSamples - offset);

                // 1) CLOCK smoothing (tau = 100 ms host time, advanced once per
                //    slice by the exact closed form) -> bracket ratio targets.
                const double aSlice = 1.0 - std::exp (-(double) h / (kClockTauMs * 1.0e-3 * fs));
                clockSm += aSlice * (clockHz - clockSm);
                for (int lane = 0; lane < 2; ++lane)
                {
                    down[(size_t) lane].setTargetRatio (fs / clockSm);
                    up[(size_t) lane].setTargetRatio (clockSm / fs);
                }

                // 2) Host input: finite guard, stereo lanes (mono duplicated).
                for (int i = 0; i < h; ++i)
                {
                    for (int lane = 0; lane < 2; ++lane)
                    {
                        const double raw = (double) audio[nCh == 2 ? lane : 0][offset + i];
                        hostIn[(size_t) lane][(size_t) i] = (float) (std::isfinite (raw) ? raw : 0.0);
                    }
                }

                // 3) Down-resample host -> internal (variable ratio).
                const int m0 = down[0].process (hostIn[0].data(), h, intBuf[0].data(), intCap);
                const int m1 = down[1].process (hostIn[1].data(), h, intBuf[1].data(), intCap);
                const int m  = std::min (m0, m1);

                // 4) Internal machine (fixed 48 kHz domain): wash, loop, blend.
                for (int j = 0; j < m; ++j)
                {
                    washSm += aInt * (wash01 - washSm);
                    toneSm += aInt * (toneHz - toneSm);
                    applyWashParams();

                    double l = (double) intBuf[0][(size_t) j];
                    double r = (double) intBuf[1][(size_t) j];
                    wash.processStereo (l, r);            // live world (washOut)
                    intBuf[0][(size_t) j]  = (float) l;
                    intBuf[1][(size_t) j]  = (float) r;
                    loopBuf[0][(size_t) j] = (float) l;   // looper records washOut
                    loopBuf[1][(size_t) j] = (float) r;
                }

                float* loopPtrs[2] = { loopBuf[0].data(), loopBuf[1].data() };
                looper.process (loopPtrs, 2, m);          // in place -> loopOut

                bool recovered = false;   // at most one full recovery per slice
                for (int j = 0; j < m; ++j)
                {
                    balSm += aInt * (balance - balSm);
                    double wetL = (1.0 - balSm) * (double) intBuf[0][(size_t) j]
                                +        balSm  * (double) loopBuf[0][(size_t) j];
                    double wetR = (1.0 - balSm) * (double) intBuf[1][(size_t) j]
                                +        balSm  * (double) loopBuf[1][(size_t) j];

                    // Wet-node finite guard: full wet-path self-recovery. Later
                    // non-finite samples of the same slice (produced before the
                    // recovery) are zeroed without re-triggering the reset.
                    if (! std::isfinite (wetL) || ! std::isfinite (wetR))
                    {
                        if (! recovered)
                        {
                            recoverWetPath();
                            recovered = true;
                        }
                        wetL = wetR = 0.0;
                    }
                    intBuf[0][(size_t) j] = (float) wetL;
                    intBuf[1][(size_t) j] = (float) wetR;
                }

                // 5) Up-resample internal -> host, FIFO to exact slice length.
                for (int lane = 0; lane < 2; ++lane)
                {
                    const int hostM = up[(size_t) lane].process (intBuf[(size_t) lane].data(), m,
                                                                 upOut[(size_t) lane].data(), upCap);
                    fifo[(size_t) lane].push (upOut[(size_t) lane].data(), hostM);
                    fifo[(size_t) lane].pull (wetHost[(size_t) lane].data(), h);
                }

                // 6) Dry compensation (exact L-sample integer delay) + mix.
                for (int i = 0; i < h; ++i)
                {
                    mixSm += aHost * (mix - mixSm);
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        dryBuf[ch][(size_t) dryPos] =
                            (double) hostIn[(size_t) (nCh == 2 ? ch : 0)][(size_t) i];

                        int rd = dryPos - latency;
                        if (rd < 0) rd += dryCap;
                        const double dryD = dryBuf[ch][(size_t) rd];

                        double w = (nCh == 2)
                                 ? (double) wetHost[(size_t) ch][(size_t) i]
                                 : 0.5 * ((double) wetHost[0][(size_t) i]
                                        + (double) wetHost[1][(size_t) i]);
                        if (! std::isfinite (w)) w = 0.0;   // final sanitize

                        audio[ch][offset + i] = (float) ((1.0 - mixSm) * dryD + mixSm * w);
                    }
                    if (++dryPos >= dryCap) dryPos = 0;
                }

                offset += h;
            }
        }

    private:
        // Fixed-capacity power-of-two ring delivering exactly h host samples per
        // slice (single audio thread; no locks). Mirrors the RateBracket FIFO.
        struct HostFifo
        {
            std::vector<float> buf;
            int mask = 0, rd = 0, wr = 0, count = 0;
            void prepare (int minSize)
            {
                int p = 1; while (p < minSize) p <<= 1;
                buf.assign ((size_t) p, 0.0f); mask = p - 1; rd = wr = count = 0;
            }
            void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); rd = wr = count = 0; }
            void pushZeros (int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = 0.0f; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void push (const float* x, int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = x[i]; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void pull (float* out, int n) noexcept
            {
                for (int i = 0; i < n; ++i) { if (count > 0) { out[i] = buf[(size_t) rd]; rd = (rd + 1) & mask; --count; } else out[i] = 0.0f; }
            }
        };

        void applyWashParams() noexcept
        {
            wash.setDecaySec (washDecaySeconds (washSm));
            wash.setMix (kWashMixMax * washSm);
            wash.setDamping (toneToDamping (toneSm));
        }

        // Wet-path self-recovery (wet-node finite guard). The dry line, the
        // smoothers and the parameter targets are untouched; the looper's
        // frozen intent is re-asserted after its state reset.
        void recoverWetPath() noexcept
        {
            wash.reset();
            looper.reset();
            looper.setFrozen (frozenParam);
            for (int lane = 0; lane < 2; ++lane)
            {
                down[(size_t) lane].reset();
                up[(size_t) lane].reset();
                fifo[(size_t) lane].reset();
                fifo[(size_t) lane].pushZeros (fifoFill);
            }
        }

        double fs       = 44100.0;
        int    channels = 2;

        std::array<VariPolyphaseResampler, 2> down, up;
        ShimmerReverb wash;
        MicroLooper   looper;
        std::array<HostFifo, 2> fifo;

        std::array<std::vector<float>, 2> hostIn, intBuf, loopBuf, upOut, wetHost;
        std::vector<double> dryBuf[kMaxChannels];

        // parameter targets
        double clockHz    = 32000.0;
        double wash01     = 0.5;
        double toneHz     = 6000.0;
        double balance    = 0.5;
        double mix        = 0.5;
        bool   frozenParam = false;

        // smoothed / runtime state
        double clockSm = 32000.0, washSm = 0.5, toneSm = 6000.0, balSm = 0.5, mixSm = 0.5;
        double aHost = 1.0, aInt = 1.0;
        int    latency = 0, prefill = 0, fifoFill = 0, intCap = 0, upCap = 0;
        int    dryCap = 8, dryPos = 0;
    };
} // namespace factory_core
