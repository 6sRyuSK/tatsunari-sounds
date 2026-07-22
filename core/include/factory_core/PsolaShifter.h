#pragma once
//
// factory_core/PsolaShifter.h — pitch-synchronous overlap-add (TD-PSOLA style)
// pitch shifter for monophonic material, using WSOLA-style correlation
// alignment in place of explicit glottal-epoch detection. One instance
// processes an L/R pair with a SINGLE analysis/alignment stream (the mid sum),
// so both channels receive identical grain timing and stay phase-locked.
//
// WHY THIS SHAPE
//   * Formant preservation is inherent: each grain is an unmodified input
//     segment (a two-period raised-cosine slice); only the grain SPACING is
//     changed (output spacing = period / ratio), so the spectral envelope —
//     the voice's formants — stays put while the perceived pitch moves.
//   * Grains are cut on a period grid anchored to the previous grain and
//     refined by a normalised cross-correlation search, which keeps
//     consecutive grains phase-coherent without a glottal-closure detector.
//   * Each grain is scaled by 1/ratio so the raised-cosine overlap sum stays
//     at unity gain for any ratio (at spacing D the window sum is P/D).
//
// LATENCY CONTRACT
//   latencySamples() == the lookahead set via setLookahead(), exactly and
//   constantly. While the track is UNVOICED the scheduler free-runs with a
//   frozen period, zero alignment offset and source centre = synthesis centre
//   minus the lookahead — the raised-cosine COLA identity (w(u) + w(u±P) == 1
//   pointwise) then makes the output a bit-near-exact pure delay of the input.
//   The caller must keep 2*period + period/4 + 4 <= lookahead for voiced
//   periods (one grain of future input plus the alignment search radius); a
//   grain that would violate it degrades to the unvoiced identity path rather
//   than reading unwritten input.
//
// REAL-TIME SAFETY: prepare() sizes every ring for the worst case the caller
// declares (maxLookahead / maxPeriod / maxBlock); setLookahead/setTrack/
// setRatio/process never allocate, lock, or make syscalls.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace factory_core
{
    class PsolaShifter
    {
    public:
        static constexpr double kMinRatio = 0.5;   // −12 st: hard clamp — bounds
        static constexpr double kMaxRatio = 2.0;   // +12 st: grain spacing and CPU

        void prepare (double sampleRate, int maxBlockIn,
                      int maxLookaheadIn, int maxPeriodIn)
        {
            fs           = sampleRate;
            maxBlock     = std::max (1, maxBlockIn);
            maxLookahead = std::max (16, maxLookaheadIn);
            maxPeriod    = std::max (8, maxPeriodIn);

            const int inNeed  = maxLookahead + 2 * maxPeriod + maxPeriod / 2 + maxBlock + 32;
            const int outNeed = maxBlock + 2 * maxPeriod + 32;
            inSize  = nextPow2 (inNeed);
            outSize = nextPow2 (outNeed);
            inMask  = inSize - 1;
            outMask = outSize - 1;

            inL.assign ((size_t) inSize, 0.0f);
            inR.assign ((size_t) inSize, 0.0f);
            mid.assign ((size_t) inSize, 0.0f);
            accL.assign ((size_t) outSize, 0.0);
            accR.assign ((size_t) outSize, 0.0);

            setLookahead (std::min (maxLookahead, (int) std::lround (fs * 0.05)));
            reset();
        }

        // Fixed structural delay (== reported latency), in samples. Changing it
        // re-anchors the scheduler; the caller treats it as a latency change
        // (host restart), so the discontinuity is acceptable.
        void setLookahead (int samples) noexcept
        {
            lookahead = std::clamp (samples, 16, maxLookahead);
            // Frozen period for the unvoiced identity path: a "generic" two-
            // period grain that always fits the lookahead budget.
            uvPeriod = std::min (fs / 220.0, ((double) lookahead - 4.0) * 0.5);
            uvPeriod = std::max (8.0, uvPeriod);
            resetScheduler();
        }

        int latencySamples() const noexcept { return lookahead; }

        void reset() noexcept
        {
            std::fill (inL.begin(),  inL.end(),  0.0f);
            std::fill (inR.begin(),  inR.end(),  0.0f);
            std::fill (mid.begin(),  mid.end(),  0.0f);
            std::fill (accL.begin(), accL.end(), 0.0);
            std::fill (accR.begin(), accR.end(), 0.0);
            writeCount = 0;
            resetScheduler();
        }

        // Per-hop track update from the caller's detector. `periodSamples` is
        // the detected fundamental period at the prepared rate.
        void setTrack (double periodSamples, bool voicedIn) noexcept
        {
            if (voicedIn && periodSamples >= 8.0 && periodSamples <= (double) maxPeriod)
            {
                trackPeriod = periodSamples;
                trackVoiced = true;
            }
            else
            {
                trackVoiced = false;
            }
        }

        void setRatio (double r) noexcept
        {
            trackRatio = std::clamp (r, kMinRatio, kMaxRatio);
        }

        // In-place stereo processing; R may be null for mono. Output sample t
        // corresponds to input sample t - latencySamples().
        void process (float* L, float* R, int n) noexcept
        {
            if (L == nullptr || n <= 0)
                return;

            int done = 0;
            while (done < n)
            {
                const int m = std::min (n - done, maxBlock);
                processChunk (L + done, R != nullptr ? R + done : nullptr, m);
                done += m;
            }
        }

    private:
        static int nextPow2 (int v) noexcept
        {
            int p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        void resetScheduler() noexcept
        {
            synthMark       = 0.0;
            analysisMark    = -(double) lookahead;
            prevGrainVoiced = false;
            trackVoiced     = false;
            trackPeriod     = uvPeriod;
            trackRatio      = 1.0;
            uvHold          = uvPeriod;
        }

        float readIn (const std::vector<float>& ring, double pos) const noexcept
        {
            // Linear interpolation; positions are absolute sample indices.
            const double fl = std::floor (pos);
            const double fr = pos - fl;
            const std::int64_t i0 = (std::int64_t) fl;
            const float a = ring[(size_t) (i0 & inMask)];
            const float b = ring[(size_t) ((i0 + 1) & inMask)];
            return a + (float) fr * (b - a);
        }

        void processChunk (float* L, float* R, int n) noexcept
        {
            // 1) Append the inputs (and the shared analysis mid) to the rings.
            for (int i = 0; i < n; ++i)
            {
                const float l = L[i];
                const float r = R != nullptr ? R[i] : l;
                const size_t w = (size_t) ((writeCount + i) & inMask);
                inL[w] = l;
                inR[w] = r;
                mid[w] = 0.5f * (l + r);
            }
            writeCount += n;

            // 2) Place every grain whose window can reach the samples emitted by
            //    this chunk. Grain centres advance by period/ratio, so the loop
            //    is bounded; the hard cap is a belt-and-braces guard only.
            const std::int64_t emitEnd = writeCount;                 // == emitCount + n
            const std::int64_t emitStart = emitEnd - n;
            int guard = 8 + (int) (2.5 * (double) n / 8.0);
            while (synthMark - currentHalfWidth() < (double) emitEnd && guard-- > 0)
                placeGrain (emitStart);

            // 3) Emit (and clear behind) the accumulated output.
            for (int i = 0; i < n; ++i)
            {
                const size_t o = (size_t) ((emitStart + i) & outMask);
                L[i] = (float) accL[o];
                if (R != nullptr) R[i] = (float) accR[o];
                accL[o] = 0.0;
                accR[o] = 0.0;
            }
        }

        double currentHalfWidth() const noexcept
        {
            return trackVoiced ? trackPeriod : uvHold;
        }

        void placeGrain (std::int64_t emitStart) noexcept
        {
            const double s = synthMark;

            // Unvoiced grains freeze the last budget-conforming voiced period
            // (uvHold), so the identity path keeps a CONSTANT period — the exact
            // raised-cosine COLA — and voiced/unvoiced boundaries avoid a period
            // jump.
            double P      = trackVoiced ? trackPeriod : uvHold;
            double r      = trackVoiced ? trackRatio  : 1.0;
            bool   voiced = trackVoiced;

            // Structural budget: one full grain of future input plus the search
            // radius must fit inside the lookahead. A violating period degrades
            // to the unvoiced identity grain instead of reading unwritten input.
            const double searchR = voiced ? std::min (P * 0.25, (double) maxPeriod * 0.25) : 0.0;
            if (2.0 * P + searchR + 4.0 > (double) lookahead)
            {
                voiced = false;
                P      = uvHold;
                r      = 1.0;
            }

            const double ciIdeal = s - (double) lookahead;
            double a = ciIdeal;

            if (voiced)
            {
                // Period-grid anchor to the previous grain's source centre, so
                // consecutive grains sit one (detected) period apart in the
                // input — the phase-coherence backbone.
                const double k = std::round ((ciIdeal - analysisMark) / P);
                double a0 = analysisMark + k * P;
                if (std::abs (a0 - ciIdeal) > P || ! prevGrainVoiced)
                    a0 = ciIdeal;      // re-anchor after silence / first grain
                else
                    a0 = refineByCorrelation (a0, P, searchR);
                a = a0;
            }

            // Clamp reads inside the written span (belt-and-braces; the budget
            // check above already guarantees this for conforming callers).
            a = std::min (a, (double) writeCount - P - 2.0);

            const double g    = voiced ? 1.0 / r : 1.0;   // COLA gain compensation
            const double Pout = P / (voiced ? r : 1.0);

            // Raised-cosine window over u in [-P, P], evaluated by rotation
            // recurrence (no per-sample cos()).
            const std::int64_t t0raw = (std::int64_t) std::ceil  (s - P);
            const std::int64_t t1    = (std::int64_t) std::floor (s + P);
            const std::int64_t t0    = std::max (t0raw, emitStart);
            if (t1 >= t0)
            {
                const double dTh  = kPi / P;
                double th = ((double) t0 - s) * dTh;      // in [-pi, pi]
                double c  = std::cos (th);
                double sn = std::sin (th);
                const double cD = std::cos (dTh);
                const double sD = std::sin (dTh);
                for (std::int64_t t = t0; t <= t1; ++t)
                {
                    const double w = 0.5 * (1.0 + c);
                    if (w > 0.0)
                    {
                        const double srcPos = a + ((double) t - s);
                        const size_t oi = (size_t) (t & outMask);
                        const double wg = w * g;
                        accL[oi] += wg * (double) readIn (inL, srcPos);
                        accR[oi] += wg * (double) readIn (inR, srcPos);
                    }
                    const double c2 = c * cD - sn * sD;   // rotate by dTh
                    sn = sn * cD + c * sD;
                    c  = c2;
                }
            }

            analysisMark    = a;
            prevGrainVoiced = voiced;
            if (voiced)
                uvHold = P;          // budget-checked above — safe to freeze on
            synthMark       = s + std::max (8.0, Pout);
        }

        // Search the offset (±searchR around a0) whose neighbourhood best
        // matches the previous grain's source neighbourhood on the mid signal
        // (normalised cross-correlation, stride-subsampled). Returns the
        // refined source centre.
        double refineByCorrelation (double a0, double P, double searchR) noexcept
        {
            const int S = (int) std::floor (searchR);
            if (S < 1)
                return a0;

            const int h  = std::max (4, (int) std::floor (P * 0.5));
            const int st = std::max (1, h / 32);          // ~64 taps per segment

            // Reference energy (constant across offsets).
            double refE = 1.0e-12;
            for (int d = -h; d <= h; d += st)
            {
                const double v = (double) readIn (mid, analysisMark + (double) d);
                refE += v * v;
            }

            double bestScore = -1.0e30, prevScore = -1.0e30, nextScore = -1.0e30;
            int    bestOff   = 0;
            double lastScore = -1.0e30;
            for (int o = -S; o <= S; ++o)
            {
                double num = 0.0;
                double eng = 1.0e-12;
                for (int d = -h; d <= h; d += st)
                {
                    const double x = (double) readIn (mid, a0 + (double) (o + d));
                    const double y = (double) readIn (mid, analysisMark + (double) d);
                    num += x * y;
                    eng += x * x;
                }
                const double score = num / std::sqrt (eng * refE);
                if (o == bestOff + 1) nextScore = score;    // right neighbour of current best
                if (score > bestScore)
                {
                    bestScore = score;
                    bestOff   = o;
                    prevScore = lastScore;
                    nextScore = -1.0e30;
                }
                lastScore = score;
            }

            // Parabolic sub-sample refinement over the integer offset scores, so
            // fractional-period material aligns without per-grain phase jitter.
            double off = (double) bestOff;
            if (prevScore > -1.0e29 && nextScore > -1.0e29)
            {
                const double den = prevScore - 2.0 * bestScore + nextScore;
                if (std::abs (den) > 1.0e-15)
                    off += std::clamp (0.5 * (prevScore - nextScore) / den, -1.0, 1.0);
            }
            return a0 + off;
        }

        static constexpr double kPi = 3.14159265358979323846;

        double fs = 48000.0;
        int    maxBlock = 512, maxLookahead = 0, maxPeriod = 0;
        int    inSize = 0, outSize = 0;
        std::int64_t inMask = 0, outMask = 0;

        std::vector<float>  inL, inR, mid;
        std::vector<double> accL, accR;

        std::int64_t writeCount = 0;

        int    lookahead = 256;
        double uvPeriod  = 218.0;   // generic fallback period (fits any lookahead)
        double uvHold    = 218.0;   // frozen period for unvoiced grains

        double synthMark    = 0.0;   // next grain centre, output time
        double analysisMark = 0.0;   // last grain centre, input time
        bool   prevGrainVoiced = false;

        double trackPeriod = 218.0;  // caller-fed detected period (samples)
        bool   trackVoiced = false;
        double trackRatio  = 1.0;
    };
} // namespace factory_core
