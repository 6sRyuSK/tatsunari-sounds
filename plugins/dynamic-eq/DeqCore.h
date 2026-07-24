#pragma once
//
// DeqCore.h — the dynamic-EQ block-processing core, extracted as a plain C++ unit
// with NO plugin-framework dependency so the CLAP shell and the equivalence oracle
// drive the exact same DSP. It is a faithful CODE MOTION of PluginProcessor.cpp's
// prepareToPlay + processBlock math: it owns the same 24 factory_core::DynamicEqBand
// cascade, the same per-band Freq/Gain/Q linear smoothers, the same pre/post
// analyzer rings, and publishes the same per-band live (post-dynamics) gain the
// editor animates. The shipping JUCE AudioProcessor is retained UNCHANGED as the
// byte-equivalence oracle (deqcore_equiv_test) — this is a parallel unit the CLAP
// shell reuses, not a refactor of the wrapper.
//
// The caller SNAPSHOTS every per-block parameter into DeqParamSnapshot (plain
// scalars / enums — no framework types, no atomics) and hands it to process().
// Fields are stored in the SAME post-load form the processor's atomic reads
// produce (a float widened to double, a `> 0.5f` truth test, or an `(int)` cast),
// so a snapshot taken from the live parameters feeds DeqCore bit-for-bit what the
// processor feeds its bands.
//
// FAITHFULNESS MAP (call-site line refs are into PluginProcessor.cpp as of this
// extraction): prepare() mirrors prepareToPlay (l.142-164); process() mirrors
// processBlock's bypass early-out (l.183-189), the per-block band configuration
// (l.196-217), the solo/active-band selection (l.224-240), the sub-block smoothing
// + per-sample loop (l.242-287), and the live-gain publish (l.290-292);
// copyAnalyzerSamples mirrors copyAnalyzerSamples (l.295-301).
//
// Header-only, dependency-free at the framework level: it composes only
// factory_core primitives (DynamicEqBand + LinearRamp). Real-time safe: prepare()
// sizes/settles every buffer up front; process() never allocates, locks, or makes
// a syscall.
//
#include "factory_core/DynamicEqBand.h"
#include "factory_core/LinearRamp.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace deq_core
{
    // Transcribed from PluginProcessor.h: kNumBands, kSmoothChunk, kRingSize/Mask.
    inline constexpr int kNumBands    = 24;
    inline constexpr int kSmoothChunk = 32;        // samples per coefficient update
    inline constexpr int kRingSize    = 1 << 14;   // 16384 (>= analyzer FFT, with margin)
    inline constexpr int kRingMask    = kRingSize - 1;

    // Ramp length for the continuous Freq / Gain / Q (~30 ms), matching
    // prepareToPlay's kRampSeconds (l.153).
    inline constexpr double kRampSeconds = 0.03;

    // Every value processBlock pulls per band, snapshotted by the caller in the
    // exact post-load form the processor's atomic reads produce.
    struct DeqBandSnapshot
    {
        bool   on   = false;  // *on->load()   > 0.5f
        bool   byp  = false;  // *byp->load()  > 0.5f
        bool   lsn  = false;  // *lsn->load()  > 0.5f
        bool   dyn  = false;  // *dyn->load()  > 0.5f
        int    chan = 0;      // (int) *chan->load()  -> ChannelMode
        int    type = 0;      // (int) *type->load()  -> BandType
        int    slope = 0;     // (int) *slope->load() (choice index 0..7; +1 -> sections)
        double freq = 1000.0; // (double) *freq->load()
        double gain = 0.0;    // (double) *gain->load()  (dB)
        double q    = 0.707;  // (double) *q->load()
        double thr  = -24.0;  // (double) *thr->load()   (dB)
        double rng  = 0.0;    // (double) *rng->load()   (dB)
        double atk  = 10.0;   // (double) *atk->load()   (ms)
        double rel  = 120.0;  // (double) *rel->load()   (ms)
        double knee = 6.0;    // (double) *knee->load()  (dB)
    };

    struct DeqParamSnapshot
    {
        std::array<DeqBandSnapshot, kNumBands> bands {};
        bool bypass = false;  // global "bypass" > 0.5f
    };

    class DeqCore
    {
    public:
        DeqCore() = default;

        // Mirror prepareToPlay (l.142-164): prepare the 24 bands, clear both rings,
        // reset the write counter, latch each smoother's ~30 ms ramp length, and
        // reset live gains. The per-smoother SEED from the live parameters
        // (prepareToPlay l.159-161) is DEFERRED to the first process() block (see
        // primed_) because prepare() carries no parameters — the code-motion of the
        // seed, sourced from that block's own snapshot. With params stable across
        // prepare -> first block (the wrapper's normal case, and how the oracle is
        // driven) this reproduces "the first block starts settled" exactly.
        // samplesPerBlock is accepted for wrapper symmetry but unused, exactly like
        // the processor (which ignores its samplesPerBlock argument).
        void prepare (double sampleRate, int /*samplesPerBlock*/) noexcept
        {
            currentSampleRate_.store (sampleRate, std::memory_order_relaxed);
            for (auto& band : bands_)
                band.prepare (sampleRate);
            ringPre_.fill (0.0f);
            ringPost_.fill (0.0f);
            ringWrite_.store (0, std::memory_order_relaxed);

            for (int b = 0; b < kNumBands; ++b)
            {
                freqS_[(size_t) b].reset (sampleRate, kRampSeconds);
                gainS_[(size_t) b].reset (sampleRate, kRampSeconds);
                qS_[(size_t) b].reset    (sampleRate, kRampSeconds);
                liveGainDb_[(size_t) b].store (0.0f, std::memory_order_relaxed);
            }
            primed_ = false;
        }

        // Mirror processBlock's inner work on caller-owned buffers. L/R is the main
        // signal (processed in place); R may be nullptr (mono), read as r == l,
        // exactly like the processor's `R = numCh > 1 ? … : nullptr`. The caller
        // owns bus extraction / output-channel clearing (the framework housekeeping
        // in processBlock l.178-181) — DeqCore only owns the DSP. Real-time safe.
        void process (float* L, float* R, int numSamples, const DeqParamSnapshot& s) noexcept
        {
            // Seed the smoothers on the FIRST block from that block's snapshot (the
            // deferred prepareToPlay seed, l.159-161) — for ALL bands, present or
            // not, exactly as prepareToPlay reads every band's live params.
            if (! primed_)
            {
                for (int b = 0; b < kNumBands; ++b)
                {
                    freqS_[(size_t) b].setCurrentAndTargetValue (s.bands[(size_t) b].freq);
                    gainS_[(size_t) b].setCurrentAndTargetValue (s.bands[(size_t) b].gain);
                    qS_[(size_t) b].setCurrentAndTargetValue    (s.bands[(size_t) b].q);
                }
                primed_ = true;
            }

            // Bypass (l.183-189): publish the STATIC gain for the display, leave L/R
            // untouched (the shell's output already carries the input, so untouched =
            // passthrough), and do NOT advance the ring.
            if (s.bypass)
            {
                for (int b = 0; b < kNumBands; ++b)
                    liveGainDb_[(size_t) b].store ((float) s.bands[(size_t) b].gain, std::memory_order_relaxed);
                return;
            }

            // Per-block band configuration (l.196-217). Continuous Freq/Gain/Q are
            // smoothed + applied in the sub-block loop below; only the non-continuous
            // settings (type/channel/slope/knee/dynamics) are set per block.
            for (int b = 0; b < kNumBands; ++b)
            {
                const auto& bp = s.bands[(size_t) b];
                auto& band = bands_[(size_t) b];
                const bool present  = bp.on;
                const bool bypassed = bp.byp;
                band.setEnabled (present && ! bypassed);
                if (! present)
                    continue; // free slot: skip coefficient work entirely
                band.setType (static_cast<factory_core::BandType> (bp.type));
                band.setChannelMode (static_cast<factory_core::ChannelMode> (bp.chan));
                band.setSlopeStages (bp.slope + 1); // choice index 0..7 -> 1..8 sections
                band.setKnee (bp.knee);
                band.setDynamics (bp.dyn, bp.thr, bp.rng);
                band.setDynamicsTimes (bp.atk, bp.rel);

                freqS_[(size_t) b].setTargetValue (bp.freq);
                gainS_[(size_t) b].setTargetValue (bp.gain);
                qS_[(size_t) b].setTargetValue    (bp.q);
            }

            // Exclusive Listen/solo: audition the lowest present band with it on
            // (l.224-228).
            int soloBand = -1;
            for (int b = 0; b < kNumBands; ++b)
                if (s.bands[(size_t) b].on && s.bands[(size_t) b].lsn) { soloBand = b; break; }

            // Compact list of the enabled bands, ascending (l.236-240): free /
            // bypassed slots are skipped so an idle instance stops spinning.
            std::array<int, (size_t) kNumBands> activeBands;
            int numActiveBands = 0;
            for (int b = 0; b < kNumBands; ++b)
                if (bands_[(size_t) b].isEnabled())
                    activeBands[(size_t) numActiveBands++] = b;

            std::uint64_t w = ringWrite_.load (std::memory_order_relaxed);
            for (int start = 0; start < numSamples; start += kSmoothChunk)
            {
                const int end = (start + kSmoothChunk < numSamples) ? start + kSmoothChunk : numSamples;
                const int chunk = end - start;

                // Advance the smoothers by this chunk and reconfigure each present
                // band's Freq/Gain/Q + coefficients before the chunk (l.250-265).
                for (int b = 0; b < kNumBands; ++b)
                {
                    if (! s.bands[(size_t) b].on)
                    {
                        // Keep the smoothers moving so a freed slot doesn't jump.
                        freqS_[(size_t) b].skip (chunk);
                        gainS_[(size_t) b].skip (chunk);
                        qS_[(size_t) b].skip    (chunk);
                        continue;
                    }
                    auto& band = bands_[(size_t) b];
                    band.setFrequency (freqS_[(size_t) b].skip (chunk));
                    band.setGainDb    (gainS_[(size_t) b].skip (chunk));
                    band.setQ         (qS_[(size_t) b].skip    (chunk));
                    band.updateCoefficients();
                }

                for (int i = start; i < end; ++i)
                {
                    double l = L[i];
                    double r = (R != nullptr) ? R[i] : l;

                    ringPre_[(size_t) (w & (std::uint64_t) kRingMask)] = (float) (0.5 * (l + r)); // pre-EQ

                    if (soloBand >= 0)
                        bands_[(size_t) soloBand].processListen (l, r); // band-pass of the dry input
                    else
                        for (int idx = 0; idx < numActiveBands; ++idx)
                            bands_[(size_t) activeBands[(size_t) idx]].processStereo (l, r);

                    ringPost_[(size_t) (w & (std::uint64_t) kRingMask)] = (float) (0.5 * (l + r)); // post-EQ
                    ++w;

                    L[i] = (float) l;
                    if (R != nullptr) R[i] = (float) r;
                }
            }
            ringWrite_.store (w, std::memory_order_release);

            // Publish each band's effective (post-dynamics) gain for the editor
            // (l.290-292).
            for (int b = 0; b < kNumBands; ++b)
                liveGainDb_[(size_t) b].store ((float) bands_[(size_t) b].currentGainDb(),
                                               std::memory_order_relaxed);
        }

        // --- published read-outs (mirror the processor's published semantics) ----
        // Copy the latest `num` analyzer samples (mono) into dest. `post` selects
        // the post-EQ ring instead of the pre-EQ input (mirrors copyAnalyzerSamples
        // l.295-301). GUI thread; lock-free.
        void copyAnalyzerSamples (float* dest, int num, bool post) const noexcept
        {
            const std::uint64_t w = ringWrite_.load (std::memory_order_acquire);
            const auto& ring = post ? ringPost_ : ringPre_;
            for (int i = 0; i < num; ++i)
                dest[i] = ring[(size_t) ((w - (std::uint64_t) num + (std::uint64_t) i) & (std::uint64_t) kRingMask)];
        }

        // Per-band effective gain (dB) including the live dynamic offset. Lock-free;
        // GUI thread reads (mirrors getLiveGainDb, PluginProcessor.h l.68-71).
        float liveGainDb (int band) const noexcept
        {
            return liveGainDb_[(size_t) band].load (std::memory_order_relaxed);
        }

        int    numBands()   const noexcept { return kNumBands; }
        double sampleRate() const noexcept { return currentSampleRate_.load (std::memory_order_relaxed); }

    private:
        std::array<factory_core::DynamicEqBand, kNumBands> bands_;

        // Per-band smoothing of the continuous Freq / Gain / Q — bit-exact linear
        // stand-ins for the juce::SmoothedValue<double> the shipping processor uses
        // (freqSmooth/gainSmooth/qSmooth), so the ramped coefficients render
        // identically.
        std::array<factory_core::LinearRamp<double>, kNumBands> freqS_, gainS_, qS_;

        // Analyzer rings (single producer: the process thread).
        std::array<float, kRingSize> ringPre_ {};   // pre-EQ (input)
        std::array<float, kRingSize> ringPost_ {};  // post-EQ (output)
        // Monotonic sample counter, masked to index the ring. Unsigned + 64-bit so
        // the increment is well-defined wrapping and never wraps in a realistic run
        // (mirrors PluginProcessor.h l.114-122).
        std::atomic<std::uint64_t> ringWrite_ { 0 };

        // Live per-band effective gain (dB) for the editor's animated display.
        std::array<std::atomic<float>, kNumBands> liveGainDb_ {};

        std::atomic<double> currentSampleRate_ { 44100.0 };
        bool                primed_ = false; // seed smoothers on the FIRST process block
    };
} // namespace deq_core
