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
// Latency contract (tests depend on these exact formulas; D =
// VariPolyphaseResampler::kHalfTaps = 31, all quantities in host samples,
// fsHost = prepared host rate, C = preparedClockHz = the CLOCK value in
// effect at the moment prepare() is called -- a caller that needs the
// reported latency to reflect a specific clock MUST call setClockHz()
// BEFORE prepare(), the established convention in this codebase; see e.g.
// the test suite's prepareScheduleEngine()):
//   P0(fsHost) = kFifoMargin + ceil(2 * D * (fsHost / kClockMinHz
//                                            - max(1, fsHost / kInternalRateHz)))
//     -- occupancy margin covering a CLOCK SWEEP all the way down to the
//     floor kClockMinHz, independent of whatever clock is actually prepared
//     (fifoPrefillForRate() below).
//   fifoFill(fsHost) = lround(2 * D * max(1, fsHost / kInternalRateHz))
//                     + P0(fsHost) + kFifoSafetyPad
//     -- the wet output FIFO's prefill length (fifoFillForRate() below);
//     this is NOT itself the reported latency (see the D2 fix note below)
//     -- it is the FIFO's own internal, clock-sweep-safe occupancy budget,
//     and its value is UNCHANGED by the D2 fix.
//   G(fsHost, C) = lround(2 * D * max(1, fsHost / C))
//     -- the round-trip VariPolyphaseResampler group delay (down + up,
//     converted to host samples), evaluated at clock C. Computed from the
//     resamplers' own groupDelayInputSamples() (bracketGroupDelayHost()
//     below) -- the same proof style OmoideEcho.h's sibling bracket uses,
//     not a hand-rederivation.
//   L(fsHost, C) = latencyForRate(fsHost, C) = fifoFill(fsHost) + G(fsHost, C)
// The engine reports L (at the PREPARED clock), delays the dry path by
// exactly L integer samples, and prefills the wet FIFO with fifoFill(fsHost)
// zeros (NOT L). Because the FIFO is pulled at exactly the input rate, its
// occupancy sits at the constant fifoFill(fsHost) once steady state is
// reached; an input sample does not reach the up-resampler's output (i.e.
// become available to push into the FIFO) until G(fsHost, C) host samples
// later -- the cascade's own causal delay. Total: G(fsHost, C) +
// fifoFill(fsHost) == L(fsHost, C) exactly -- wet emerges at PRECISELY the
// reported latency, at the clock the engine was prepared with. QED.
//
// D2 fix (2026-07-12, approved): the previous revision of this header
// reported only lround(2*D*max(1,fsHost/kInternalRateHz)) + P0(fsHost) --
// omitting kFifoSafetyPad from the reported value (even though fifoFill, the
// quantity actually pushed into the FIFO, already included it) AND always
// evaluating the round-trip group delay at C == kInternalRateHz (48 kHz)
// regardless of the actual prepared clock. Together these meant wet actually
// emerged kFifoSafetyPad + G(fsHost, C_prepared) samples LATER than the
// reported latency at every clock other than exactly 48 kHz -- a PDC
// under-reporting bug (also producing a clock-dependent comb filter at
// mix < 1, since dry and wet were misaligned by that same clock-dependent
// amount). Fixed by folding BOTH terms into latencyForRate() itself,
// parameterised on the prepared clock -- this now matches OmoideEcho.h's
// already-correct sibling bracket (see that header's own "Latency contract"
// for the general pattern).
//
// Clock-automation caveat: latency can only be reported once (a JUCE
// AudioProcessor reports it exactly once per prepareToPlay), so it stays
// fixed at L(fsHost, preparedClockHz) for the life of that prepare() call.
// If CLOCK is automated to a different value afterward, the TRUE wet transit
// at the new clock (fifoFill(fsHost) + G(fsHost, C_new)) drifts away from
// the still-reported L -- this is INHERENT tape-repitch character (the same
// "clock pin" contract that makes wash/loop timing itself track CLOCK), not
// a residual defect: fifoFillForRate()'s own kClockMinHz-referenced margin
// still guarantees the FIFO never underruns across the full sweep, so the
// drift is a benign, bounded transit-length change, never a glitch. The
// defect this fix addresses is the STATIC omission at the PREPARED clock --
// L(fsHost, C) != true transit even with CLOCK held perfectly constant --
// which is now an exact equality (proof above).
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

        // Wet output FIFO prefill length (host samples of silence pushed at
        // reset()/recovery/resetForBypass()) -- P0(fsHost) unchanged, PLUS the
        // kInternalRateHz-referenced baseline term PLUS kFifoSafetyPad. See
        // the header's "Latency contract": this is a clock-sweep-safety
        // budget, NOT the reported latency, and its value is UNCHANGED by the
        // D2 fix below.
        static int fifoFillForRate (double fsHost) noexcept
        {
            const double d = (double) VariPolyphaseResampler::kHalfTaps;
            return (int) std::lround (2.0 * d * std::max (1.0, fsHost / kInternalRateHz))
                 + fifoPrefillForRate (fsHost)
                 + kFifoSafetyPad;
        }

        // Round-trip VariPolyphaseResampler group delay (HOST samples) at
        // clock C: down's own domain IS the host stream (ratio
        // rDown = fsHost/C), so its group delay is already in host samples;
        // up's own domain IS the C-rate stream (ratio rUp = C/fsHost), so its
        // raw group delay (C-rate samples) is converted to host samples by
        // * (fsHost/C). Structurally identical to OmoideEcho.h's own
        // (fixed-ratio) derivation, just evaluated at the variable clock C.
        static double bracketGroupDelayHost (double fsHost, double clockHzPrepared) noexcept
        {
            const double rDown = fsHost / clockHzPrepared;
            const double rUp   = clockHzPrepared / fsHost;
            const double downDelayHost = VariPolyphaseResampler::groupDelayInputSamples (rDown);
            const double upDelayHost   = VariPolyphaseResampler::groupDelayInputSamples (rUp)
                                        * (fsHost / clockHzPrepared);
            return downDelayHost + upDelayHost;
        }

        // TRUE wet transit / reported latency (host samples) at clock
        // clockHzPrepared -- see the header's "Latency contract" / "D2 fix"
        // notes. clockHzPrepared is the CLOCK in effect at prepare() time
        // (preparedClockHz below); this is exactly what prepare() calls.
        static int latencyForRate (double fsHost, double clockHzPrepared) noexcept
        {
            const double c = std::clamp (clockHzPrepared, kClockMinHz, kClockMaxHz);
            return fifoFillForRate (fsHost)
                 + (int) std::lround (bracketGroupDelayHost (fsHost, c));
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            // D2 fix: latency is fixed HERE, at prepare() time, from whatever
            // clock TARGET (clockHz) is current -- see the header's Latency
            // contract. A caller that needs the reported latency to reflect a
            // specific clock value MUST call setClockHz() before prepare().
            preparedClockHz = std::clamp (clockHz, kClockMinHz, kClockMaxHz);

            prefill  = fifoPrefillForRate (fs);
            fifoFill = fifoFillForRate (fs);                       // clock-independent (see header)
            latency  = latencyForRate (fs, preparedClockHz);       // TRUE transit at preparedClockHz

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
            // Determinism fix (2026-07-13, approved): latency/dryCap became
            // CLOCK-dependent by the D2 fix (latencyForRate(fsHost, C)), but a
            // plain reset() used to leave preparedClockHz/latency/dryCap at
            // whatever prepare() last computed them as -- so if clockHz had
            // been changed since prepare() (with no intervening prepare()
            // call), reset()-then-run would silently keep the STALE dry-line
            // length/delay instead of the one a fresh prepare() at the CURRENT
            // clockHz would produce, breaking "reset()-then-rerun == fresh-
            // prepare run" bit-exactness. Re-derive both here, exactly as
            // prepare() does, and resize the dry line if its length changed.
            // (Only prepare() itself -- the message thread -- calls this full
            // reset(); resetForBypass() is the separate, allocation-free,
            // audio-thread-safe variant and deliberately does NOT do this --
            // see its own contract comment: JUCE's reported latency must not
            // change mid-stream from a bypass toggle.)
            preparedClockHz = std::clamp (clockHz, kClockMinHz, kClockMaxHz);
            latency = latencyForRate (fs, preparedClockHz);

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

            const int newDryCap = latency + 8;
            if (newDryCap != dryCap)
            {
                dryCap = newDryCap;
                for (int ch = 0; ch < kMaxChannels; ++ch)
                    dryBuf[ch].assign ((size_t) dryCap, 0.0);
            }
            else
            {
                for (int ch = 0; ch < kMaxChannels; ++ch)
                    std::fill (dryBuf[ch].begin(), dryBuf[ch].end(), 0.0);
            }
            dryPos = 0;
        }

        // D6 fix (bypass de-click, approved): a plain reset() on a bypass
        // transition zeros the dry compensation delay line too, producing an
        // audible dropout (the dry path is a pure passthrough delay, not a
        // feedback/history node -- it does not need clearing for state
        // hygiene). resetForBypass() resets every ACTUAL DSP/feedback node
        // (wash FDN, looper, both resamplers, both output FIFOs) exactly like
        // reset() above, but LEAVES dryBuf/dryPos untouched, so the dry path
        // stays sample-continuous across the transition. It also snaps every
        // smoother (including mixSm) to its CURRENT target immediately,
        // rather than letting it glide: the wet-side content is jumping
        // discontinuously (to silence, on reset) at this exact instant, so
        // gliding the crossfade WEIGHT against that jump would blend a stale
        // weight against a discontinuous signal for up to kParamSmoothMs --
        // audible. Snapping makes the weight and the content change together.
        // Call this (not reset()) from the wrapper on bypass transitions;
        // keep the full reset() for prepare()/channel-count changes. The
        // wrapper must push its bypass-appropriate parameter targets (mix in
        // particular) BEFORE calling this, so mixSm snaps to the right value.
        void resetForBypass() noexcept
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

            // dryBuf / dryPos: DELIBERATELY NOT touched -- see contract above.
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
        double preparedClockHz = kClockMaxHz;   // clock captured at prepare() time (D2 fix)
        int    latency = 0, prefill = 0, fifoFill = 0, intCap = 0, upCap = 0;
        int    dryCap = 8, dryPos = 0;
    };
} // namespace factory_core
