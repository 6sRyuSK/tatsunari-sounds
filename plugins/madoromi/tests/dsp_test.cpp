//
// dsp_test.cpp -- headless verification of the madoromi DSP core.
//
// Spec-based suite per scratchpad/madoromi-test-plan.md (FINAL, 2026-07-12).
// Oracles: (a) public contracts/constants/pure functions of MicroLooper.h /
// VariPolyphaseResampler.h / Madoromi.h (contract verification, not derived
// from the process() code path under test), (b) analytic formulas transcribed
// independently test-side. See .claude/skills/write-dsp-test and
// docs/regression-policy.md.
//
#include "factory_core/Madoromi.h"
#include "factory_core/MicroLooper.h"
#include "factory_core/VariPolyphaseResampler.h"
#include "factory_core/KaiserBessel.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace fct = factory_core::testing;
    namespace fc  = factory_core;

    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    void fail (const std::string& m)
    {
        ++g_failures;
        std::fprintf (stderr, "FAIL: %s\n", m.c_str());
    }

    // ---------------------------------------------------------------- §3 --
    // Deterministic LCG, standard across the suite.
    struct Lcg
    {
        uint64_t s;
        explicit Lcg (uint64_t seed) : s (seed) {}
        double next() noexcept   // uniform in [-1, 1)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            // NOTE: the plan's §3 formula (`(int64_t)(s>>11) * 1/2^52`, no offset)
            // actually yields [0, 2) -- s>>11 is a non-negative 53-bit quantity, so
            // the cast to int64_t never goes negative. Appending "- 1.0" is required
            // to match the documented/intended [-1, 1) range (all downstream
            // amplitude/peak-bound assumptions in the plan assume symmetric [-A, A)
            // signals). This is a test-utility bug fix, not a tolerance change.
            return (double) (int64_t) (s >> 11) * (1.0 / 4503599627370496.0) - 1.0;
        }
    };

    // Integer-period Goertzel magnitude (2/N normalised amplitude).
    double goertzelMag (const std::vector<float>& x, size_t start, size_t N, double f, double fs)
    {
        const double w     = 2.0 * kPi * f / fs;
        const double coeff = 2.0 * std::cos (w);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            s0 = (double) x[start + i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double real = s1 - s2 * std::cos (w);
        const double imag = s2 * std::sin (w);
        return (2.0 / (double) N) * std::sqrt (real * real + imag * imag);
    }

    // Positive-going zero-crossing counter with hysteresis +/- hyst (Schmitt
    // trigger: must dip below -hyst before a crossing above +hyst counts).
    int countPosCrossings (const std::vector<float>& x, size_t start, size_t n, double hyst)
    {
        int count = 0;
        int state = 0; // -1 below -hyst, +1 above +hyst, 0 neutral
        const size_t end = std::min (start + n, x.size());
        for (size_t i = start; i < end; ++i)
        {
            const double v = (double) x[i];
            if (v < -hyst) state = -1;
            else if (v > hyst)
            {
                if (state == -1) ++count;
                state = 1;
            }
        }
        return count;
    }

    // Burst onset detector: 5 ms RMS window, 1 ms hop, on 0.25*amp / off
    // 0.10*amp hysteresis. Returns the sample index (within x) of each onset.
    std::vector<size_t> detectOnsets (const std::vector<float>& x, double fs, double amp)
    {
        const size_t winLen = std::max<size_t> (1, (size_t) std::llround (0.005 * fs));
        const size_t hop    = std::max<size_t> (1, (size_t) std::llround (0.001 * fs));
        const double onT    = 0.25 * amp;
        const double offT   = 0.10 * amp;

        std::vector<size_t> onsets;
        bool above = false;
        if (x.size() < winLen) return onsets;
        for (size_t start = 0; start + winLen <= x.size(); start += hop)
        {
            double e = 0.0;
            for (size_t i = 0; i < winLen; ++i)
            {
                const double v = (double) x[start + i];
                e += v * v;
            }
            const double rms = std::sqrt (e / (double) winLen);
            if (! above && rms >= onT)
            {
                onsets.push_back (start);
                above = true;
            }
            else if (above && rms <= offT)
            {
                above = false;
            }
        }
        return onsets;
    }

    // Median of consecutive differences of a sorted-by-construction onset list.
    double medianConsecutiveDiff (const std::vector<size_t>& onsets)
    {
        if (onsets.size() < 2) return 0.0;
        std::vector<double> diffs;
        diffs.reserve (onsets.size() - 1);
        for (size_t i = 1; i < onsets.size(); ++i)
            diffs.push_back ((double) (onsets[i] - onsets[i - 1]));
        std::sort (diffs.begin(), diffs.end());
        const size_t n = diffs.size();
        return (n % 2 == 1) ? diffs[n / 2] : 0.5 * (diffs[n / 2 - 1] + diffs[n / 2]);
    }

    std::vector<double> toD (const std::vector<float>& v)
    {
        std::vector<double> out (v.size());
        for (size_t i = 0; i < v.size(); ++i) out[i] = (double) v[i];
        return out;
    }

    std::vector<double> toD2 (const std::vector<float>& a, const std::vector<float>& b)
    {
        std::vector<double> out;
        out.reserve (a.size() + b.size());
        for (float v : a) out.push_back ((double) v);
        for (float v : b) out.push_back ((double) v);
        return out;
    }

    static double sincf (double x) noexcept
    {
        if (std::abs (x) < 1.0e-9) return 1.0;
        const double px = kPi * x;
        return std::sin (px) / px;
    }

    // #6 analytic cross-check (non-gating, §3): direct DTFT evaluation of the
    // VariPolyphaseResampler index-stretch kernel's public design formula.
    // Used only as a design-time sanity computation; not asserted against.
    double designDownGainAt (double fHz, double fsIn, double fsOut)
    {
        const double s    = std::max (1.0, fsIn / fsOut);
        const int    D    = fc::VariPolyphaseResampler::kHalfTaps;
        const double beta = fc::VariPolyphaseResampler::kBeta;
        const double fc1  = fc::VariPolyphaseResampler::kFc1;
        const double i0b  = fc::besselI0 (beta);
        const int reach   = (int) std::ceil ((double) D * s) + 1;
        double worst = 0.0;
        for (int ph = 0; ph < 16; ++ph)
        {
            const double frac = (double) ph / 16.0;
            double re = 0.0, im = 0.0, den = 0.0;
            for (int i = -reach; i <= reach; ++i)
            {
                const double t = (double) i - frac;
                const double u = std::abs (t) / s;
                if (u > (double) D) continue;
                const double win = fc::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - (u / D) * (u / D)))) / i0b;
                const double w   = win * sincf (2.0 * fc1 * u);
                const double ang = 2.0 * kPi * (fHz / fsIn) * t;
                re += w * std::cos (ang);
                im -= w * std::sin (ang);
                den += w;
            }
            worst = std::max (worst, std::hypot (re, im) / den);
        }
        return worst;
    }

    // Engine execution helper chunk set (§3).
    const int kPrimeChunks[] = { 1, 2, 3, 5, 7, 13, 31, 61, 127, 251, 509 };
    const size_t kPrimeChunksLen = sizeof (kPrimeChunks) / sizeof (kPrimeChunks[0]);

    // Shared baseline setup for the §5.8/§5.9 bit-exact parameter-churn
    // schedule: setters BEFORE prepare (so reset() snaps deterministically).
    void prepareScheduleEngine (fc::Madoromi& engine, double fs)
    {
        engine.setClockHz (32000.0);
        engine.setWash01 (0.4);
        engine.setToneHz (6000.0);
        engine.setLengthSeconds (0.3);
        engine.setBalance01 (0.4);
        engine.setMix01 (0.6);
        engine.prepare (fs, 2);
    }

    // 3 s deterministic parameter-churn + prime-chunk audio schedule on an
    // ALREADY prepared/reset engine. injectNaN rotates NaN/+Inf/-Inf into the
    // 6 double setters at every chunk boundary in addition to the legitimate
    // churn (setFrozen is bool, excluded per house rule).
    void runSchedule (fc::Madoromi& engine, double fs, bool injectNaN,
                       std::vector<float>& outL, std::vector<float>& outR)
    {
        const long long total = (long long) std::llround (3.0 * fs);
        outL.assign ((size_t) total, 0.0f);
        outR.assign ((size_t) total, 0.0f);
        Lcg lcgL (0x1234ULL), lcgR (0x5678ULL);

        static const double clockVals[]   = { 8000.0, 48000.0, 16000.0, 32000.0 };
        static const double washVals[]    = { 0.0, 1.0, 0.5 };
        static const double toneVals[]    = { 1500.0, 18000.0, 6000.0 };
        static const double lengthVals[]  = { 0.05, 2.0, 0.4 };
        static const double balanceVals[] = { 0.0, 1.0, 0.3 };
        static const double mixVals[]     = { 0.0, 1.0, 0.7 };
        static const double badVals[]     = { std::numeric_limits<double>::quiet_NaN(),
                                               std::numeric_limits<double>::infinity(),
                                              -std::numeric_limits<double>::infinity() };

        int idx = 0, badIdx = 0;
        bool loopState = false;
        long long pos = 0;
        size_t chunkI = 0;
        while (pos < total)
        {
            engine.setClockHz       (clockVals[(size_t) idx % 4]);
            engine.setWash01        (washVals[(size_t) idx % 3]);
            engine.setToneHz        (toneVals[(size_t) idx % 3]);
            engine.setLengthSeconds (lengthVals[(size_t) idx % 3]);
            engine.setBalance01     (balanceVals[(size_t) idx % 3]);
            engine.setMix01         (mixVals[(size_t) idx % 3]);
            if (idx % 5 == 0) { loopState = ! loopState; engine.setFrozen (loopState); }
            ++idx;

            if (injectNaN)
            {
                engine.setClockHz       (badVals[(size_t) badIdx % 3]); ++badIdx;
                engine.setWash01        (badVals[(size_t) badIdx % 3]); ++badIdx;
                engine.setToneHz        (badVals[(size_t) badIdx % 3]); ++badIdx;
                engine.setLengthSeconds (badVals[(size_t) badIdx % 3]); ++badIdx;
                engine.setBalance01     (badVals[(size_t) badIdx % 3]); ++badIdx;
                engine.setMix01         (badVals[(size_t) badIdx % 3]); ++badIdx;
            }

            const int chunk = (int) std::min<long long> ((long long) kPrimeChunks[chunkI % kPrimeChunksLen], total - pos);
            ++chunkI;
            for (int i = 0; i < chunk; ++i)
            {
                outL[(size_t) (pos + i)] = (float) (0.6 * lcgL.next());
                outR[(size_t) (pos + i)] = (float) (0.6 * lcgR.next());
            }
            float* ptrs[2] = { outL.data() + pos, outR.data() + pos };
            engine.process (ptrs, 2, chunk);
            pos += chunk;
        }
    }

    // ---------------------------------------------------------- forward decls --
    // -- primitives (MicroLooper, §4) --
    void microLooperPeriodicityTest   (double fs);   // #1
    void microLooperWrapCrossfadeTest (double fs);   // #1 (burn reality)
    void microLooperDeterminismTest   (double fs);   // #2
    void microLooperLengthQuantizeTest(double fs);   // #3
    void microLooperTransparencyTest  (double fs);   // #4
    void microLooperGuardResetTest    (double fs);   // #10 primitive part

    // -- engine (Madoromi, §5) --
    void engineTransparencyLatencyTest(double fs);   // #5
    void engineAntiAliasTest          (double fs);   // #6
    void engineRepitchTest            (double fs);   // #7
    void engineWashDecayTest          (double fs);   // #8
    void engineWashStabilityTest      (double fs);   // regression class A
    void engineLongHoldChurnTest      (double fs);   // #9
    void engineNanRecoveryTest        (double fs);   // #10a
    void engineParamNanGuardTest      (double fs);   // #10b
    void engineResetAndDeterminismTest(double fs);   // #10c
    void engineChunkInvarianceTest    (double fs);   // determinism derivative
    void engineParamSmoothingTest     (double fs);   // regression class F

    void coreTests (double fs)
    {
        microLooperPeriodicityTest (fs);
        microLooperWrapCrossfadeTest (fs);
        microLooperDeterminismTest (fs);
        microLooperLengthQuantizeTest (fs);
        microLooperTransparencyTest (fs);
        microLooperGuardResetTest (fs);

        engineTransparencyLatencyTest (fs);
        engineAntiAliasTest (fs);
        engineRepitchTest (fs);
        engineWashDecayTest (fs);
        engineWashStabilityTest (fs);
        engineLongHoldChurnTest (fs);
        engineNanRecoveryTest (fs);
        engineParamNanGuardTest (fs);
        engineResetAndDeterminismTest (fs);
        engineChunkInvarianceTest (fs);
        engineParamSmoothingTest (fs);
    }

    // ---------------------------------------------------------------- §4 --

    // §4.1 one length variant: freeze, run periodicity + content-identity gates.
    void periodicityCase (double fs, double requestedLen, uint64_t seedL, uint64_t seedR)
    {
        const double expectedLen = std::clamp (requestedLen, fc::MicroLooper::kMinLoopSec, fc::MicroLooper::kMaxLoopSec);
        const long long P = (long long) std::llround (expectedLen * fs);
        const long long W = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs   * 1.0e-3 * fs));
        const long long F = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));

        const long long tf    = P + W + 1000;      // pre-freeze samples fed (== t_f)
        const long long post  = F + 8 + 4 * P;
        const long long total = tf + post;

        std::vector<float> xL ((size_t) total), xR ((size_t) total);
        Lcg lcgL (seedL), lcgR (seedR);
        for (long long i = 0; i < total; ++i)
        {
            xL[(size_t) i] = (float) (0.5 * lcgL.next());
            xR[(size_t) i] = (float) (0.5 * lcgR.next());
        }

        fc::MicroLooper looper;
        looper.prepare (fs, 2);
        looper.setLengthSeconds (requestedLen);

        std::vector<float> outL = xL, outR = xR;
        float* p1[2] = { outL.data(), outR.data() };
        looper.process (p1, 2, (int) tf);
        looper.setFrozen (true);
        float* p2[2] = { outL.data() + tf, outR.data() + tf };
        looper.process (p2, 2, (int) post);

        const std::string tag = " @fs=" + std::to_string (fs) + " len=" + std::to_string (requestedLen);

        if (looper.currentPeriodSamples() != (int) P)
            fail ("microLooperPeriodicityTest: currentPeriodSamples()=" + std::to_string (looper.currentPeriodSamples())
                  + " != expected P=" + std::to_string (P) + tag);

        const long long n0 = tf + F + 8;
        // Periodicity y[n] == y[n-P] only holds once BOTH n and n-P are past the
        // freeze-fade engage point (n0); comparing against n0..n0+P-1 would read
        // n-P values from before freeze (live pass-through), which is not
        // periodic. Compare over the range [n0+P, n0+4P) so n-P >= n0 throughout.
        bool periodOk = true, contentOk = true;
        for (long long n = n0 + P; n < n0 + 4 * P && periodOk; ++n)
        {
            if (outL[(size_t) n] != outL[(size_t) (n - P)]) { periodOk = false; fail ("microLooperPeriodicityTest: L periodicity broke at n=" + std::to_string (n) + tag); }
            else if (outR[(size_t) n] != outR[(size_t) (n - P)]) { periodOk = false; fail ("microLooperPeriodicityTest: R periodicity broke at n=" + std::to_string (n) + tag); }
        }
        for (long long n = n0; n < n0 + 3 * P && contentOk; ++n)
        {
            const long long j = (n - tf) % P;
            if (j < P - W)
            {
                const float expL = xL[(size_t) (tf - P + j)];
                const float expR = xR[(size_t) (tf - P + j)];
                if (outL[(size_t) n] != expL || outR[(size_t) n] != expR)
                { contentOk = false; fail ("microLooperPeriodicityTest: content identity (table read) broke at n=" + std::to_string (n) + tag); }
            }
            else
            {
                const long long k = j - (P - W);
                const double a = 0.5 * (1.0 - std::cos (kPi * (double) (k + 1) / (double) W));
                const double expLd = (1.0 - a) * (double) xL[(size_t) (tf - W + k)] + a * (double) xL[(size_t) (tf - P - W + k)];
                const double expRd = (1.0 - a) * (double) xR[(size_t) (tf - W + k)] + a * (double) xR[(size_t) (tf - P - W + k)];
                // The engine computes the burn blend in double and casts to float
                // only on output; round our independent double recomputation to
                // float the same way before differencing, so 1e-12 measures only
                // genuine FMA/evaluation-order slop, not the unavoidable float cast.
                const double expL = (double) (float) expLd;
                const double expR = (double) (float) expRd;
                if (std::fabs ((double) outL[(size_t) n] - expL) > 1e-12 || std::fabs ((double) outR[(size_t) n] - expR) > 1e-12)
                { contentOk = false; fail ("microLooperPeriodicityTest: content identity (burn tail) broke at n=" + std::to_string (n) + tag); }
            }
        }

        if (! fct::allFinite (toD2 (outL, outR)))
            fail ("microLooperPeriodicityTest: non-finite output" + tag);
        const double peak = fct::peakAbs (toD2 (outL, outR));
        if (peak > 1.5 * 0.5)
            fail ("microLooperPeriodicityTest: peak " + std::to_string (peak) + " > 0.75" + tag);

        if (std::memcmp (outL.data(), outR.data(), outL.size() * sizeof (float)) == 0)
            fail ("microLooperPeriodicityTest: L/R channels identical (crosstalk suspected)" + tag);
    }

    void microLooperPeriodicityTest (double fs)
    {
        periodicityCase (fs, 0.313, 0xA1A1ULL, 0xB2B2ULL);
        periodicityCase (fs, 2.0,   0xA3A3ULL, 0xB4B4ULL);
        periodicityCase (fs, 5.0,   0xA5A5ULL, 0xB6B6ULL);   // clamp -> 2.0
        periodicityCase (fs, 0.001, 0xA7A7ULL, 0xB8B8ULL);   // clamp -> 0.05
    }

    // §4.2: wrap-burn continuity (click detection at successive wrap points).
    void microLooperWrapCrossfadeTest (double fs)
    {
        const double len = 0.313;
        const double A   = 0.8;
        const double f0  = 16.5 / len;
        const long long P = (long long) std::llround (len * fs);
        const long long W = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs * 1.0e-3 * fs));

        const double periodSamples = fs / f0;
        long long tf = 0;
        for (int j = 0; ; ++j)
        {
            tf = (long long) std::llround (periodSamples * (0.25 + (double) j));
            if (tf >= P + W + 64) break;
        }

        const long long total = tf + 3 * P + W + 64;
        std::vector<float> xL ((size_t) total);
        for (long long i = 0; i < total; ++i)
            xL[(size_t) i] = (float) (A * std::sin (2.0 * kPi * f0 * (double) i / fs));
        std::vector<float> xR = xL;

        fc::MicroLooper looper;
        looper.prepare (fs, 2);
        looper.setLengthSeconds (len);

        float* p1[2] = { xL.data(), xR.data() };
        looper.process (p1, 2, (int) tf);
        looper.setFrozen (true);
        float* p2[2] = { xL.data() + tf, xR.data() + tf };
        looper.process (p2, 2, (int) (total - tf));

        const std::string tag = " @fs=" + std::to_string (fs);
        const double gate = 0.05 * A;

        for (int k = 1; k <= 3; ++k)
        {
            const long long wrapN = tf + (long long) k * P;
            const long long lo = std::max<long long> (1, wrapN - W - 8);
            const long long hi = std::min<long long> (total - 1, wrapN + W + 8);
            for (long long n = lo; n <= hi; ++n)
            {
                const double d = std::fabs ((double) xL[(size_t) n] - (double) xL[(size_t) (n - 1)]);
                if (d > gate)
                {
                    fail ("microLooperWrapCrossfadeTest: wrap click |dy|=" + std::to_string (d)
                          + " > " + std::to_string (gate) + " near wrap k=" + std::to_string (k) + tag);
                    break;
                }
            }
        }
    }

    // §4.3: determinism (two fresh runs, identical schedule, bit-exact).
    void microLooperDeterminismTest (double fs)
    {
        const double len = 0.313;
        const long long P = (long long) std::llround (len * fs);
        const long long W = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs   * 1.0e-3 * fs));
        const long long F = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));

        const long long t1 = P + W + 500;
        const long long t2 = t1 + 2 * P + F + 200;
        const long long t3 = t2 + F + 300;
        const long long total = t3 + 2 * P + F + 200;

        Lcg seedGenL (0xD00D1ULL), seedGenR (0xD00D2ULL);
        std::vector<float> srcL ((size_t) total), srcR ((size_t) total);
        for (long long i = 0; i < total; ++i)
        {
            srcL[(size_t) i] = (float) (0.5 * seedGenL.next());
            srcR[(size_t) i] = (float) (0.5 * seedGenR.next());
        }

        auto runOnce = [&] (std::vector<float>& outL, std::vector<float>& outR)
        {
            outL = srcL; outR = srcR;
            fc::MicroLooper looper;
            looper.prepare (fs, 2);
            long long pos = 0;
            auto step = [&] (long long upTo, bool freeze)
            {
                float* p[2] = { outL.data() + pos, outR.data() + pos };
                looper.process (p, 2, (int) (upTo - pos));
                pos = upTo;
                looper.setFrozen (freeze);
            };
            step (t1, true);
            step (t2, false);
            step (t3, true);
            float* p[2] = { outL.data() + pos, outR.data() + pos };
            looper.process (p, 2, (int) (total - pos));
        };

        std::vector<float> aL, aR, bL, bR;
        runOnce (aL, aR);
        runOnce (bL, bR);

        if (std::memcmp (aL.data(), bL.data(), aL.size() * sizeof (float)) != 0
         || std::memcmp (aR.data(), bR.data(), aR.size() * sizeof (float)) != 0)
            fail ("microLooperDeterminismTest: two runs from reset() not bit-identical @fs=" + std::to_string (fs));
    }
    // §4.4 variant (a): LCG content, checks wrap-quantised reflection timing,
    // new-period bit-exact periodicity and freeze-time-snapshot provenance,
    // across two consecutive length changes (len1->len2->len1, abbreviated 2nd).
    void quantizeVariantA (double fs)
    {
        const double len1 = 0.313, len2 = 0.1517;
        const long long P1 = (long long) std::llround (len1 * fs);
        const long long P2 = (long long) std::llround (len2 * fs);
        const long long W  = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs   * 1.0e-3 * fs));
        const long long F  = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));

        const long long tf       = P1 + W + 1000;
        const long long tChange  = tf + 2 * P1 + P1 / 2;
        const long long tWrap    = tf + 3 * P1;
        const long long tChange2 = tWrap + 2 * P2 + P2 / 2;
        const long long tWrap2   = tWrap + 3 * P2;
        const long long total    = tWrap2 + 3 * P1 + W + 64;

        std::vector<float> xL ((size_t) total), xR ((size_t) total);
        Lcg lcgL (0x51ULL), lcgR (0x52ULL);
        for (long long i = 0; i < total; ++i)
        {
            xL[(size_t) i] = (float) (0.5 * lcgL.next());
            xR[(size_t) i] = (float) (0.5 * lcgR.next());
        }

        fc::MicroLooper looper;
        looper.prepare (fs, 2);
        looper.setLengthSeconds (len1);

        std::vector<float> outL = xL, outR = xR;
        auto proc = [&] (long long from, long long to)
        {
            float* p[2] = { outL.data() + from, outR.data() + from };
            looper.process (p, 2, (int) (to - from));
        };

        proc (0, tf);
        looper.setFrozen (true);
        proc (tf, tChange);
        looper.setLengthSeconds (len2);
        proc (tChange, tChange2);
        looper.setLengthSeconds (len1);
        proc (tChange2, total);

        const std::string tag = " @fs=" + std::to_string (fs);

        if (! fct::allFinite (toD2 (outL, outR)))
            fail ("microLooperLengthQuantizeTest(a): non-finite output" + tag);

        // 1) no premature reflection before the first wrap. Start at tf+F+P1 (not
        // tf+P1): comparing against y[tf..tf+F) would reference the freeze-engage
        // crossfade samples, which are not pure table reads (same class of
        // off-by-one as §4.1's periodicity window).
        for (long long n = tf + F + P1; n < tWrap; ++n)
            if (outL[(size_t) n] != outL[(size_t) (n - P1)] || outR[(size_t) n] != outR[(size_t) (n - P1)])
            { fail ("microLooperLengthQuantizeTest(a): premature P1 reflection at n=" + std::to_string (n) + tag); break; }

        // 2) reflection happens within [tWrap, tWrap+W].
        {
            bool found = false;
            for (long long n = tWrap; n <= tWrap + W; ++n)
                if (outL[(size_t) n] != outL[(size_t) (n - P1)] || outR[(size_t) n] != outR[(size_t) (n - P1)]) { found = true; break; }
            if (! found) fail ("microLooperLengthQuantizeTest(a): no reflection found within [tWrap, tWrap+W]" + tag);
        }

        // 3) new P2 periodicity from pass 2 onward: compare pass 3 against pass 2
        // (NOT pass 2 against pass 1 -- pass 1's first W samples are the
        // transient head-splice, not a pure table read).
        for (long long n = tWrap + 2 * P2; n < tWrap + 3 * P2; ++n)
            if (outL[(size_t) n] != outL[(size_t) (n - P2)] || outR[(size_t) n] != outR[(size_t) (n - P2)])
            { fail ("microLooperLengthQuantizeTest(a): P2 periodicity broke at n=" + std::to_string (n) + tag); break; }

        // 4) pass-2 content provenance: sourced from the freeze-time snapshot (tf), not live ring.
        for (long long j = 0; j < P2 - W; ++j)
        {
            const long long n = tWrap + P2 + j;
            const long long src = tf - P2 + j;
            if (outL[(size_t) n] != xL[(size_t) src] || outR[(size_t) n] != xR[(size_t) src])
            { fail ("microLooperLengthQuantizeTest(a): pass-2 content provenance broke at j=" + std::to_string (j) + tag); break; }
        }

        // 5) second cycle (abbreviated): reflection within [tWrap2, tWrap2+W] vs P2.
        {
            bool found = false;
            for (long long n = tWrap2; n <= tWrap2 + W; ++n)
                if (outL[(size_t) n] != outL[(size_t) (n - P2)] || outR[(size_t) n] != outR[(size_t) (n - P2)]) { found = true; break; }
            if (! found) fail ("microLooperLengthQuantizeTest(a): no 2nd reflection found within [tWrap2, tWrap2+W]" + tag);
        }

        // 6) new P1 periodicity re-established (abbreviated: 1 period span,
        // pass 3 vs pass 2 for the same head-splice reason as (3)).
        for (long long n = tWrap2 + 2 * P1; n < tWrap2 + 3 * P1; ++n)
            if (outL[(size_t) n] != outL[(size_t) (n - P1)] || outR[(size_t) n] != outR[(size_t) (n - P1)])
            { fail ("microLooperLengthQuantizeTest(a): P1 periodicity (2nd cycle) broke at n=" + std::to_string (n) + tag); break; }
    }

    // §4.4 variant (b): sine input, wrap/head-splice continuity across both
    // length changes (reuses the §4.2 click gate: max|dy| <= 0.05*A).
    void quantizeVariantB (double fs)
    {
        const double len1 = 0.313, len2 = 0.1517;
        const double A = 0.8;
        const double f0 = 16.5 / len1;
        const long long P1 = (long long) std::llround (len1 * fs);
        const long long P2 = (long long) std::llround (len2 * fs);
        const long long W  = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs   * 1.0e-3 * fs));
        const long long F  = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));

        const long long tf       = P1 + W + 1000;
        const long long tChange  = tf + 2 * P1 + P1 / 2;
        const long long tWrap    = tf + 3 * P1;
        const long long tChange2 = tWrap + 2 * P2 + P2 / 2;
        const long long tWrap2   = tWrap + 3 * P2;
        const long long total    = tWrap2 + 2 * P1 + W + 64;

        std::vector<float> xL ((size_t) total);
        for (long long i = 0; i < total; ++i)
            xL[(size_t) i] = (float) (A * std::sin (2.0 * kPi * f0 * (double) i / fs));
        std::vector<float> xR = xL;

        fc::MicroLooper looper;
        looper.prepare (fs, 2);
        looper.setLengthSeconds (len1);

        auto proc = [&] (long long from, long long to)
        {
            float* p[2] = { xL.data() + from, xR.data() + from };
            looper.process (p, 2, (int) (to - from));
        };
        proc (0, tf);
        looper.setFrozen (true);
        proc (tf, tChange);
        looper.setLengthSeconds (len2);
        proc (tChange, tChange2);
        looper.setLengthSeconds (len1);
        proc (tChange2, total);

        const std::string tag = " @fs=" + std::to_string (fs);
        const double gate = 0.05 * A;
        double maxD = 0.0;
        for (long long n = tf + F + 1; n < total; ++n)
            maxD = std::max (maxD, std::fabs ((double) xL[(size_t) n] - (double) xL[(size_t) (n - 1)]));
        if (maxD > gate)
            fail ("microLooperLengthQuantizeTest(b): max|dy|=" + std::to_string (maxD) + " > " + std::to_string (gate) + tag);
    }

    void microLooperLengthQuantizeTest (double fs)
    {
        quantizeVariantA (fs);
        quantizeVariantB (fs);
    }
    // §4.5: transparency (unfrozen pass-through, and restored after unfreeze).
    void microLooperTransparencyTest (double fs)
    {
        const double A = 0.5;
        const long long F = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));
        const long long seg = (long long) std::llround (1.0 * fs);
        const long long total = 3 * seg;   // [0,seg) unfrozen, [seg,2seg) frozen, [2seg,3seg) unfrozen restored

        std::vector<float> xL ((size_t) total), xR ((size_t) total);
        Lcg lcgL (0x71ULL), lcgR (0x72ULL);
        for (long long i = 0; i < total; ++i)
        {
            xL[(size_t) i] = (float) (A * lcgL.next());
            xR[(size_t) i] = (float) (A * lcgR.next());
        }

        fc::MicroLooper looper;
        looper.prepare (fs, 2);
        looper.setLengthSeconds (0.4);

        std::vector<float> outL = xL, outR = xR;
        auto proc = [&] (long long from, long long to)
        {
            float* p[2] = { outL.data() + from, outR.data() + from };
            looper.process (p, 2, (int) (to - from));
        };

        proc (0, seg);
        looper.setFrozen (true);           // t_f = seg
        proc (seg, 2 * seg);
        looper.setFrozen (false);          // t_unfrz = 2*seg
        proc (2 * seg, 3 * seg);

        const std::string tag = " @fs=" + std::to_string (fs);
        const long long tUnfrz = 2 * seg;

        for (long long n = 0; n < seg; ++n)
            if (outL[(size_t) n] != xL[(size_t) n] || outR[(size_t) n] != xR[(size_t) n])
            { fail ("microLooperTransparencyTest: pre-freeze transparency broke at n=" + std::to_string (n) + tag); break; }

        for (long long n = tUnfrz + F + 8; n < total; ++n)
            if (outL[(size_t) n] != xL[(size_t) n] || outR[(size_t) n] != xR[(size_t) n])
            { fail ("microLooperTransparencyTest: post-unfreeze transparency broke at n=" + std::to_string (n) + tag); break; }

        auto checkFadeWindow = [&] (long long from, long long to, const char* label)
        {
            for (long long n = from; n < to && n < total; ++n)
            {
                if (! std::isfinite ((double) outL[(size_t) n]) || ! std::isfinite ((double) outR[(size_t) n]))
                { fail (std::string ("microLooperTransparencyTest: non-finite in ") + label + tag); break; }
                const double p = std::max (std::fabs ((double) outL[(size_t) n]), std::fabs ((double) outR[(size_t) n]));
                if (p > 1.5 * A)
                { fail (std::string ("microLooperTransparencyTest: peak > 1.5A in ") + label + tag); break; }
            }
        };
        checkFadeWindow (seg, seg + F, "freeze-in xfade");
        checkFadeWindow (tUnfrz, tUnfrz + F, "unfreeze-out xfade");
    }

    // §4.6: input finite guard, param-NaN guard (bit-exact A/B), reset residual.
    void microLooperGuardResetTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        // 1) input finite guard.
        {
            const double len = 0.313;
            const long long P = (long long) std::llround (len * fs);
            const long long W = std::max<long long> (1, std::llround (fc::MicroLooper::kWrapXfadeMs   * 1.0e-3 * fs));
            const long long F = std::max<long long> (1, std::llround (fc::MicroLooper::kFreezeXfadeMs * 1.0e-3 * fs));
            const long long tf = P + W + 2000;
            const long long post = F + 8 + 2 * P;
            const long long total = tf + post;

            std::vector<float> xClean ((size_t) total), xInj ((size_t) total);
            Lcg lcg (0x91ULL);
            for (long long i = 0; i < total; ++i)
            {
                const float v = (float) (0.5 * lcg.next());
                xClean[(size_t) i] = v;
                xInj[(size_t) i] = v;
            }
            const long long badIdx[3] = { 500, 1000, 1500 };
            xInj[(size_t) badIdx[0]] = std::numeric_limits<float>::quiet_NaN();
            xInj[(size_t) badIdx[1]] = std::numeric_limits<float>::infinity();
            xInj[(size_t) badIdx[2]] = -std::numeric_limits<float>::infinity();

            fc::MicroLooper looper;
            looper.prepare (fs, 2);
            looper.setLengthSeconds (len);

            std::vector<float> outL = xInj, outR = xInj;
            float* p1[2] = { outL.data(), outR.data() };
            looper.process (p1, 2, (int) tf);
            looper.setFrozen (true);
            float* p2[2] = { outL.data() + tf, outR.data() + tf };
            looper.process (p2, 2, (int) post);

            for (long long i = 0; i < tf; ++i)
            {
                const bool isBad = (i == badIdx[0] || i == badIdx[1] || i == badIdx[2]);
                const float expect = isBad ? 0.0f : xClean[(size_t) i];
                if (outL[(size_t) i] != expect || outR[(size_t) i] != expect)
                { fail ("microLooperGuardResetTest: input finite guard broke at i=" + std::to_string (i) + tag); break; }
            }

            if (! fct::allFinite (toD2 (outL, outR)))
                fail ("microLooperGuardResetTest: non-finite in engaged region after injected-sample freeze" + tag);
        }

        // 2) param-NaN guard, bit-exact A/B (setFrozen excluded: bool).
        {
            auto runSched = [&] (bool injectNaN, std::vector<float>& outL, std::vector<float>& outR)
            {
                const long long total = (long long) std::llround (2.0 * fs);
                outL.assign ((size_t) total, 0.0f);
                outR.assign ((size_t) total, 0.0f);
                Lcg lcgL (0xA1ULL), lcgR (0xA2ULL);

                fc::MicroLooper looper;
                looper.prepare (fs, 2);
                looper.setLengthSeconds (0.2);

                static const double lens[] = { 0.2, 0.5, 0.35 };
                static const double bad[]  = { std::numeric_limits<double>::quiet_NaN(),
                                               std::numeric_limits<double>::infinity(),
                                              -std::numeric_limits<double>::infinity() };
                int idx = 0, badIdx2 = 0;
                bool frozen = false;
                long long pos = 0;
                size_t chunkI = 0;
                while (pos < total)
                {
                    looper.setLengthSeconds (lens[(size_t) idx % 3]);
                    if (idx % 4 == 0) { frozen = ! frozen; looper.setFrozen (frozen); }
                    ++idx;
                    if (injectNaN)
                    {
                        looper.setLengthSeconds (bad[(size_t) badIdx2 % 3]); ++badIdx2;
                    }
                    const int chunk = (int) std::min<long long> ((long long) kPrimeChunks[chunkI % kPrimeChunksLen], total - pos);
                    ++chunkI;
                    for (int i = 0; i < chunk; ++i)
                    {
                        outL[(size_t) (pos + i)] = (float) (0.5 * lcgL.next());
                        outR[(size_t) (pos + i)] = (float) (0.5 * lcgR.next());
                    }
                    float* p[2] = { outL.data() + pos, outR.data() + pos };
                    looper.process (p, 2, chunk);
                    pos += chunk;
                }
            };

            std::vector<float> aL, aR, bL, bR;
            runSched (false, aL, aR);
            runSched (true,  bL, bR);
            if (std::memcmp (aL.data(), bL.data(), aL.size() * sizeof (float)) != 0
             || std::memcmp (aR.data(), bR.data(), aR.size() * sizeof (float)) != 0)
                fail ("microLooperGuardResetTest: param-NaN guard A/B not bit-identical" + tag);
        }

        // 3) reset residual (expect exact silence) + immediate transparency recovery;
        // the length parameter itself survives reset() (house rule).
        {
            const double len = 0.6;
            fc::MicroLooper looper;
            looper.prepare (fs, 2);
            looper.setLengthSeconds (len);

            const long long warm = (long long) std::llround (0.8 * fs);
            std::vector<float> wL (warm), wR (warm);
            Lcg lcgL (0xB1ULL), lcgR (0xB2ULL);
            for (long long i = 0; i < warm; ++i) { wL[(size_t) i] = (float) (0.5 * lcgL.next()); wR[(size_t) i] = (float) (0.5 * lcgR.next()); }
            float* pw[2] = { wL.data(), wR.data() };
            looper.process (pw, 2, (int) warm);
            looper.setFrozen (true);
            std::vector<float> fL (warm), fR (warm);
            for (long long i = 0; i < warm; ++i) { fL[(size_t) i] = (float) (0.5 * lcgL.next()); fR[(size_t) i] = (float) (0.5 * lcgR.next()); }
            float* pf[2] = { fL.data(), fR.data() };
            looper.process (pf, 2, (int) warm);

            looper.reset();

            const int expectedP = (int) std::llround (len * fs);
            if (looper.currentPeriodSamples() != expectedP)
                fail ("microLooperGuardResetTest: currentPeriodSamples() after reset() != expected P (length should survive reset)" + tag);

            const long long silence = (long long) std::llround (0.5 * fs);
            std::vector<float> sL ((size_t) silence, 0.0f), sR ((size_t) silence, 0.0f);
            float* ps[2] = { sL.data(), sR.data() };
            looper.process (ps, 2, (int) silence);
            double peak = 0.0;
            for (float v : sL) peak = std::max (peak, (double) std::fabs (v));
            for (float v : sR) peak = std::max (peak, (double) std::fabs (v));
            if (peak > 1e-12)
                fail ("microLooperGuardResetTest: reset residue " + std::to_string (peak) + " > 1e-12" + tag);

            const long long seg2 = (long long) std::llround (0.3 * fs);
            std::vector<float> tL (seg2), tR (seg2);
            Lcg lcgL2 (0xC1ULL), lcgR2 (0xC2ULL);
            for (long long i = 0; i < seg2; ++i) { tL[(size_t) i] = (float) (0.5 * lcgL2.next()); tR[(size_t) i] = (float) (0.5 * lcgR2.next()); }
            std::vector<float> outL = tL, outR = tR;
            float* pt[2] = { outL.data(), outR.data() };
            looper.process (pt, 2, (int) seg2);
            for (long long i = 0; i < seg2; ++i)
                if (outL[(size_t) i] != tL[(size_t) i] || outR[(size_t) i] != tR[(size_t) i])
                { fail ("microLooperGuardResetTest: transparency not immediately restored after reset()" + tag); break; }
        }
    }

    // ---------------------------------------------------------------- §5 --
    // §5.1 support: design-time latency table + transcribed formula (independent
    // hand-copy of the header's published formula, literal 62/16 -- NOT read
    // from VariPolyphaseResampler::kHalfTaps / Madoromi::kFifoMargin).
    //
    // D2 fix (approved): the reported latency now depends on the PREPARED
    // clock C (previously it silently assumed C == kInternalRateHz == 48000
    // regardless of the actual clock, which was the bug), so this table/
    // formula is now parameterised on C. Values are hand-computed for the two
    // clocks the suite exercises (48000 == kInternalRateHz, and 8000 ==
    // kClockMinHz, the opposite end of the range), giving a genuine per-
    // rate/2-clock cross-check rather than a single clock-invariant number.
    struct RateLatency { double fs; long long L48; long long L8; };
    const RateLatency kLatencyTable[] = {
        { 44100.0,   484,  764 }, { 48000.0,   514,  824 }, { 88200.0,   878, 1448 },
        { 96000.0,   948, 1568 }, { 176400.0, 1676, 2815 }, { 192000.0, 1816, 3056 }
    };
    // fifoFill(fsHost): the FIFO's own clock-sweep-safety budget, UNCHANGED by
    // the D2 fix (identical formula/value to the pre-fix `latency + 64`).
    long long transcribedFifoFill (double fsHost)
    {
        const double P0 = 16.0 + std::ceil (62.0 * (fsHost / 8000.0 - std::max (1.0, fsHost / 48000.0)));
        return (long long) std::llround (62.0 * std::max (1.0, fsHost / 48000.0)) + (long long) P0 + 64;
    }
    // L(fsHost, C) = fifoFill(fsHost) + G(fsHost, C), G = round(2*31*max(1,fsHost/C)).
    long long transcribedLatency (double fsHost, double clockHzPrepared)
    {
        const long long G = (long long) std::llround (62.0 * std::max (1.0, fsHost / clockHzPrepared));
        return transcribedFifoFill (fsHost) + G;
    }
    long long absDiff (long long a, long long b) { return a >= b ? a - b : b - a; }

    void engineTransparencyLatencyTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        // (0) contract pin, clock=48000 (== kInternalRateHz).
        long long expectedL48 = -1, expectedL8 = -1;
        for (const auto& e : kLatencyTable) if (std::fabs (e.fs - fs) < 0.5) { expectedL48 = e.L48; expectedL8 = e.L8; break; }
        if (expectedL48 < 0) { fail ("engineTransparencyLatencyTest: fs not in design-time table" + tag); return; }

        const long long Lx48    = transcribedLatency (fs, 48000.0);
        const long long Lpure48 = fc::Madoromi::latencyForRate (fs, 48000.0);
        {
            fc::Madoromi pin;
            pin.setWash01 (0.0); pin.setToneHz (6000.0); pin.setFrozen (false);
            pin.setBalance01 (0.0); pin.setClockHz (48000.0); pin.setMix01 (0.0);
            pin.prepare (fs, 2);
            const long long engineL = pin.latencySamples();
            if (engineL != Lx48 || engineL != Lpure48 || engineL != expectedL48)
                fail ("engineTransparencyLatencyTest: latency contract pin (clock=48000) mismatch engine=" + std::to_string (engineL)
                      + " transcribed=" + std::to_string (Lx48) + " pure=" + std::to_string (Lpure48)
                      + " table=" + std::to_string (expectedL48) + tag);
        }
        if (fc::Madoromi::kFifoSafetyPad != 64)
            fail ("engineTransparencyLatencyTest: kFifoSafetyPad != 64" + tag);
        if (fc::Madoromi::kInternalRateHz != 48000.0)
            fail ("engineTransparencyLatencyTest: kInternalRateHz != 48000" + tag);
        if (std::fabs (fc::Madoromi::washDecaySeconds (1.0) - 8.0) > 1e-12)
            fail ("engineTransparencyLatencyTest: washDecaySeconds(1.0) != 8.0" + tag);
        if (std::fabs (fc::Madoromi::washDecaySeconds (0.0) - 0.4) > 1e-12)
            fail ("engineTransparencyLatencyTest: washDecaySeconds(0.0) != 0.4" + tag);
        if (fc::Madoromi::toneToDamping (18000.0) != 0.0 || fc::Madoromi::toneToDamping (1500.0) != 1.0)
            fail ("engineTransparencyLatencyTest: toneToDamping endpoint mismatch" + tag);

        // D2 fix (approved): under the corrected contract, L is NO LONGER
        // clock-independent -- it must equal the TRUE wet transit at
        // WHATEVER clock is prepared. Second contract pin at clock=8000
        // (kClockMinHz), the opposite end of the range, replaces the removed
        // "L is clock-independent" assertion with the opposite (and now
        // correct) one: a DIFFERENT clock produces a DIFFERENT (larger) L,
        // cross-checked the same triple way (engine / transcribed / pure).
        const long long Lx8    = transcribedLatency (fs, 8000.0);
        const long long Lpure8 = fc::Madoromi::latencyForRate (fs, 8000.0);
        {
            fc::Madoromi pin8;
            pin8.setWash01 (0.0); pin8.setToneHz (6000.0); pin8.setFrozen (false);
            pin8.setBalance01 (0.0); pin8.setClockHz (8000.0); pin8.setMix01 (0.0);
            pin8.prepare (fs, 2);
            const long long engineL8 = pin8.latencySamples();
            if (engineL8 != Lx8 || engineL8 != Lpure8 || engineL8 != expectedL8)
                fail ("engineTransparencyLatencyTest: latency contract pin (clock=8000) mismatch engine=" + std::to_string (engineL8)
                      + " transcribed=" + std::to_string (Lx8) + " pure=" + std::to_string (Lpure8)
                      + " table=" + std::to_string (expectedL8) + tag);
            if (engineL8 == expectedL48)
                fail ("engineTransparencyLatencyTest: clock=8000 latency equals clock=48000 latency (clock-independence bug not fixed)" + tag);
        }

        const long long L   = expectedL48;   // reference latency at clock=48000, used below
        const int       PAD = fc::Madoromi::kFifoSafetyPad;

        // Dry-path delay identity: the dry ring is delayed by exactly
        // engine.latencySamples() for WHATEVER clock that specific engine was
        // prepared with (D2 fix: this is now clock-dependent, so it must be
        // fetched per-instance rather than reusing the outer clock=48000 `L`).
        auto dryIdentityCase = [&] (double clockHz, double durSec, uint64_t seedL, uint64_t seedR, const char* label)
        {
            fc::Madoromi engine;
            engine.setWash01 (0.0); engine.setToneHz (6000.0); engine.setFrozen (false);
            engine.setBalance01 (0.0); engine.setClockHz (clockHz); engine.setMix01 (0.0);
            engine.prepare (fs, 2);
            const long long Lthis = engine.latencySamples();

            const long long total = (long long) std::llround (durSec * fs);
            std::vector<float> inL ((size_t) total), inR ((size_t) total);
            Lcg lcgL (seedL), lcgR (seedR);
            for (long long i = 0; i < total; ++i)
            {
                inL[(size_t) i] = (float) (0.6 * lcgL.next());
                inR[(size_t) i] = (float) (0.6 * lcgR.next());
            }
            std::vector<float> outL = inL, outR = inR;
            float* p[2] = { outL.data(), outR.data() };
            engine.process (p, 2, (int) total);

            double worst = 0.0;
            for (long long n = 0; n < total; ++n)
            {
                const double expL = (n >= Lthis) ? (double) inL[(size_t) (n - Lthis)] : 0.0;
                const double expR = (n >= Lthis) ? (double) inR[(size_t) (n - Lthis)] : 0.0;
                worst = std::max (worst, std::fabs ((double) outL[(size_t) n] - expL));
                worst = std::max (worst, std::fabs ((double) outR[(size_t) n] - expR));
            }
            if (worst > 1e-6)
                fail (std::string ("engineTransparencyLatencyTest(") + label + "): dry delay identity worst diff="
                      + std::to_string (worst) + tag);
        };

        // (a)1: mix=0 -> exact L-sample dry delay, stereo, independent L/R seeds.
        dryIdentityCase (48000.0, 2.0, 0x111ULL, 0x222ULL, "a1");

        // (a)2: mono path -> same identity (mono duplication + wet average code path).
        {
            fc::Madoromi engine;
            engine.setWash01 (0.0); engine.setToneHz (6000.0); engine.setFrozen (false);
            engine.setBalance01 (0.0); engine.setClockHz (48000.0); engine.setMix01 (0.0);
            engine.prepare (fs, 2);
            const long long Lthis = engine.latencySamples();

            const long long total = (long long) std::llround (0.5 * fs);
            std::vector<float> in ((size_t) total);
            Lcg lcg (0x333ULL);
            for (long long i = 0; i < total; ++i) in[(size_t) i] = (float) (0.6 * lcg.next());
            std::vector<float> out = in;
            float* p[1] = { out.data() };
            engine.process (p, 1, (int) total);

            double worst = 0.0;
            for (long long n = 0; n < total; ++n)
            {
                const double exp = (n >= Lthis) ? (double) in[(size_t) (n - Lthis)] : 0.0;
                worst = std::max (worst, std::fabs ((double) out[(size_t) n] - exp));
            }
            if (worst > 1e-6)
                fail ("engineTransparencyLatencyTest(a2): mono dry delay identity worst diff=" + std::to_string (worst) + tag);
        }

        // (b) mix=1: passband level + wet transit alignment.
        const long long N = (long long) std::llround (fs * 0.01) * 100;   // 1.0 s, integer-periodic
        auto measureGainDb = [&] (double freq, double clockHz) -> double
        {
            fc::Madoromi eng;
            eng.setWash01 (0.0); eng.setToneHz (6000.0); eng.setFrozen (false);
            eng.setBalance01 (0.0); eng.setClockHz (clockHz); eng.setMix01 (1.0);
            eng.prepare (fs, 2);
            const long long skip = eng.latencySamples() + PAD + (long long) std::llround (0.1 * fs);
            const long long total = skip + N;
            const double A = 0.5;
            std::vector<float> Lb ((size_t) total), Rb ((size_t) total);
            for (long long i = 0; i < total; ++i)
            {
                const float v = (float) (A * std::sin (2.0 * kPi * freq * (double) i / fs));
                Lb[(size_t) i] = v; Rb[(size_t) i] = v;
            }
            float* p[2] = { Lb.data(), Rb.data() };
            eng.process (p, 2, (int) total);
            const double mag = goertzelMag (Lb, (size_t) skip, (size_t) N, freq, fs);
            return 20.0 * std::log10 (mag / A);
        };

        const double gMidDb = measureGainDb (1000.0, 48000.0);
        if (std::fabs (gMidDb) > 0.10)
            fail ("engineTransparencyLatencyTest(b1): midband gain " + std::to_string (gMidDb) + " dB, |.|>0.10" + tag);

        const double edgeRaw = 0.35 * std::min (fs, 48000.0);
        const double edge    = std::floor (edgeRaw / 100.0) * 100.0;
        const double gEdgeDb = measureGainDb (edge, 48000.0);
        if (std::fabs (gEdgeDb) > 0.75)
            fail ("engineTransparencyLatencyTest(b2): band-edge (" + std::to_string (edge) + " Hz) gain "
                  + std::to_string (gEdgeDb) + " dB, |.|>0.75" + tag);

        // Transit measurement via impulse-response peak (a linear system's group
        // delay is exact and unambiguous this way -- impulse->peak is a stronger
        // independent oracle than a windowed noise cross-correlation here, and
        // is tractable: the combined down+up Kaiser kernel reach scales with
        // fs/clockHz, so the search window is sized from that, not a fixed +/-64).
        auto measureTransitLag = [&] (double clockHz) -> long long
        {
            fc::Madoromi eng;
            eng.setWash01 (0.0); eng.setToneHz (6000.0); eng.setFrozen (false);
            eng.setBalance01 (0.0); eng.setClockHz (clockHz); eng.setMix01 (1.0);
            eng.prepare (fs, 2);
            const double D = (double) fc::VariPolyphaseResampler::kHalfTaps;
            const double reach = 4.0 * D * std::max (1.0, fs / std::min (clockHz, fs));
            const long long total = eng.latencySamples() + PAD + (long long) std::ceil (reach) + 200;
            std::vector<float> Lb ((size_t) total, 0.0f), Rb ((size_t) total, 0.0f);
            Lb[0] = 1.0f; Rb[0] = 1.0f;
            float* p[2] = { Lb.data(), Rb.data() };
            eng.process (p, 2, (int) total);
            long long peakIdx = 0; double peakVal = 0.0;
            for (long long i = 0; i < total; ++i)
            {
                const double v = std::fabs ((double) Lb[(size_t) i]);
                if (v > peakVal) { peakVal = v; peakIdx = i; }
            }
            return peakIdx;
        };

        // D2 fix (approved): wet transit now equals engine.latencySamples()
        // EXACTLY (kFifoSafetyPad is folded into the reported latency itself,
        // so no separate "+PAD" is added here anymore), and it is measured at
        // TWO different prepared clocks -- 48000 (== L) and 8000 (a genuinely
        // DIFFERENT, larger expected value, Lx8/Lpure8 from the contract pin
        // above) -- replacing the removed "transit is clock-invariant"
        // assumption with a per-clock "transit == reported latency" check,
        // which is the actual corrected contract.
        const long long lag48 = measureTransitLag (48000.0);
        if (absDiff (lag48, L) > 1)
            fail ("engineTransparencyLatencyTest(b3): wet transit lag=" + std::to_string (lag48)
                  + " expected=" + std::to_string (L) + " (clock=48000)" + tag);

        const long long lag8 = measureTransitLag (8000.0);
        if (absDiff (lag8, Lx8) > 1)
            fail ("engineTransparencyLatencyTest(b3): wet transit lag=" + std::to_string (lag8)
                  + " expected=" + std::to_string (Lx8) + " (clock=8000)" + tag);

        // (b)4: clock=8000 worst-case-buffer configuration, dry identity (abbreviated 1s).
        dryIdentityCase (8000.0, 1.0, 0x555ULL, 0x666ULL, "b4");

        // (c) mix=0.5: dry+wet alignment at the prepared clock. D2 fix
        // (approved): the previous revision misaligned dry (delayed by the
        // reported L) and wet (which actually emerged L + PAD later) by
        // exactly kFifoSafetyPad samples, so a mix=0.5 blend combed at
        // gain = |cos(pi*f0*PAD/fs)| < 1. With the fix, wet emerges at
        // EXACTLY the reported latency (== the dry delay), so the two copies
        // are sample-aligned and the blend is unity (0 dB, no comb) -- the
        // predicted gain is now 1.0. (No "+PAD" in `skip` either: wet is no
        // longer PAD samples late; a small extra 0.05 s skip past steady
        // state is retained.)
        {
            fc::Madoromi engine;
            engine.setWash01 (0.0); engine.setToneHz (6000.0); engine.setFrozen (false);
            engine.setBalance01 (0.0); engine.setClockHz (48000.0); engine.setMix01 (0.5);
            engine.prepare (fs, 2);

            const double f0 = 100.0, A = 0.5;
            const long long skip = engine.latencySamples() + (long long) std::llround (0.05 * fs);
            const long long total = skip + N;
            std::vector<float> Lb ((size_t) total), Rb ((size_t) total);
            for (long long i = 0; i < total; ++i)
            {
                const float v = (float) (A * std::sin (2.0 * kPi * f0 * (double) i / fs));
                Lb[(size_t) i] = v; Rb[(size_t) i] = v;
            }
            float* p[2] = { Lb.data(), Rb.data() };
            engine.process (p, 2, (int) total);
            const double mag = goertzelMag (Lb, (size_t) skip, (size_t) N, f0, fs);
            const double gMeasDb = 20.0 * std::log10 (mag / A);
            const double predDb = 0.0;   // aligned dry+wet -> unity (no comb)
            if (std::fabs (gMeasDb - predDb) > 0.2)
                fail ("engineTransparencyLatencyTest(c): mix=0.5 aligned-blend meas=" + std::to_string (gMeasDb)
                      + "dB pred=" + std::to_string (predDb) + "dB (dry/wet should align at prepared clock)" + tag);
        }
    }
    // §5.2: band-limiting (anti-alias) at the worst-case clock=8000 bracket.
    void engineAntiAliasTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);
        const double A = 0.8;
        const long long N = (long long) std::llround (fs * 0.01) * 100;   // 1.0 s, integer-periodic

        long long skip;
        {
            fc::Madoromi probe;
            probe.setClockHz (8000.0);
            probe.prepare (fs, 2);
            skip = probe.latencySamples() + fc::Madoromi::kFifoSafetyPad + (long long) std::llround (0.1 * fs);
        }

        auto measureDb = [&] (double freq) -> double
        {
            fc::Madoromi eng;
            eng.setWash01 (0.0); eng.setFrozen (false);
            eng.setBalance01 (0.0); eng.setClockHz (8000.0); eng.setMix01 (1.0);
            eng.prepare (fs, 2);
            const long long total = skip + N;
            std::vector<float> Lb ((size_t) total), Rb ((size_t) total);
            for (long long i = 0; i < total; ++i)
            {
                const float v = (float) (A * std::sin (2.0 * kPi * freq * (double) i / fs));
                Lb[(size_t) i] = v; Rb[(size_t) i] = v;
            }
            float* p[2] = { Lb.data(), Rb.data() };
            eng.process (p, 2, (int) total);
            const double mag = goertzelMag (Lb, (size_t) skip, (size_t) N, freq, fs);
            return 20.0 * std::log10 (mag / A);
        };

        // 1) positive control: deep passband, should be near-flat.
        const double gPassDb = measureDb (2000.0);
        if (gPassDb < -1.5 || gPassDb > 0.5)
            fail ("engineAntiAliasTest: 2000Hz passband gain " + std::to_string (gPassDb) + " dB outside [-1.5,+0.5]" + tag);

        // 2/3) 5000 Hz direct leak + 3000 Hz alias product (from the same run).
        {
            fc::Madoromi eng;
            eng.setWash01 (0.0); eng.setFrozen (false);
            eng.setBalance01 (0.0); eng.setClockHz (8000.0); eng.setMix01 (1.0);
            eng.prepare (fs, 2);
            const long long total = skip + N;
            std::vector<float> Lb ((size_t) total), Rb ((size_t) total);
            for (long long i = 0; i < total; ++i)
            {
                const float v = (float) (A * std::sin (2.0 * kPi * 5000.0 * (double) i / fs));
                Lb[(size_t) i] = v; Rb[(size_t) i] = v;
            }
            float* p[2] = { Lb.data(), Rb.data() };
            eng.process (p, 2, (int) total);

            const double leak  = goertzelMag (Lb, (size_t) skip, (size_t) N, 5000.0, fs) / A;
            const double alias = goertzelMag (Lb, (size_t) skip, (size_t) N, 3000.0, fs) / A;
            const double gateA = std::pow (10.0, -70.0 / 20.0);
            const double gateB = std::pow (10.0, -40.0 / 20.0);

            if (leak > gateA)
                fail ("engineAntiAliasTest: 5000Hz direct leak " + std::to_string (20.0 * std::log10 (leak)) + "dB > -70dB" + tag);
            if (alias > gateA)
                fail ("engineAntiAliasTest: 3000Hz alias product " + std::to_string (20.0 * std::log10 (alias)) + "dB > -70dB (gate A, design)" + tag);
            if (alias > gateB)
                fail ("engineAntiAliasTest: 3000Hz alias product " + std::to_string (20.0 * std::log10 (alias)) + "dB > -40dB (gate B, spec floor)" + tag);

            // Non-gating analytic cross-check (design-time confirmation, §3/§5.2-3).
            const double crossCheck = designDownGainAt (5000.0, fs, 8000.0);
            (void) crossCheck;
        }
    }
    // §5.3: CLOCK repitch. length=0.4s -> P=llround(0.4*48000)=19200 internal
    // (machine-time) samples; host-time loop period = P/C.
    void engineRepitchTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);
        const double A = 0.8;

        // Scenario A: pitch (positive zero-crossing count) tracks C2/C1.
        {
            fc::Madoromi engine;
            engine.setWash01 (0.0); engine.setMix01 (1.0); engine.setBalance01 (1.0);
            engine.setLengthSeconds (0.4); engine.setToneHz (6000.0); engine.setClockHz (32000.0);
            engine.prepare (fs, 2);

            long long phase = 0;
            auto processSineChunk = [&] (long long n) -> std::vector<float>
            {
                std::vector<float> Lb ((size_t) n), Rb ((size_t) n);
                for (long long i = 0; i < n; ++i)
                {
                    const float v = (float) (A * std::sin (2.0 * kPi * 1000.0 * (double) (phase + i) / fs));
                    Lb[(size_t) i] = v; Rb[(size_t) i] = v;
                }
                phase += n;
                float* p[2] = { Lb.data(), Rb.data() };
                engine.process (p, 2, (int) n);
                return Lb;
            };

            processSineChunk ((long long) std::llround (1.2 * fs));         // warm-up, loop off
            engine.setFrozen (true);
            processSineChunk ((long long) std::llround (0.3 * fs));         // post-freeze settle
            std::vector<float> seg1 = processSineChunk ((long long) std::llround (1.0 * fs));
            const int c1 = countPosCrossings (seg1, 0, seg1.size(), 0.05 * A);

            engine.setClockHz (16000.0);
            processSineChunk ((long long) std::llround (0.8 * fs));         // clock-change settle
            std::vector<float> seg2 = processSineChunk ((long long) std::llround (1.0 * fs));
            const int c2 = countPosCrossings (seg2, 0, seg2.size(), 0.05 * A);

            if (std::abs (c2 - 500) > 15)
                fail ("engineRepitchTest(A): c2=" + std::to_string (c2) + " not within 500+-15" + tag);
            const double ratio = (c1 != 0) ? (double) c2 / (double) c1 : -1.0;
            if (ratio < 0.5 * 0.97 || ratio > 0.5 * 1.03)
                fail ("engineRepitchTest(A): c2/c1=" + std::to_string (ratio) + " (c1=" + std::to_string (c1)
                      + " c2=" + std::to_string (c2) + ") outside [0.485,0.515]" + tag);
        }

        // Scenario B: loop period (onset interval) doubles in host time with the clock halved.
        {
            fc::Madoromi engine;
            engine.setWash01 (0.0); engine.setMix01 (1.0); engine.setBalance01 (1.0);
            engine.setLengthSeconds (0.4); engine.setToneHz (6000.0); engine.setClockHz (32000.0);
            engine.prepare (fs, 2);

            const long long cyc  = (long long) std::llround (0.6 * fs);
            const long long half = (long long) std::llround (0.3 * fs);
            long long phase = 0;
            auto processBurstChunk = [&] (long long n) -> std::vector<float>
            {
                std::vector<float> Lb ((size_t) n), Rb ((size_t) n);
                for (long long i = 0; i < n; ++i)
                {
                    const long long t = phase + i;
                    const long long m = t % cyc;
                    float v = 0.0f;
                    if (m < half)
                        v = (float) (A * std::sin (2.0 * kPi * 1000.0 * (double) t / fs));
                    Lb[(size_t) i] = v; Rb[(size_t) i] = v;
                }
                phase += n;
                float* p[2] = { Lb.data(), Rb.data() };
                engine.process (p, 2, (int) n);
                return Lb;
            };

            processBurstChunk (5 * cyc);   // 5 full cycles, loop off, ends at a silence->tone boundary
            engine.setFrozen (true);       // captured window = [tone 0.3 | silence 0.3]

            std::vector<float> base = processBurstChunk ((long long) std::llround (0.9 * fs));
            const auto onsetsBase = detectOnsets (base, fs, A);
            const double baseMedian = medianConsecutiveDiff (onsetsBase);

            engine.setClockHz (16000.0);
            processBurstChunk ((long long) std::llround (0.8 * fs));   // clock-change settle
            std::vector<float> fin = processBurstChunk ((long long) std::llround (4.8 * fs));   // ~4 periods at new clock
            const auto onsetsFin = detectOnsets (fin, fs, A);
            const double finMedian = medianConsecutiveDiff (onsetsFin);

            const double expectedBase = 0.6 * fs;
            const double expectedFin  = 1.2 * fs;

            if (onsetsBase.size() < 2)
                fail ("engineRepitchTest(B): fewer than 2 baseline onsets detected" + tag);
            else if (std::fabs (baseMedian - expectedBase) > 0.03 * expectedBase)
                fail ("engineRepitchTest(B): baseline onset interval=" + std::to_string (baseMedian)
                      + " expected~" + std::to_string (expectedBase) + tag);

            if (onsetsFin.size() < 2)
                fail ("engineRepitchTest(B): fewer than 2 post-change onsets detected" + tag);
            else if (std::fabs (finMedian - expectedFin) > 0.03 * expectedFin)
                fail ("engineRepitchTest(B): post-change onset interval=" + std::to_string (finMedian)
                      + " expected~" + std::to_string (expectedFin) + tag);

            if (onsetsBase.size() >= 2 && onsetsFin.size() >= 2)
            {
                const double growthRatio = finMedian / baseMedian;
                if (std::fabs (growthRatio - 2.0) > 0.03 * 2.0)
                    fail ("engineRepitchTest(B): interval growth ratio=" + std::to_string (growthRatio)
                          + " expected~2.00" + tag);
            }
        }
    }
    // §5.4: wash decay. D_max = washDecaySeconds(1.0) = 8.0s (pinned in §5.1).
    void engineWashDecayTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        fc::Madoromi engine;
        engine.setWash01 (1.0); engine.setToneHz (6000.0); engine.setFrozen (false);
        engine.setBalance01 (0.0); engine.setMix01 (1.0); engine.setClockHz (32000.0);
        engine.prepare (fs, 2);

        const long long L   = engine.latencySamples();
        const int PAD       = fc::Madoromi::kFifoSafetyPad;
        const long long impulseAt = (long long) std::llround (0.2 * fs);
        const long long origin    = impulseAt + L + PAD;   // impulse time + L + PAD, per header contract
        const long long fsI       = (long long) std::llround (fs);
        const long long total     = origin + (long long) std::llround (12.6 * fs) + (long long) std::llround (0.2 * fs);

        std::vector<float> Lb ((size_t) total, 0.0f), Rb ((size_t) total, 0.0f);
        Lb[(size_t) impulseAt] = 1.0f; Rb[(size_t) impulseAt] = 1.0f;
        float* p[2] = { Lb.data(), Rb.data() };
        engine.process (p, 2, (int) total);

        bool finite = true;
        for (long long i = 0; i < total && finite; ++i)
            if (! std::isfinite ((double) Lb[(size_t) i]) || ! std::isfinite ((double) Rb[(size_t) i])) finite = false;
        if (! finite)
            fail ("engineWashDecayTest: non-finite output" + tag);

        auto windowEnergy = [&] (long long start, long long len) -> double
        {
            double e = 0.0;
            const long long end = std::min (start + len, total);
            for (long long i = std::max<long long> (0, start); i < end; ++i)
                e += (double) Lb[(size_t) i] * (double) Lb[(size_t) i] + (double) Rb[(size_t) i] * (double) Rb[(size_t) i];
            return e;
        };

        const double E13   = windowEnergy (origin + 1 * fsI, 2 * fsI);
        const double E1012 = windowEnergy (origin + 10 * fsI, 2 * fsI);

        if (E13 <= 0.0)
            fail ("engineWashDecayTest: E[1..3] positive control is <= 0 (wash not audible)" + tag);

        const double ratio = (E13 > 0.0) ? E1012 / E13 : 1.0e300;
        if (ratio > 1e-4)
            fail ("engineWashDecayTest: E[10..12]/E[1..3]=" + std::to_string (ratio) + " > 1e-4" + tag);

        double tailPeak = 0.0;
        const long long tailStart = origin + (long long) std::llround (12.1 * fs);
        const long long tailEnd   = origin + (long long) std::llround (12.6 * fs);
        for (long long i = tailStart; i < tailEnd && i < total; ++i)
        {
            tailPeak = std::max (tailPeak, (double) std::fabs (Lb[(size_t) i]));
            tailPeak = std::max (tailPeak, (double) std::fabs (Rb[(size_t) i]));
        }
        if (tailPeak > 1e-3)
            fail ("engineWashDecayTest: tail residual peak=" + std::to_string (tailPeak) + " > 1e-3" + tag);
    }

    // D3 (approved local fix): the shared fct::impulseResponseNonIncreasing()
    // (DspInvariants.h) compares every window against window0 and NEVER
    // updates its running reference (the `prev = std::max(prev, 1e-300)` line
    // there only guards against a zero, it does not advance to `cur`) -- it
    // implicitly assumes window0 IS the global peak. That is false for
    // Madoromi at clock=8000: the large transit + anti-alias kernel spread
    // delays the FDN excitation enough that the impulse-response energy PEAK
    // lands in window1 (measured ratio ~1.24, failing the shared helper's
    // tolerance=1.05 gate) even though the engine is genuinely stable (loop
    // gain < 1) and decays monotonically from that peak onward. Fix: locate
    // the PEAK window first (a build-up/warm-up envelope is allowed), then
    // require non-increasing energy (same 1.05 tolerance -- NOT relaxed)
    // only from that peak onward. A genuine runaway cannot exploit this: a
    // divergence that never turns over has its "peak" sit at (or approach)
    // the LAST observed window, so the anti-runaway guard below (peak must
    // fall within the first third of the observed tail) fails it; a
    // divergence that DOES turn over late is still caught by the same 1.05
    // per-window tolerance across the (generous, ~4s) remaining tail. Kept
    // LOCAL to this test file (not added to DspInvariants.h) per the D3
    // constraint: a shared-helper change must not alter behaviour for any
    // other plugin's tests, and this local copy carries zero risk to them.
    template <typename Process>
    bool washNonIncreasingAfterPeak (Process&& process, double sampleRate,
                                      double tailSeconds, double windowSeconds, double tolerance)
    {
        const std::size_t total  = (std::size_t) (tailSeconds * sampleRate);
        const std::size_t window = (std::size_t) (windowSeconds * sampleRate);
        if (window == 0 || total <= window) return true;

        std::vector<double> y (total);
        for (std::size_t n = 0; n < total; ++n) y[n] = process (n == 0 ? 1.0 : 0.0);
        if (! fct::allFinite (y)) return false;

        std::vector<double> energies;
        for (std::size_t start = 0; start + window <= total; start += window)
            energies.push_back (fct::windowEnergy (y, start, window));
        if (energies.size() < 2) return true;

        std::size_t peakIdx = 0;
        for (std::size_t i = 1; i < energies.size(); ++i)
            if (energies[i] > energies[peakIdx]) peakIdx = i;

        // Anti-runaway guard: a real divergence never turns over, so its
        // "peak" sits at (or near) the LAST observed window -- require the
        // measured peak to land within the first third of the tail (a
        // generous warm-up allowance, well beyond the single-window build-up
        // this fix targets), so this check still fails a genuine runaway.
        if (peakIdx >= energies.size() / 3)
            return false;

        double prev = energies[peakIdx];
        for (std::size_t i = peakIdx + 1; i < energies.size(); ++i)
        {
            if (energies[i] > prev * tolerance) return false;
            prev = std::max (energies[i], 1e-300);
        }
        return true;
    }

    // Regression class A: worst-case wash feedback stability (loop gain < 1).
    void engineWashStabilityTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);
        const double clocks[] = { 32000.0, 8000.0 };
        for (double clockHz : clocks)
        {
            fc::Madoromi engine;
            engine.setWash01 (1.0); engine.setToneHz (18000.0); engine.setFrozen (false);
            engine.setBalance01 (0.0); engine.setMix01 (1.0); engine.setClockHz (clockHz);
            engine.prepare (fs, 2);

            auto perSample = [&] (double x) -> double
            {
                float buf[1] = { (float) x };
                float* p[1] = { buf };
                engine.process (p, 1, 1);
                return (double) buf[0];
            };

            if (! washNonIncreasingAfterPeak (perSample, fs, 6.0, 0.25, 1.05))
                fail ("engineWashStabilityTest: impulse response energy increased after its build-up peak (clock=" + std::to_string (clockHz) + ")" + tag);
        }
    }
    // §5.6: long-hold worst case. 8s stereo LCG, prime-chunk driven, full-range +
    // out-of-range parameter churn every 50ms, clock alternation every 0.5s (+
    // occasional mid-range value), loop toggling every 0.7s plus one rapid
    // on/off/on retrigger burst at 50ms spacing.
    void engineLongHoldChurnTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        fc::Madoromi engine;
        engine.prepare (fs, 2);

        const long long total = (long long) std::llround (8.0 * fs);
        std::vector<float> outL ((size_t) total), outR ((size_t) total);
        Lcg lcgL (0xC0FFEEULL), lcgR (0xFACADEULL), lcgMid (0xBADA55ULL);

        const long long paramPeriod = std::max<long long> (1, std::llround (0.05 * fs));
        const long long clockPeriod = std::max<long long> (1, std::llround (0.5  * fs));
        const long long loopPeriod  = std::max<long long> (1, std::llround (0.7  * fs));
        const long long burstAt     = std::max<long long> (1, std::llround (4.0  * fs));
        const long long burstStep   = std::max<long long> (1, std::llround (0.05 * fs));

        static const double washVals[]    = { 0.0, 1.0, 0.5, 0.9, 0.1 };
        static const double toneVals[]    = { 1500.0, 18000.0, 6000.0, 500.0, 1.0e5 };
        static const double lengthVals[]  = { 0.05, 2.0, 0.4, 5.0, 0.001 };
        static const double balanceVals[] = { 0.0, 1.0, 0.5, -1.0, 2.0 };
        static const double mixVals[]     = { 0.0, 1.0, 0.5, -1.0, 2.0 };
        int paramIdx = 0;

        int clockIdx = 0;
        bool loopState = false;
        bool burstDone = false;
        int burstStepsLeft = 0;

        long long pos = 0;
        long long nextParam = 0, nextClock = 0, nextLoop = loopPeriod;
        size_t chunkI = 0;

        while (pos < total)
        {
            if (pos >= nextParam)
            {
                engine.setWash01 (washVals[(size_t) paramIdx % 5]);
                engine.setToneHz (toneVals[(size_t) paramIdx % 5]);
                engine.setLengthSeconds (lengthVals[(size_t) paramIdx % 5]);
                engine.setBalance01 (balanceVals[(size_t) paramIdx % 5]);
                engine.setMix01 (mixVals[(size_t) paramIdx % 5]);
                ++paramIdx;
                nextParam += paramPeriod;
            }
            if (pos >= nextClock)
            {
                if (clockIdx % 4 == 3)
                    engine.setClockHz (8000.0 + 40000.0 * 0.5 * (lcgMid.next() + 1.0));
                else
                    engine.setClockHz ((clockIdx % 2 == 0) ? 8000.0 : 48000.0);
                ++clockIdx;
                nextClock += clockPeriod;
            }

            long long chunk;
            if (burstStepsLeft > 0)
            {
                loopState = ! loopState;
                engine.setFrozen (loopState);
                chunk = std::min<long long> (burstStep, total - pos);
                --burstStepsLeft;
            }
            else if (! burstDone && pos >= burstAt)
            {
                burstStepsLeft = 3;
                burstDone = true;
                continue;
            }
            else
            {
                if (pos >= nextLoop) { loopState = ! loopState; engine.setFrozen (loopState); nextLoop += loopPeriod; }
                chunk = std::min<long long> ((long long) kPrimeChunks[chunkI % kPrimeChunksLen], total - pos);
                ++chunkI;
            }

            for (long long i = 0; i < chunk; ++i)
            {
                outL[(size_t) (pos + i)] = (float) lcgL.next();
                outR[(size_t) (pos + i)] = (float) lcgR.next();
            }
            float* p[2] = { outL.data() + pos, outR.data() + pos };
            engine.process (p, 2, (int) chunk);
            pos += chunk;
        }

        bool finite = true; double peak = 0.0;
        for (long long i = 0; i < total; ++i)
        {
            const double l = (double) outL[(size_t) i], r = (double) outR[(size_t) i];
            if (! std::isfinite (l) || ! std::isfinite (r)) { finite = false; break; }
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
        }
        if (! finite) fail ("engineLongHoldChurnTest: non-finite output during churn" + tag);
        if (peak > 1.5) fail ("engineLongHoldChurnTest: peak " + std::to_string (peak) + " > 1.5 during churn" + tag);

        // Post-churn decay: fix clock/wash/loop to a benign state, feed silence,
        // and confirm the tail settles (no stuck oscillation / stack residue).
        engine.setClockHz (48000.0);
        engine.setWash01 (0.0);
        engine.setFrozen (false);
        const long long silence = (long long) std::llround (2.0 * fs);
        std::vector<float> sL ((size_t) silence, 0.0f), sR ((size_t) silence, 0.0f);
        float* ps[2] = { sL.data(), sR.data() };
        engine.process (ps, 2, (int) silence);

        const long long tailLen = (long long) std::llround (0.2 * fs);
        double tailPeak = 0.0;
        for (long long i = silence - tailLen; i < silence; ++i)
        {
            tailPeak = std::max (tailPeak, (double) std::fabs (sL[(size_t) i]));
            tailPeak = std::max (tailPeak, (double) std::fabs (sR[(size_t) i]));
        }
        if (tailPeak > 1e-3)
            fail ("engineLongHoldChurnTest: post-churn decay tail peak=" + std::to_string (tailPeak) + " > 1e-3" + tag);
    }
    // §5.7: NaN/Inf input recovery. Normal operation with a loop-on region;
    // inject NaN/+Inf/-Inf into the INPUT (guarded to 0 before reaching the
    // engine); output must stay finite and within the house peak bound.
    void engineNanRecoveryTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        fc::Madoromi engine;
        engine.setWash01 (0.7); engine.setMix01 (0.7); engine.setClockHz (32000.0);
        engine.setBalance01 (0.5); engine.setToneHz (6000.0); engine.setFrozen (false);
        engine.prepare (fs, 2);

        const long long total = (long long) std::llround (3.5 * fs);
        std::vector<float> L ((size_t) total), R ((size_t) total);
        Lcg lcgL (0xE1E1ULL), lcgR (0xE2E2ULL);
        for (long long i = 0; i < total; ++i)
        {
            L[(size_t) i] = (float) (0.6 * lcgL.next());
            R[(size_t) i] = (float) (0.6 * lcgR.next());
        }

        const long long tOn  = (long long) std::llround (1.0 * fs);   // enter loop-on region
        const long long tNan = (long long) std::llround (1.1 * fs);
        const long long tPI  = (long long) std::llround (1.2 * fs);
        const long long tNI  = (long long) std::llround (1.3 * fs);
        L[(size_t) tNan] = std::numeric_limits<float>::quiet_NaN();
        L[(size_t) tPI]  = std::numeric_limits<float>::infinity();
        L[(size_t) tNI]  = -std::numeric_limits<float>::infinity();

        float* p1[2] = { L.data(), R.data() };
        engine.process (p1, 2, (int) tOn);
        engine.setFrozen (true);
        float* p2[2] = { L.data() + tOn, R.data() + tOn };
        engine.process (p2, 2, (int) (total - tOn));

        bool finite = true;
        for (long long i = 0; i < total && finite; ++i)
            if (! std::isfinite ((double) L[(size_t) i]) || ! std::isfinite ((double) R[(size_t) i])) finite = false;
        if (! finite)
            fail ("engineNanRecoveryTest: non-finite output after injected NaN/Inf" + tag);

        double peak = 0.0;
        for (long long i = tNan; i < total; ++i)
        {
            peak = std::max (peak, (double) std::fabs (L[(size_t) i]));
            peak = std::max (peak, (double) std::fabs (R[(size_t) i]));
        }
        if (peak > 1.5)
            fail ("engineNanRecoveryTest: peak " + std::to_string (peak) + " > 1.5 after injection" + tag);
    }
    // §5.8: param-NaN guard, bit-exact A/B (house standard). setFrozen excluded
    // (bool, not covered by the non-finite guard rule).
    void engineParamNanGuardTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        fc::Madoromi a; prepareScheduleEngine (a, fs);
        std::vector<float> aL, aR;
        runSchedule (a, fs, false, aL, aR);

        fc::Madoromi b; prepareScheduleEngine (b, fs);
        std::vector<float> bL, bR;
        runSchedule (b, fs, true, bL, bR);

        if (aL.size() != bL.size() || std::memcmp (aL.data(), bL.data(), aL.size() * sizeof (float)) != 0
         || aR.size() != bR.size() || std::memcmp (aR.data(), bR.data(), aR.size() * sizeof (float)) != 0)
            fail ("engineParamNanGuardTest: A/B runs not bit-identical" + tag);
    }

    // §5.9: reset residue (silence, exact), reset-then-rerun determinism
    // (matches a fresh prepare's run bit-for-bit), and silent-input -> exact 0.
    void engineResetAndDeterminismTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);

        // 1) excite (wash impulses, freeze, clock change) then reset(): residue <= 1e-12.
        {
            fc::Madoromi engine;
            engine.setWash01 (1.0); engine.setToneHz (6000.0); engine.setBalance01 (0.5);
            engine.setMix01 (1.0); engine.setClockHz (32000.0);
            engine.prepare (fs, 2);

            const long long total = (long long) std::llround (1.5 * fs);
            std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
            const long long impPeriod = std::max<long long> (1, (long long) std::llround (0.2 * fs));
            for (long long n = 0; n < total; n += impPeriod) { L[(size_t) n] = 1.0f; R[(size_t) n] = 1.0f; }

            const long long half = total / 2;
            float* p1[2] = { L.data(), R.data() };
            engine.process (p1, 2, (int) half);
            engine.setFrozen (true);
            engine.setClockHz (16000.0);
            float* p2[2] = { L.data() + half, R.data() + half };
            engine.process (p2, 2, (int) (total - half));

            engine.reset();

            const long long silence = (long long) std::llround (0.5 * fs);
            std::vector<float> sL ((size_t) silence, 0.0f), sR ((size_t) silence, 0.0f);
            float* p3[2] = { sL.data(), sR.data() };
            engine.process (p3, 2, (int) silence);

            double peak = 0.0;
            for (size_t i = 0; i < sL.size(); ++i)
            {
                peak = std::max (peak, (double) std::fabs (sL[i]));
                peak = std::max (peak, (double) std::fabs (sR[i]));
            }
            if (peak > 1e-12)
                fail ("engineResetAndDeterminismTest: reset residue " + std::to_string (peak) + " > 1e-12" + tag);
        }

        // 2) reset()-then-rerun of the §5.8 schedule matches a fresh prepare's run,
        // bit-for-bit (frozenParam is explicitly reconciled: it survives reset()).
        {
            fc::Madoromi fresh; prepareScheduleEngine (fresh, fs);
            std::vector<float> freshL, freshR;
            runSchedule (fresh, fs, false, freshL, freshR);

            fc::Madoromi engine2;
            engine2.setWash01 (0.8); engine2.setClockHz (20000.0);
            engine2.prepare (fs, 2);
            const long long junkLen = (long long) std::llround (0.3 * fs);
            std::vector<float> jL ((size_t) junkLen), jR ((size_t) junkLen);
            Lcg jlcgL (0x99ULL), jlcgR (0x88ULL);
            for (long long i = 0; i < junkLen; ++i)
            {
                jL[(size_t) i] = (float) jlcgL.next();
                jR[(size_t) i] = (float) jlcgR.next();
            }
            float* jp[2] = { jL.data(), jR.data() };
            engine2.process (jp, 2, (int) junkLen);

            // Reconcile to the schedule's baseline targets before reset() so the
            // snapped smoothers match the fresh-prepare case exactly.
            engine2.setClockHz (32000.0);
            engine2.setWash01 (0.4);
            engine2.setToneHz (6000.0);
            engine2.setLengthSeconds (0.3);
            engine2.setBalance01 (0.4);
            engine2.setMix01 (0.6);
            engine2.setFrozen (false);
            engine2.reset();

            std::vector<float> repL, repR;
            runSchedule (engine2, fs, false, repL, repR);

            if (freshL.size() != repL.size() || std::memcmp (freshL.data(), repL.data(), freshL.size() * sizeof (float)) != 0
             || freshR.size() != repR.size() || std::memcmp (freshR.data(), repR.data(), freshR.size() * sizeof (float)) != 0)
                fail ("engineResetAndDeterminismTest: reset()-then-rerun != fresh-prepare run" + tag);
        }

        // 3) silent input -> exact silent output (no phantom wash, class J).
        {
            fc::Madoromi engine;
            engine.prepare (fs, 2);
            const long long total = (long long) std::llround (1.0 * fs);
            std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
            float* p[2] = { L.data(), R.data() };
            engine.process (p, 2, (int) total);
            for (long long i = 0; i < total; ++i)
            {
                if (L[(size_t) i] != 0.0f || R[(size_t) i] != 0.0f)
                { fail ("engineResetAndDeterminismTest: silent input produced non-zero output at i=" + std::to_string (i) + tag); break; }
            }
        }
    }
    // Determinism-contract derivative: with ALL parameters static (set before
    // prepare, no mid-run setter calls -> smoothers snap exactly at reset()),
    // the chunk split used to deliver the same input must not affect the output.
    void engineChunkInvarianceTest (double fs)
    {
        const std::string tag = " @fs=" + std::to_string (fs);
        const long long total = (long long) std::llround (2.0 * fs);
        Lcg lcgL (0xAAAAULL), lcgR (0xBBBBULL);
        std::vector<float> inL ((size_t) total), inR ((size_t) total);
        for (long long i = 0; i < total; ++i)
        {
            inL[(size_t) i] = (float) (0.6 * lcgL.next());
            inR[(size_t) i] = (float) (0.6 * lcgR.next());
        }

        auto run = [&] (bool primeChunks, std::vector<float>& outL, std::vector<float>& outR)
        {
            fc::Madoromi engine;
            engine.setWash01 (0.5); engine.setClockHz (32000.0); engine.setToneHz (6000.0);
            engine.setBalance01 (0.3); engine.setMix01 (0.7); engine.setFrozen (false);
            engine.prepare (fs, 2);

            outL = inL; outR = inR;
            long long pos = 0;
            size_t chunkI = 0;
            while (pos < total)
            {
                const long long chunk = primeChunks
                    ? std::min<long long> ((long long) kPrimeChunks[chunkI % kPrimeChunksLen], total - pos)
                    : std::min<long long> (512, total - pos);
                ++chunkI;
                float* p[2] = { outL.data() + pos, outR.data() + pos };
                engine.process (p, 2, (int) chunk);
                pos += chunk;
            }
        };

        std::vector<float> aL, aR, bL, bR;
        run (false, aL, aR);
        run (true,  bL, bR);

        // D1 (approved resolution): bit-exact chunk invariance is
        // mathematically impossible here. VariPolyphaseResampler's per-output
        // step ramp (outPos += step; step += kRampAlpha*(targetStep - step))
        // makes outPos/step a running, NON-ASSOCIATIVE floating-point
        // accumulator -- its state after N samples depends on the exact
        // sequence of process() call boundaries (the order floating-point
        // additions are grouped in), not just on the N samples delivered, so
        // splitting the same total input into a different chunk schedule
        // changes that accumulation order. The resulting divergence is tiny
        // (~1e-6 absolute), confined to isolated samples, and the two streams
        // RE-CONVERGE immediately afterward (not a growing drift) -- so a
        // tolerance-based comparison is the CORRECT oracle, not a loosened
        // gate. Sample-COUNT alignment stays an EXACT check.
        //
        // Metric fix (2026-07-13, approved): a PURE relative-error metric
        // (da / max(|a|,|b|,1e-9)) is not a valid oracle here -- near a
        // zero-crossing both streams pass through values far smaller than the
        // ~1e-6 absolute divergence itself, so the 1e-9 denominator floor
        // divides a ~1e-6 absolute diff by a ~1e-6-scale signal and reports a
        // spurious O(1) "relative error" even though the two streams agree to
        // within the same benign absolute tolerance everywhere else. Measured
        // directly (worst absolute diff over the full standard rate matrix):
        // ~1e-6..~4e-6, i.e. the SAME order as the pre-D2/D6 engine -- D2/D6
        // did NOT introduce a new chunk-dependence bug. Fixed with the
        // standard absolute+relative criterion so near-zero samples are
        // judged on absolute difference instead of blowing up a relative
        // ratio: |a-b| <= atol + rtol*max(|a|,|b|), atol chosen with margin
        // above the measured ~4e-6 worst case, rtol for the (already-passing,
        // far-from-zero) bulk of samples.
        if (aL.size() != bL.size() || aR.size() != bR.size())
        {
            fail ("engineChunkInvarianceTest: fixed-512 vs prime-chunk runs produced different sample counts" + tag);
            return;
        }

        const double atol = 1.0e-5, rtol = 1.0e-5;
        double worstMargin = 0.0;   // (|a-b| - (atol + rtol*max(|a|,|b|))), > 0 => failing sample
        size_t worstIdx = 0; double worstDa = 0.0, worstA = 0.0, worstB = 0.0;
        for (size_t i = 0; i < aL.size(); ++i)
        {
            const double daL = std::fabs ((double) aL[i] - (double) bL[i]);
            const double mL  = std::max (std::fabs ((double) aL[i]), std::fabs ((double) bL[i]));
            const double marginL = daL - (atol + rtol * mL);
            if (marginL > worstMargin) { worstMargin = marginL; worstIdx = i; worstDa = daL; worstA = aL[i]; worstB = bL[i]; }

            const double daR = std::fabs ((double) aR[i] - (double) bR[i]);
            const double mR  = std::max (std::fabs ((double) aR[i]), std::fabs ((double) bR[i]));
            const double marginR = daR - (atol + rtol * mR);
            if (marginR > worstMargin) { worstMargin = marginR; worstIdx = i; worstDa = daR; worstA = aR[i]; worstB = bR[i]; }
        }
        if (worstMargin > 0.0)
            fail ("engineChunkInvarianceTest: fixed-512 vs prime-chunk runs diverged beyond atol=" + std::to_string (atol)
                  + " + rtol=" + std::to_string (rtol) + "*max(|a|,|b|) at i=" + std::to_string (worstIdx)
                  + " |a-b|=" + std::to_string (worstDa) + " a=" + std::to_string (worstA) + " b=" + std::to_string (worstB)
                  + " (static params)" + tag);
    }

    // Regression class F: parameter smoothing actually exists (a discontinuous
    // 0->1 step must not appear as a 1-sample jump in the output).
    void smoothingStepCase (double fs, bool stepMix)
    {
        const std::string tag = " @fs=" + std::to_string (fs) + (stepMix ? " (mix)" : " (balance)");

        fc::Madoromi engine;
        engine.setWash01 (0.0); engine.setClockHz (32000.0); engine.setToneHz (6000.0);
        if (stepMix) { engine.setBalance01 (1.0); engine.setMix01 (0.0); }
        else         { engine.setBalance01 (0.0); engine.setMix01 (1.0); }
        engine.setFrozen (false);
        engine.prepare (fs, 2);

        const long long warm = (long long) std::llround (1.0 * fs);
        std::vector<float> L1 ((size_t) warm, 0.5f), R1 ((size_t) warm, 0.5f);
        float* p1[2] = { L1.data(), R1.data() };
        engine.process (p1, 2, (int) warm);

        engine.setFrozen (true);

        const long long wait = (long long) std::llround (0.3 * fs);
        std::vector<float> L2 ((size_t) wait, 0.0f), R2 ((size_t) wait, 0.0f);
        float* p2[2] = { L2.data(), R2.data() };
        engine.process (p2, 2, (int) wait);

        if (stepMix) engine.setMix01 (1.0);
        else         engine.setBalance01 (1.0);

        const long long obs = (long long) std::llround (0.6 * fs);
        std::vector<float> L3 ((size_t) obs, 0.0f), R3 ((size_t) obs, 0.0f);
        float* p3[2] = { L3.data(), R3.data() };
        engine.process (p3, 2, (int) obs);

        bool finite = true;
        for (long long i = 0; i < obs && finite; ++i)
            if (! std::isfinite ((double) L3[(size_t) i]) || ! std::isfinite ((double) R3[(size_t) i])) finite = false;
        if (! finite) fail ("engineParamSmoothingTest: non-finite during transition" + tag);

        double maxDelta = 0.0;
        for (long long i = 1; i < obs; ++i)
        {
            maxDelta = std::max (maxDelta, std::fabs ((double) L3[(size_t) i] - (double) L3[(size_t) (i - 1)]));
            maxDelta = std::max (maxDelta, std::fabs ((double) R3[(size_t) i] - (double) R3[(size_t) (i - 1)]));
        }
        const double slopeLimit = 0.7 / (0.02 * fs) + 1e-6;
        if (maxDelta > slopeLimit)
            fail ("engineParamSmoothingTest: max|delta out|=" + std::to_string (maxDelta)
                  + " > " + std::to_string (slopeLimit) + tag);

        const long long settleIdx = (long long) std::llround (0.4 * fs);
        if (settleIdx < obs)
        {
            const double vL = (double) L3[(size_t) settleIdx];
            const double vR = (double) R3[(size_t) settleIdx];
            if (std::fabs (vL - 0.5) > 1e-3 || std::fabs (vR - 0.5) > 1e-3)
                fail ("engineParamSmoothingTest: not settled to 0.5 by 0.4s (L=" + std::to_string (vL)
                      + " R=" + std::to_string (vR) + ")" + tag);
        }
    }

    void engineParamSmoothingTest (double fs)
    {
        smoothingStepCase (fs, false);   // balance 0->1 step
        smoothingStepCase (fs, true);    // mix 0->1 step
        // clock smoothing (tau=100ms + 1/512 ramp) is exercised by §5.3's settle
        // behaviour already -- no separate probe here (per plan §5.11-5).
    }

} // namespace

int main (int argc, char** argv)
{
    for (double fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (fs);

    if (g_failures > 0)
    {
        std::fprintf (stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts ("madoromi dsp_test: all OK");
    return 0;
}
