#pragma once
//
// factory_core/MicroLooper.h -- an always-recording micro-loop primitive
// (Chase Bliss MOOD-style "loop world"): a ring buffer records the input at
// all times; engaging freeze captures the most recent LENGTH window, burns
// the wrap crossfade INTO the captured table once, and then plays the table
// back as a pure read -- so the frozen output is EXACTLY P-periodic
// (bit-exact) from the first pass. No feedback path anywhere. Header-only,
// JUCE-independent; prepare() allocates, process() does not (no locks, no
// syscalls, noexcept).
//
// Constants (spec'd, tests depend on them):
//   kMaxChannels   = 2
//   kMaxLoopSec    = 2.0
//   kMinLoopSec    = 0.05
//   kWrapXfadeMs   = 10.0   // loop wrap splice, burned into the table at freeze
//   kFreezeXfadeMs = 15.0   // live <-> loop switch crossfade
//   kParamSmoothMs = 20.0   // house smoothing constant (published per spec;
//                           // length is wrap-quantised and freeze is
//                           // crossfaded, so no continuous smoother is
//                           // currently active in this primitive)
//
// Derived sizes at prepare(fs):
//   W       = llround(kWrapXfadeMs   * 1e-3 * fs)   (wrap splice, samples)
//   F       = max(1, llround(kFreezeXfadeMs * 1e-3 * fs)) (freeze fade, samples)
//   Pcap    = ceil(kMaxLoopSec * fs)
//   ring    = Pcap + W + 8 samples per channel      (always-recording history)
//   snap    = Pcap + W samples per channel          (pristine freeze snapshot)
//   table   = Pcap + 1 samples per channel          (burned playback table)
//
// Freeze capture contract (t = index of the FIRST sample processed with the
// frozen state; the window excludes the input at t itself):
//   P      = llround(lengthSeconds * fs)            (clamped length)
//   window = x[t-P .. t-1], table[j] = x[t-P+j]
//   Wrap crossfade burned once at freeze, blending the window tail with the
//   history samples IMMEDIATELY PRECEDING the window, k = 0..W-1:
//     a_k              = 0.5 * (1 - cos(pi * (k+1) / W))     (raised cosine)
//     table[P-W+k]     = (1 - a_k) * x[t-W+k] + a_k * x[t-P-W+k]
//   Because a_{W-1} = 1 exactly, the last table sample equals x[t-P-1], so
//   the wrap table[P-1] -> table[0] = x[t-P] lands on ORIGINAL-time adjacent
//   samples: the splice is seamless and playback is a pure table read --
//   exactly P-periodic, bit-exact, from the first pass. The burn is O(P+W)
//   writes at the freeze edge (the snapshot copy is O(Pcap+W)); no allocation.
//
// Freeze on/off crossfade (equal power, click-free): an integer counter
// c in [0, F] moves by +/-1 per sample toward (frozen ? F : 0); with
// g = c / F the output is
//   c == 0             ->  out = in            (EXACT transparency, no math)
//   c == F             ->  out = table read    (EXACT, pure table read)
//   otherwise          ->  out = cos(pi/2 * g) * in + sin(pi/2 * g) * loop
// The loop position starts at 0 on the freeze edge and advances every sample
// while engaged (frozen or still fading out).
//
// Length changes while frozen are WRAP-QUANTISED (click-free, deterministic):
// setLengthSeconds() only arms pendingP = llround(length * fs); at the next
// wrap the table is rebuilt for pendingP from the FREEZE-TIME SNAPSHOT (the
// loop content stays from the freeze moment; the ring keeps recording live
// input independently), the tail burn above is re-applied for the new P, and
// a transient HEAD SPLICE crossfades from the old table into the new one over
// the first W samples of the first new pass, k = 0..W-1:
//   b_k = 0.5 * (1 - cos(pi * (k+1) / W))
//   y_k = (1 - b_k) * oldTable[k] + b_k * newTable[k]
// From the second pass of the new period the output is again a pure table
// read: P2-periodicity holds bit-exactly from pass 2 onward.
//
// Safety / state rules (house standard):
//   - Input finite guard: a non-finite input sample is treated as 0 before
//     it can reach the ring or the output.
//   - Every non-bool public setter ignores non-finite values:
//     if (!std::isfinite(v)) return;
//   - reset() clears all buffers, unfreezes, zeroes every counter -- two
//     runs from reset() with identical inputs/calls are bit-identical.
//   - While not engaged the primitive is a bit-exact pass-through.
//
#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class MicroLooper
    {
    public:
        static constexpr int    kMaxChannels   = 2;
        static constexpr double kMaxLoopSec    = 2.0;
        static constexpr double kMinLoopSec    = 0.05;
        static constexpr double kWrapXfadeMs   = 10.0;
        static constexpr double kFreezeXfadeMs = 15.0;
        static constexpr double kParamSmoothMs = 20.0;

        void prepare (double sampleRate, int numChannels)
        {
            fs       = std::max (8000.0, sampleRate);
            channels = std::clamp (numChannels, 1, kMaxChannels);

            wrapLen  = (int) std::max (1LL, std::llround (kWrapXfadeMs   * 1.0e-3 * fs));
            fadeLen  = (int) std::max (1LL, std::llround (kFreezeXfadeMs * 1.0e-3 * fs));
            pCap     = (int) std::ceil (kMaxLoopSec * fs);
            ringCap  = pCap + wrapLen + 8;
            snapLen  = pCap + wrapLen;

            for (int ch = 0; ch < channels; ++ch)
            {
                ring[ch].assign ((size_t) ringCap, 0.0);
                snap[ch].assign ((size_t) snapLen, 0.0);
                tab[ch].assign  ((size_t) (pCap + 1), 0.0);
                oldHead[ch].assign ((size_t) (wrapLen + 1), 0.0);
            }
            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                std::fill (ring[ch].begin(),    ring[ch].end(),    0.0);
                std::fill (snap[ch].begin(),    snap[ch].end(),    0.0);
                std::fill (tab[ch].begin(),     tab[ch].end(),     0.0);
                std::fill (oldHead[ch].begin(), oldHead[ch].end(), 0.0);
            }
            ringPos    = 0;
            frozen     = false;
            freezeReq  = false;
            fadeCount  = 0;
            loopPos    = 0;
            pCur       = periodFromLength();
            pPending   = pCur;
            spliceLeft = 0;
        }

        // -- parameters ----------------------------------------------------
        // While frozen the new length is armed and applied at the next wrap
        // (wrap quantisation); while live it is used by the next freeze.
        void setLengthSeconds (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            lenSec = std::clamp (v, kMinLoopSec, kMaxLoopSec);
            const int p = periodFromLength();
            if (frozen) pPending = p;
            else        pPending = pCur = p;
        }

        // bool parameter: excluded from the non-finite guard rule by spec.
        void setFrozen (bool f) noexcept { freezeReq = f; }

        bool isFrozen() const noexcept { return frozen; }
        int  currentPeriodSamples() const noexcept { return pCur; }

        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            for (int i = 0; i < numSamples; ++i)
            {
                // 1) Latch the freeze request edge. On a rising edge the
                //    snapshot/burn happens BEFORE this sample's ring write, so
                //    the window is exactly the P samples preceding the first
                //    frozen output sample.
                if (freezeReq != frozen)
                {
                    frozen = freezeReq;
                    if (frozen)
                        captureAndBurn (nCh);
                }

                const bool engaged = frozen || fadeCount > 0;

                for (int ch = 0; ch < nCh; ++ch)
                {
                    // Input finite guard (house rule): NaN/Inf becomes 0 on
                    // every path.
                    const double raw = (double) audio[ch][i];
                    const double in  = std::isfinite (raw) ? raw : 0.0;

                    // 2) Always-recording ring (records during freeze too; the
                    //    frozen loop reads from the snapshot, not the ring).
                    ring[ch][(size_t) ringPos] = in;

                    // 3) Output: exact branches at the fade endpoints keep the
                    //    pass-through and the frozen loop bit-exact.
                    if (! engaged)
                    {
                        audio[ch][i] = (float) in;   // bit-exact pass-through
                        continue;
                    }

                    double loopVal = tab[ch][(size_t) loopPos];
                    if (spliceLeft > 0)
                    {
                        const int    k = wrapLen - spliceLeft;
                        const double b = 0.5 * (1.0 - std::cos (kPi * (double) (k + 1) / (double) wrapLen));
                        loopVal = (1.0 - b) * oldHead[ch][(size_t) k] + b * loopVal;
                    }

                    if (fadeCount >= fadeLen)
                        audio[ch][i] = (float) loopVal;              // pure table read
                    else if (fadeCount <= 0)
                        audio[ch][i] = (float) in;                   // fade not started
                    else
                    {
                        const double g = (double) fadeCount / (double) fadeLen;
                        const double wLive = std::cos (0.5 * kPi * g);
                        const double wLoop = std::sin (0.5 * kPi * g);
                        audio[ch][i] = (float) (wLive * in + wLoop * loopVal);
                    }
                }

                // 4) Shared per-sample state advance (once per sample).
                if (++ringPos >= ringCap) ringPos = 0;

                if (spliceLeft > 0) --spliceLeft;

                if (engaged)
                {
                    if (++loopPos >= pCur)
                    {
                        loopPos = 0;
                        if (pPending != pCur)
                            applyPendingLength (nCh);   // wrap-quantised change
                    }
                }

                fadeCount += frozen ? 1 : -1;
                fadeCount  = std::clamp (fadeCount, 0, fadeLen);
            }
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;

        int periodFromLength() const noexcept
        {
            return (int) std::llround (lenSec * fs);
        }

        // Rebuild the playback table for period p from the freeze snapshot and
        // burn the wrap-tail crossfade (see the header contract). O(p + W).
        void buildTable (int p, int nCh) noexcept
        {
            const int base = snapLen - p;               // snap[base + j] = x[t-P+j]
            for (int ch = 0; ch < nCh; ++ch)
            {
                for (int j = 0; j < p; ++j)
                    tab[ch][(size_t) j] = snap[ch][(size_t) (base + j)];

                for (int k = 0; k < wrapLen; ++k)
                {
                    const double a = 0.5 * (1.0 - std::cos (kPi * (double) (k + 1) / (double) wrapLen));
                    tab[ch][(size_t) (p - wrapLen + k)] =
                        (1.0 - a) * snap[ch][(size_t) (base + p - wrapLen + k)]
                      +        a  * snap[ch][(size_t) (base - wrapLen + k)];
                }
            }
        }

        // Freeze rising edge: linearise the ring history into the pristine
        // snapshot (oldest -> newest, ending one sample before "now"), then
        // build the table for the current length. O(Pcap + W); allocation-free.
        void captureAndBurn (int nCh) noexcept
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                int r = ringPos - snapLen;
                while (r < 0) r += ringCap;
                for (int j = 0; j < snapLen; ++j)
                {
                    snap[ch][(size_t) j] = ring[ch][(size_t) r];
                    if (++r >= ringCap) r = 0;
                }
            }
            pCur = pPending = periodFromLength();
            buildTable (pCur, nCh);
            loopPos    = 0;
            spliceLeft = 0;
        }

        // Wrap-quantised length change: save the old table head for the
        // transient head splice, rebuild for the pending period. O(P2 + W).
        void applyPendingLength (int nCh) noexcept
        {
            for (int ch = 0; ch < nCh; ++ch)
                for (int k = 0; k < wrapLen; ++k)
                    oldHead[ch][(size_t) k] = tab[ch][(size_t) k];

            buildTable (pPending, nCh);
            pCur       = pPending;
            spliceLeft = wrapLen;
        }

        double fs       = 44100.0;
        int    channels = 2;

        std::vector<double> ring[kMaxChannels];
        std::vector<double> snap[kMaxChannels];
        std::vector<double> tab[kMaxChannels];
        std::vector<double> oldHead[kMaxChannels];

        // parameter targets
        double lenSec = 0.5;

        // runtime state
        int  ringPos    = 0;
        bool frozen     = false;
        bool freezeReq  = false;
        int  fadeCount  = 0;
        int  loopPos    = 0;
        int  pCur       = 1;
        int  pPending   = 1;
        int  spliceLeft = 0;

        int wrapLen = 1, fadeLen = 1, pCap = 1, ringCap = 4, snapLen = 1;
    };
} // namespace factory_core
