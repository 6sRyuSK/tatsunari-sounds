//
// dsp_test.cpp — headless verification of the omoide-echo DSP core.
//
// Full spec-based suite per the reconciled test plan (scratchpad/omoide-test-plan.md,
// FINAL reconciled version, reworked-engine facts). See .claude/skills/write-dsp-test
// and docs/regression-policy.md for the house testing philosophy: every check needs
// an independent oracle (never derived from the code path under test) and must run
// across the full sample-rate matrix.
//
// Oracle discipline: matching OmoideEcho's PUBLISHED pure functions
// (latencyForRate / delaySamplesForMs / scanAgeSecondsForScan01) is contract
// verification (allowed); the plan additionally requires transcribing the closed-form
// formulas test-side for a second, independent cross-check (see
// engineDryDelayLatencyTest's quadruple pin). z-domain filter magnitudes
// (onePoleMag) and the index-stretch Kaiser design gain (designDownGainAt) are
// evaluated analytically here, never by calling into the engine's own process path.
//
#include "factory_core/HistoryBuffer.h"
#include "factory_core/OmoideEcho.h"
#include "factory_core/OnePole.h"
#include "factory_core/VariPolyphaseResampler.h"
#include "factory_core/KaiserBessel.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace fct = factory_core::testing;
namespace fc  = factory_core;

namespace
{
    int g_failures = 0;
    void fail (const std::string& m)
    {
        ++g_failures;
        std::fprintf (stderr, "FAIL: %s\n", m.c_str());
    }

    constexpr double kPi = 3.14159265358979323846;

    // ------------------------------------------------------------------ §3
    // Common test-side helpers. None of these call into OmoideEcho's process
    // path -- they are independent oracles (analytic / signal-processing
    // primitives standard to this test suite, self-contained here).

    // Deterministic LCG, uniform in [-1, 1). Test-side only (not from any
    // production header) -- standard across this repo's dsp_test.cpp files.
    struct Lcg
    {
        uint64_t s;
        explicit Lcg (uint64_t seed) : s (seed) {}
        double next() noexcept
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return (double) (int64_t) (s >> 11) * (1.0 / 4503599627370496.0);
        }
    };

    // Integer-period Goertzel single-frequency magnitude (amplitude, 2/N
    // normalised for a real sinusoid). `start`/`N` in samples of `x`.
    double goertzelMag (const std::vector<double>& x, size_t start, size_t N, double f, double fs)
    {
        const double w     = 2.0 * kPi * f / fs;
        const double coeff = 2.0 * std::cos (w);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            const double xi = (start + i < x.size()) ? x[start + i] : 0.0;
            s0 = xi + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double real = s1 - s2 * std::cos (w);
        const double imag = s2 * std::sin (w);
        return std::sqrt (real * real + imag * imag) * (2.0 / (double) N);
    }

    // z-domain one-pole lowpass magnitude |H(f)| -- independent of OnePole.h's
    // own implementation (transcribed formula: a = exp(-2*pi*fc/fsInt)),
    // matching OnePole::setCutoff's clamp [1, 0.49*fsInt] for completeness
    // (tone in-range never reaches the clamp -- R4).
    double onePoleMag (double f, double fcHz, double fsInt)
    {
        const double fc = std::clamp (fcHz, 1.0, 0.49 * fsInt);
        const double a  = std::exp (-2.0 * kPi * fc / fsInt);
        const double w  = 2.0 * kPi * f / fsInt;
        return (1.0 - a) / std::sqrt (1.0 - 2.0 * a * std::cos (w) + a * a);
    }

    // Positive-going zero-crossing counter with hysteresis (±hyst) to reject
    // noise-triggered multi-counts near zero. Independent of any phase model --
    // pure signal-processing primitive used as the Doppler-glide oracle (#4).
    int countPosCrossings (const std::vector<double>& x, size_t start, size_t n, double hyst)
    {
        int count = 0;
        int state = 0; // -1 = below -hyst, +1 = above +hyst, 0 = neutral/unknown
        for (size_t i = 0; i < n; ++i)
        {
            const size_t idx = start + i;
            if (idx >= x.size()) break;
            const double v = x[idx];
            if (v <= -hyst) state = -1;
            else if (v >= hyst)
            {
                if (state == -1) ++count;
                state = 1;
            }
        }
        return count;
    }

    // Cross-correlation argmax: the lag in [lo, hi] maximising
    // sum_k sig[lag + k] * tmpl[k]. Used for onset/marker detection (#3, #5,
    // §5.8b) -- an independent template-matching oracle, no engine internals.
    long xcorrArgmax (const std::vector<double>& sig, const std::vector<double>& tmpl, long lo, long hi)
    {
        long bestLag = lo;
        double bestScore = -1.0e300;
        for (long lag = lo; lag <= hi; ++lag)
        {
            double s = 0.0;
            for (size_t k = 0; k < tmpl.size(); ++k)
            {
                const long idx = lag + (long) k;
                if (idx < 0 || idx >= (long) sig.size()) continue;
                s += sig[(size_t) idx] * tmpl[k];
            }
            if (s > bestScore) { bestScore = s; bestLag = lag; }
        }
        return bestLag;
    }

    // #6 design cross-check (NON-GATING, diagnostic only -- see plan §0.3-3 /
    // §5.7): numerically integrates the SAME index-stretch Kaiser prototype
    // design formula documented in VariPolyphaseResampler.h's own contract
    // ("Kernel index-stretch" section) as an independent DTFT evaluation --
    // this is a from-scratch transcription of the published design formula,
    // not a call into VariPolyphaseResampler::process()/table. `fsIn`/`fsOut`
    // define the down-stage ratio (fsIn = host fs, fsOut = 24000 for the down
    // stage); `fHz` is evaluated relative to fsIn (the down stage's own
    // "input" domain).
    double designDownGainAt (double fHz, double fsIn, double fsOut)
    {
        const double s          = fsIn / fsOut;
        const double stretch    = std::max (1.0, s);
        const double kHalfTapsD = (double) fc::VariPolyphaseResampler::kHalfTaps;
        const double kBeta      = fc::VariPolyphaseResampler::kBeta;
        const double kFc1       = fc::VariPolyphaseResampler::kFc1;
        const double i0b        = fc::besselI0 (kBeta);

        const double omega = 2.0 * kPi * fHz / fsIn;
        const double reach = kHalfTapsD * stretch;
        const double dtau  = 1.0 / 32.0; // dense sub-sample integration step

        double acc = 0.0, wsum = 0.0;
        for (double tau = -reach; tau <= reach; tau += dtau)
        {
            const double t = std::abs (tau) / stretch;
            double win = 0.0;
            if (t <= kHalfTapsD)
            {
                const double r = t / kHalfTapsD;
                win = fc::besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            }
            const double x = 2.0 * kFc1 * t;
            const double sincv = (std::abs (x) < 1.0e-9) ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double w = sincv * win;
            acc  += w * std::cos (omega * tau);
            wsum += w;
        }
        return (wsum != 0.0) ? std::abs (acc / wsum) : 0.0;
    }

    // Engine driver: prime chunk sizes to exercise chunk-split invariance /
    // determinism (never the caller's real block size -- process() always
    // internally slices to kHostChunk anyway).
    const int kPrimeChunks[] = { 1, 2, 3, 5, 7, 13, 31, 61, 127, 251, 509 };
    constexpr size_t kNumPrimeChunks = sizeof (kPrimeChunks) / sizeof (kPrimeChunks[0]);

    std::vector<double> toDoubleVec (const std::vector<float>& x)
    {
        return std::vector<double> (x.begin(), x.end());
    }

    bool allFiniteF (const std::vector<float>& x)
    {
        for (float v : x) if (! std::isfinite (v)) return false;
        return true;
    }

    double peakAbsF (const std::vector<float>& x)
    {
        double p = 0.0;
        for (float v : x) p = std::max (p, (double) std::fabs (v));
        return p;
    }

    // Drives `eng` in-place over `bufL`/`bufR` (already containing the input,
    // overwritten with output) using a fixed chunk size.
    void processFixedChunk (fc::OmoideEcho& eng, std::vector<float>& bufL, std::vector<float>& bufR, int chunk)
    {
        const long long n = (long long) bufL.size();
        long long offset = 0;
        while (offset < n)
        {
            const int h = (int) std::min<long long> (chunk, n - offset);
            float* ptrs[2] = { bufL.data() + offset, bufR.data() + offset };
            eng.process (ptrs, 2, h);
            offset += h;
        }
    }

    // Drives `eng` in-place cycling through kPrimeChunks for chunk sizes.
    void processPrimeChunks (fc::OmoideEcho& eng, std::vector<float>& bufL, std::vector<float>& bufR)
    {
        const long long n = (long long) bufL.size();
        long long offset = 0;
        size_t idx = 0;
        while (offset < n)
        {
            const int h = (int) std::min<long long> (kPrimeChunks[idx % kNumPrimeChunks], n - offset);
            float* ptrs[2] = { bufL.data() + offset, bufR.data() + offset };
            eng.process (ptrs, 2, h);
            offset += h;
            ++idx;
        }
    }

    // Streaming driver for the long (§5.6, 120 s) test: generates input
    // sample-by-sample via `emitInput(idx, l, r)` (called strictly in
    // increasing index order), tracks running finite/peak, and forwards each
    // produced chunk to `onOutput(offset, l, r, n)` for small-window capture.
    template <typename EmitInput, typename OnOutput>
    void streamRun (fc::OmoideEcho& eng, long long totalSamples, int chunk,
                    EmitInput&& emitInput, OnOutput&& onOutput,
                    bool& finiteOk, double& peakOut)
    {
        std::vector<float> bufL ((size_t) chunk), bufR ((size_t) chunk);
        long long offset = 0;
        while (offset < totalSamples)
        {
            const int n = (int) std::min<long long> (chunk, totalSamples - offset);
            for (int i = 0; i < n; ++i)
                emitInput (offset + i, bufL[(size_t) i], bufR[(size_t) i]);

            float* ptrs[2] = { bufL.data(), bufR.data() };
            eng.process (ptrs, 2, n);

            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (bufL[(size_t) i]) || ! std::isfinite (bufR[(size_t) i]))
                    finiteOk = false;
                peakOut = std::max ({ peakOut, (double) std::fabs (bufL[(size_t) i]), (double) std::fabs (bufR[(size_t) i]) });
            }
            onOutput (offset, bufL.data(), bufR.data(), n);
            offset += n;
        }
    }

    // Cross-rate diagnostic collector for #6's "±6 dB spread across rates"
    // invariant (only meaningful when the full matrix runs in one process --
    // see engineBracketBandLimitTest / checkAliasCrossRateSpread).
    std::vector<double> g_aliasDbByRate;

    // ---- primitive (HistoryBuffer standalone, §4) --------------------------
    void historyBufferInterpExactnessTest (double fs);   // #5 prim
    void historyBufferWrapGuardResetTest   (double fs);  // #5/#9 prim

    // ---- engine (OmoideEcho, §5) -------------------------------------------
    void engineEchoScheduleRatioTest       (double fs);  // #1
    void engineLoopGainStabilityTest       (double fs);  // #2a
    void engineLongHoldPeakTest            (double fs);  // #2b
    void engineScanAgeAccuracyTest         (double fs);  // #3
    void engineScanDopplerGlideTest        (double fs);  // #4
    void engineHistoryCapacityLongRunTest  (double fs);  // #5
    void engineBracketBandLimitTest        (double fs);  // #6 band
    void engineDryDelayLatencyTest         (double fs);  // #6 dry/latency + contract pin
    void engineDetectorFloorTest           (double fs);  // #7
    void engineDeterminismTest             (double fs);  // #8a
    void engineParamNanGuardTest           (double fs);  // #8b
    void engineNanRecoveryTest             (double fs);  // #9a
    void engineResetResidueTest            (double fs);  // #9b (+ state-reset-on-prepare)
    void engineChunkInvarianceTest         (double fs);  // #9 (determinism derivative, static params)

    void checkAliasCrossRateSpread();

    // ======================================================================
    // §4 primitive tests (HistoryBuffer standalone)
    // ======================================================================

    // #5 primitive (§4.1): ramp identity readAtAge(0,A) == K-A exercises BOTH
    // age-exactness (integer A) and linear-interpolation exactness (fractional
    // A) with one formula, per the plan's "ramp identity" derivation.
    void historyBufferInterpExactnessTest (double fs)
    {
        fc::HistoryBuffer hb;
        hb.prepare (fs, 2, 0.05);
        const int capacity = hb.capacitySamples();
        const int K = capacity - 16;

        for (int k = 0; k < K; ++k)
        {
            hb.write (0, (double) k);
            hb.write (1, -(double) k);
            hb.advance();
        }

        // 1. Age-exactness (integer ages) -- exact equality (float-lossless ramp).
        const double intAges[] = { 1.0, 2.0, 17.0, (double) (K / 2), (double) (K - 1) };
        for (double A : intAges)
        {
            const double got  = hb.readAtAge (0, A);
            const double want = (double) K - A;
            if (got != want)
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs)
                    + ": age-exact A=" + std::to_string (A) + " got=" + std::to_string (got)
                    + " want=" + std::to_string (want));
        }

        // 2. Fractional ages -- linear interpolation, 1e-9 floor.
        const double fracAges[] = { 1.5, 2.25, 3.75, (double) K - 1.5 };
        for (double A : fracAges)
        {
            const double got  = hb.readAtAge (0, A);
            const double want = (double) K - A;
            if (std::abs (got - want) > 1.0e-9)
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs)
                    + ": frac A=" + std::to_string (A) + " got=" + std::to_string (got)
                    + " want=" + std::to_string (want));
        }

        // 3. zero-before-write on a fresh buffer.
        {
            fc::HistoryBuffer hb2;
            hb2.prepare (fs, 2, 0.05);
            const double got = hb2.readAtAge (0, 5.0);
            if (got != 0.0)
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs) + ": zero-before-write got=" + std::to_string (got));
        }

        // 4. Saturation clamp (upper): age beyond capacity-2 saturates, no wrap.
        {
            const double a1 = hb.readAtAge (0, (double) (capacity + 100));
            const double a2 = hb.readAtAge (0, (double) (capacity - 2));
            if (a1 != a2 || ! std::isfinite (a1))
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs)
                    + ": upper clamp mismatch a1=" + std::to_string (a1) + " a2=" + std::to_string (a2));
        }

        // 5. Clamp (lower): ages <= 1 all clamp to age=1.
        {
            const double a0   = hb.readAtAge (0, 0.0);
            const double a05  = hb.readAtAge (0, 0.5);
            const double a1   = hb.readAtAge (0, 1.0);
            if (a0 != a1 || a05 != a1)
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs)
                    + ": lower clamp mismatch a0=" + std::to_string (a0) + " a05=" + std::to_string (a05) + " a1=" + std::to_string (a1));
        }

        // 6. Channel independence.
        {
            const double got  = hb.readAtAge (1, 17.0);
            const double want = -((double) K - 17.0);
            if (got != want)
                fail ("historyBufferInterpExactnessTest @Fs=" + std::to_string (fs)
                    + ": channel independence got=" + std::to_string (got) + " want=" + std::to_string (want));
        }
    }

    // #5/#9 primitive (§4.2): write() non-finite guard + wrap-around
    // worst-case (class D) + reset + determinism, all at exact (0) tolerance.
    void historyBufferWrapGuardResetTest (double fs)
    {
        // 1. write() non-finite guard.
        {
            fc::HistoryBuffer hbg;
            hbg.prepare (fs, 2, 0.02);
            const int cap2 = hbg.capacitySamples();
            const int M = std::min (cap2 - 4, 50);
            std::vector<double> written ((size_t) M);
            for (int k = 0; k < M; ++k)
            {
                double v = (double) k;
                if (k == 5)       v = std::numeric_limits<double>::quiet_NaN();
                else if (k == 10) v = std::numeric_limits<double>::infinity();
                else if (k == 15) v = -std::numeric_limits<double>::infinity();
                written[(size_t) k] = std::isfinite (v) ? v : 0.0;
                hbg.write (0, v);
                hbg.advance();
            }
            for (int k = 0; k < M; ++k)
            {
                const double age = (double) (M - k);
                const double got = hbg.readAtAge (0, age);
                if (got != written[(size_t) k])
                    fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs)
                        + ": non-finite guard k=" + std::to_string (k) + " got=" + std::to_string (got)
                        + " want=" + std::to_string (written[(size_t) k]));
            }
        }

        // 2. capacity wrap / worst-case (class D): M = capacity + capacity/2.
        {
            fc::HistoryBuffer hbw;
            hbw.prepare (fs, 2, 0.02);
            const long long capacity = hbw.capacitySamples();
            const long long M = capacity + capacity / 2;
            for (long long k = 0; k < M; ++k)
            {
                hbw.write (0, (double) k);
                hbw.advance();
            }
            const int numPoints = 32;
            for (int p = 0; p < numPoints; ++p)
            {
                long long A = 1 + (long long) std::llround ((double) p * (double) (capacity - 3) / (double) (numPoints - 1));
                A = std::clamp<long long> (A, 1, capacity - 2);
                const double got  = hbw.readAtAge (0, (double) A);
                const double want = (double) (M - A);
                if (got != want)
                    fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs)
                        + ": wrap A=" + std::to_string (A) + " got=" + std::to_string (got) + " want=" + std::to_string (want));
                if (! std::isfinite (got))
                    fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs) + ": wrap non-finite at A=" + std::to_string (A));
            }

            // 3. reset(): full clear, capacity unchanged.
            hbw.reset();
            if (hbw.capacitySamples() != capacity)
                fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs) + ": capacity changed by reset()");
            for (long long A = 1; A <= capacity - 2; A += std::max<long long> (1, capacity / 37))
            {
                const double got = hbw.readAtAge (0, (double) A);
                if (got != 0.0)
                    fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs) + ": reset residue at A=" + std::to_string (A) + " got=" + std::to_string (got));
            }
        }

        // 4. Determinism: identical write/advance sequences from fresh
        //    prepare are bit-identical.
        {
            fc::HistoryBuffer hbA, hbB;
            hbA.prepare (fs, 2, 0.02);
            hbB.prepare (fs, 2, 0.02);
            const long long capacity = hbA.capacitySamples();
            const long long M = capacity + capacity / 2;
            for (long long k = 0; k < M; ++k)
            {
                hbA.write (0, (double) k); hbA.write (1, -(double) k); hbA.advance();
                hbB.write (0, (double) k); hbB.write (1, -(double) k); hbB.advance();
            }
            for (long long A = 1; A <= capacity - 2; A += std::max<long long> (1, capacity / 41))
            {
                const double ga = hbA.readAtAge (0, (double) A);
                const double gb = hbB.readAtAge (0, (double) A);
                if (ga != gb)
                    fail ("historyBufferWrapGuardResetTest @Fs=" + std::to_string (fs) + ": determinism mismatch at A=" + std::to_string (A));
            }
        }
    }

    // ======================================================================
    // §5 engine tests (OmoideEcho)
    // ======================================================================
    // #1 (§5.1): integer echo schedule + amplitude ratio (z-domain).
    void engineEchoScheduleRatioTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);

        // ---- (a) time: impulse -> echo onsets ------------------------------
        {
            fc::OmoideEcho eng;
            eng.setDelayMs (500.0);
            eng.setRegen01 (0.6);
            eng.setToneHz (6000.0);
            eng.setScanLevel01 (0.0);
            eng.setMix01 (1.0);
            eng.prepare (fs, 2);
            const int L = eng.latencySamples();

            const long long Nd = fc::OmoideEcho::delaySamplesForMs (500.0); // 12000
            const long long intervalHost = (long long) std::llround ((double) Nd * fs / fc::OmoideEcho::kInternalRateHz); // 0.5*fs

            const long long total = (long long) std::llround (3.0 * fs) + L;
            std::vector<float> bufL ((size_t) total, 0.0f), bufR ((size_t) total, 0.0f);
            bufL[0] = 1.0f; bufR[0] = 1.0f;
            processFixedChunk (eng, bufL, bufR, 509);

            auto findPeakNear = [&] (long long center, long long halfWin) -> long long
            {
                const long long lo = std::max<long long> (0, center - halfWin);
                const long long hi = std::min<long long> (total - 1, center + halfWin);
                long long bestIdx = lo;
                double bestVal = -1.0;
                for (long long i = lo; i <= hi; ++i)
                {
                    const double v = std::abs ((double) bufL[(size_t) i]);
                    if (v > bestVal) { bestVal = v; bestIdx = i; }
                }
                return bestIdx;
            };

            long long p[4] = { 0, 0, 0, 0 };
            for (int k = 1; k <= 3; ++k)
            {
                const long long expected = (long long) L + (long long) k * intervalHost;
                p[k] = findPeakNear (expected, 25);
                if (std::llabs (p[k] - expected) > 2)
                    fail ("engineEchoScheduleRatioTest" + tag + ": echo" + std::to_string (k)
                        + " onset=" + std::to_string (p[k]) + " expected=" + std::to_string (expected));
            }
            if (std::llabs ((p[2] - p[1]) - intervalHost) > 2)
                fail ("engineEchoScheduleRatioTest" + tag + ": interval1-2 mismatch " + std::to_string (p[2] - p[1]) + " vs " + std::to_string (intervalHost));
            if (std::llabs ((p[3] - p[2]) - intervalHost) > 2)
                fail ("engineEchoScheduleRatioTest" + tag + ": interval2-3 mismatch " + std::to_string (p[3] - p[2]) + " vs " + std::to_string (intervalHost));

            // Echo1 peak vs the down-decimator's kernel peak (NOT unity): a
            // broadband unit impulse does not retain unit PEAK through the
            // internal down-resample -- VariPolyphaseResampler's index-stretch
            // ideal-lowpass kernel has impulse-response peak = 2*fc (the
            // standard sinc-kernel result), with fc = kFc1 * min(1, outRate /
            // inRate) tracking the down stage's own effective cutoff (see
            // VariPolyphaseResampler.h's index-stretch contract: "effective
            // cutoff = kFc1 / stretch" and OmoideEcho.h's down-stage ratio
            // outRate/inRate = kInternalRateHz/fs). This is transcribed
            // independently from the PUBLISHED design constants (kFc1,
            // kInternalRateHz) and the standard ideal-LP impulse-peak formula
            // -- never from calling into the engine's own process path. A
            // passband tone (2000 Hz, checked just below) still passes the
            // bracket at unity gain, confirming this is correct bandlimiter
            // behaviour (broadband impulse energy spread by the kernel), not
            // a gain error.
            const double kernelPeak = 2.0 * fc::VariPolyphaseResampler::kFc1
                                     * std::min (1.0, fc::OmoideEcho::kInternalRateHz / fs);
            const double peak1 = std::abs ((double) bufL[(size_t) p[1]]);
            if (peak1 < 0.85 * kernelPeak || peak1 > 1.10 * kernelPeak)
                fail ("engineEchoScheduleRatioTest" + tag + ": echo1 peak=" + std::to_string (peak1)
                    + " kernelPeak=" + std::to_string (kernelPeak) + " out of [0.85,1.10]*kernelPeak");
        }

        // ---- (b) amplitude ratio: tone burst + Goertzel (z-domain) --------
        {
            const double f = 2000.0, A = 0.05;
            const long long burstLen = (long long) std::llround (fs * 0.1);
            const long long N60 = (long long) std::llround (fs * 0.01) * 6;
            const long long margin20ms = (long long) std::llround (fs * 0.02);

            auto runRatio = [&] (double regen01) -> std::array<double, 3>
            {
                fc::OmoideEcho eng;
                eng.setDelayMs (500.0);
                eng.setRegen01 (regen01);
                eng.setToneHz (6000.0);
                eng.setScanLevel01 (0.0);
                eng.setMix01 (1.0);
                eng.prepare (fs, 2);
                const int L = eng.latencySamples();
                const long long Nd = fc::OmoideEcho::delaySamplesForMs (500.0);
                const long long intervalHost = (long long) std::llround ((double) Nd * fs / fc::OmoideEcho::kInternalRateHz);

                const long long total = (long long) std::llround (3.0 * fs) + L;
                std::vector<float> bufL ((size_t) total, 0.0f), bufR ((size_t) total, 0.0f);
                for (long long n = 0; n < burstLen; ++n)
                {
                    const double s = A * std::sin (2.0 * kPi * f * (double) n / fs);
                    bufL[(size_t) n] = (float) s; bufR[(size_t) n] = (float) s;
                }
                processFixedChunk (eng, bufL, bufR, 509);

                const std::vector<double> outL = toDoubleVec (bufL);
                std::array<double, 3> mags {};
                for (int k = 1; k <= 3; ++k)
                {
                    const long long onset = (long long) L + (long long) k * intervalHost;
                    const long long windowStart = onset + margin20ms;
                    mags[(size_t) (k - 1)] = goertzelMag (outL, (size_t) windowStart, (size_t) N60, f, fs);
                }
                return mags;
            };

            const std::array<double, 3> mags = runRatio (0.6);
            const double ratio21 = mags[1] / mags[0];
            const double ratio32 = mags[2] / mags[1];
            const double expectedRatio = fc::OmoideEcho::kMaxRegen * 0.6 * onePoleMag (2000.0, 6000.0, fc::OmoideEcho::kInternalRateHz);

            if (std::abs (ratio21 - expectedRatio) > 0.03 * expectedRatio)
                fail ("engineEchoScheduleRatioTest" + tag + ": mag2/mag1=" + std::to_string (ratio21) + " expected=" + std::to_string (expectedRatio));
            if (std::abs (ratio32 - expectedRatio) > 0.03 * expectedRatio)
                fail ("engineEchoScheduleRatioTest" + tag + ": mag3/mag2=" + std::to_string (ratio32) + " expected=" + std::to_string (expectedRatio));
            if (! (ratio21 < 1.0))
                fail ("engineEchoScheduleRatioTest" + tag + ": ratio21 not < 1 (" + std::to_string (ratio21) + ")");

            const std::array<double, 3> magsZeroRegen = runRatio (0.0);
            if (magsZeroRegen[1] > 1.0e-6)
                fail ("engineEchoScheduleRatioTest" + tag + ": regen=0 mag2=" + std::to_string (magsZeroRegen[1]) + " should be ~0");
        }
    }
    // #2a (§5.2): feedback loop-gain stability at the WORST-CASE setting
    // (class A hard rule): regen=1 (effective 0.95), tone=11000 (max in-range,
    // least attenuation), mix=1 (pure-echo, no dry passthrough -- R10), scan
    // excluded from feedback (scanLevel=0), delay at its MINIMUM (100 ms) so
    // several echoes land inside window0 (impulseResponseNonIncreasing always
    // compares against window0 -- see the helper's own contract).
    void engineLoopGainStabilityTest (double fs)
    {
        fc::OmoideEcho eng;
        eng.setRegen01 (1.0);
        eng.setToneHz (11000.0);
        eng.setMix01 (1.0);
        eng.setScanLevel01 (0.0);
        eng.setDelayMs (100.0);
        eng.prepare (fs, 2);

        auto perSample = [&] (double x) -> double
        {
            float l = (float) x, r = (float) x;
            float* ptrs[2] = { &l, &r };
            eng.process (ptrs, 2, 1);
            return 0.5 * ((double) l + (double) r);
        };

        const bool ok = fct::impulseResponseNonIncreasing (perSample, fs, /*tail*/ 4.0, /*win*/ 0.5, /*tol*/ 1.05);
        if (! ok)
            fail ("engineLoopGainStabilityTest @Fs=" + std::to_string (fs) + ": impulse response energy grew window-over-window (loop gain >= 1 at worst-case setting)");
    }

    // #2b (§5.3): long-hold (8 s) realistic peak bound with scan sweeping
    // throughout (the condition the header's peak-bound argument assumes),
    // at the worst setting: regen=1, tone=11000, scanLevel=1, mix=1, delay=500ms.
    //
    // STATUS: RED, pending a human/orchestrator decision (do NOT touch this
    // oracle/tolerance). Measured worst-case peak is ~2.45-2.99 > kPeakBound
    // = 2.0 across the rate matrix; the header's own peak-bound argument
    // already flags a pathological instantaneous-coincidence ceiling of
    // ~3.27 (echo+scan tanh-ceiling superposition) and explicitly instructs
    // "escalate (Ask a human), NOT loosen" if the long-hold gate ever
    // measures above 2.0. Left as-is until that decision (kPeakBound raised,
    // or wet gain constrained) is made.
    void engineLongHoldPeakTest (double fs)
    {
        fc::OmoideEcho eng;
        eng.setRegen01 (1.0);
        eng.setToneHz (11000.0);
        eng.setScanLevel01 (1.0);
        eng.setMix01 (1.0);
        eng.setDelayMs (500.0);
        eng.prepare (fs, 2);

        const long long N8 = (long long) std::llround (8.0 * fs);
        const long long stepSamples = std::max<long long> (1, (long long) std::llround (0.05 * fs));

        Lcg lcgL (0xC0FFEEULL), lcgR (0xF00DULL);
        bool finiteOk = true;
        double runningPeak = 0.0;

        long long offset = 0;
        long long nextBoundary = 0;
        size_t primeIdx = 0;
        while (offset < N8)
        {
            if (offset >= nextBoundary)
            {
                const double t = (double) nextBoundary / fs;
                const double phase = std::fmod (t, 1.0);
                const double tri = 1.0 - std::abs (2.0 * phase - 1.0); // triangle, period 1s, range [0,1]
                eng.setScan01 (tri);
                nextBoundary += stepSamples;
            }
            const int chunk = (int) std::min<long long> (kPrimeChunks[primeIdx % kNumPrimeChunks], N8 - offset);
            std::vector<float> bl ((size_t) chunk), br ((size_t) chunk);
            for (int i = 0; i < chunk; ++i) { bl[(size_t) i] = (float) lcgL.next(); br[(size_t) i] = (float) lcgR.next(); }
            float* ptrs[2] = { bl.data(), br.data() };
            eng.process (ptrs, 2, chunk);
            for (int i = 0; i < chunk; ++i)
            {
                if (! std::isfinite (bl[(size_t) i]) || ! std::isfinite (br[(size_t) i])) finiteOk = false;
                runningPeak = std::max ({ runningPeak, (double) std::fabs (bl[(size_t) i]), (double) std::fabs (br[(size_t) i]) });
            }
            offset += chunk;
            ++primeIdx;
        }

        if (! finiteOk)
            fail ("engineLongHoldPeakTest @Fs=" + std::to_string (fs) + ": non-finite sample during 8s long-hold");
        if (runningPeak > fc::OmoideEcho::kPeakBound)
            fail ("engineLongHoldPeakTest @Fs=" + std::to_string (fs) + ": peak=" + std::to_string (runningPeak)
                + " exceeds kPeakBound=" + std::to_string (fc::OmoideEcho::kPeakBound)
                + " -- header instructs escalate (Ask a human), NOT loosen");

        // Post-hold sanity: drain to silence, verify no residual oscillation/stack.
        eng.setRegen01 (0.0);
        eng.setScanLevel01 (0.0);
        eng.setMix01 (1.0);
        const long long N2 = (long long) std::llround (2.0 * fs);
        std::vector<float> silL ((size_t) N2, 0.0f), silR ((size_t) N2, 0.0f);
        processPrimeChunks (eng, silL, silR);

        const long long tailLen = (long long) std::llround (0.2 * fs);
        const long long tailStart = N2 - tailLen;
        double tailPeak = 0.0;
        for (long long i = tailStart; i < N2; ++i)
            tailPeak = std::max ({ tailPeak, (double) std::fabs (silL[(size_t) i]), (double) std::fabs (silR[(size_t) i]) });
        if (tailPeak > 1.0e-3)
            fail ("engineLongHoldPeakTest @Fs=" + std::to_string (fs) + ": post-hold tail peak=" + std::to_string (tailPeak) + " > 1e-3 (echo/bracket not draining)");
    }
    // #3 (§5.4): SCAN age accuracy, ±5 ms (spec value). age=2.0s substitutes
    // for the full 119s (R3 reset-snap means age is exact from t=0, no glide
    // settle needed).
    void engineScanAgeAccuracyTest (double fs)
    {
        const double v = 2.0 / 119.0; // scanAgeSecondsForScan01(v) == 2.0
        const double markerA = 0.5, markerF = 1000.0;
        const long long n0 = (long long) std::llround (0.5 * fs);
        const long long markerLen = (long long) std::llround (0.02 * fs); // 20ms

        auto buildAndRun = [&] (double scanLevel01) -> std::vector<double>
        {
            fc::OmoideEcho eng;
            eng.setScan01 (v);
            eng.setScanLevel01 (scanLevel01);
            eng.setRegen01 (0.0);
            eng.setToneHz (6000.0);
            eng.setMix01 (1.0);
            eng.setDelayMs (100.0);
            eng.prepare (fs, 2);
            const int L = eng.latencySamples();

            const long long total = n0 + (long long) std::llround (2.0 * fs) + (long long) L + (long long) std::llround (0.2 * fs);
            std::vector<float> bufL ((size_t) total, 0.0f), bufR ((size_t) total, 0.0f);
            for (long long n = 0; n < markerLen; ++n)
            {
                const double s = markerA * std::sin (2.0 * kPi * markerF * (double) n / fs);
                bufL[(size_t) (n0 + n)] = (float) s;
                bufR[(size_t) (n0 + n)] = (float) s;
            }
            processFixedChunk (eng, bufL, bufR, 509);
            return toDoubleVec (bufL);
        };

        fc::OmoideEcho probe;
        probe.setDelayMs (100.0);
        probe.prepare (fs, 2);
        const int L = probe.latencySamples();

        std::vector<double> tmpl ((size_t) markerLen);
        for (long long n = 0; n < markerLen; ++n)
            tmpl[(size_t) n] = markerA * std::sin (2.0 * kPi * markerF * (double) n / fs);

        const long long expected = n0 + (long long) std::llround (2.0 * fs) + (long long) L;
        const long long margin8ms = (long long) std::llround (0.008 * fs);

        const std::vector<double> outOn = buildAndRun (1.0);
        const long bestLag = xcorrArgmax (outOn, tmpl, (long) (expected - margin8ms), (long) (expected + margin8ms));
        const long long tol5ms = (long long) std::llround (0.005 * fs);
        if (std::llabs ((long long) bestLag - expected) > tol5ms)
            fail ("engineScanAgeAccuracyTest @Fs=" + std::to_string (fs) + ": scan replay lag=" + std::to_string (bestLag)
                + " expected=" + std::to_string (expected) + " tol=" + std::to_string (tol5ms));

        // scanLevel=0 control run: no scan playback near the expected location.
        const std::vector<double> outOff = buildAndRun (0.0);
        double peakOff = 0.0;
        for (long long i = std::max<long long> (0, expected - margin8ms); i <= expected + margin8ms && i < (long long) outOff.size(); ++i)
            peakOff = std::max (peakOff, std::abs (outOff[(size_t) i]));
        if (peakOff > 1.0e-6)
            fail ("engineScanAgeAccuracyTest @Fs=" + std::to_string (fs) + ": scanLevel=0 residual near scan onset=" + std::to_string (peakOff));
    }

    // #4 (§5.5): SCAN glide Doppler, one-pole age trajectory. echoTap isolated
    // via delay=10s (never reached within the test's run length, so echoTap
    // stays at its zero-before-write floor -- output is scanTap only).
    double runDopplerCrossings (double fs, double v0, double v1, double f, double A,
                                double tFill, double tStep, double hyst, long long& outNStep)
    {
        fc::OmoideEcho eng;
        eng.setDelayMs (10000.0);
        eng.setRegen01 (0.0);
        eng.setScanLevel01 (1.0);
        eng.setMix01 (1.0);
        eng.setToneHz (11000.0);
        eng.setScan01 (v0);
        eng.prepare (fs, 2);

        const long long nFill = (long long) std::llround (tFill * fs);
        const long long nStep = (long long) std::llround (tStep * fs);
        outNStep = nStep;

        std::vector<float> fillL ((size_t) nFill), fillR ((size_t) nFill);
        for (long long n = 0; n < nFill; ++n)
        {
            const double s = A * std::sin (2.0 * kPi * f * (double) n / fs);
            fillL[(size_t) n] = (float) s; fillR[(size_t) n] = (float) s;
        }
        processFixedChunk (eng, fillL, fillR, 509);

        eng.setScan01 (v1);

        std::vector<float> stepL ((size_t) nStep), stepR ((size_t) nStep);
        for (long long n = 0; n < nStep; ++n)
        {
            const double tGlobal = (double) (nFill + n) / fs;
            const double s = A * std::sin (2.0 * kPi * f * tGlobal);
            stepL[(size_t) n] = (float) s; stepR[(size_t) n] = (float) s;
        }
        processFixedChunk (eng, stepL, stepR, 509);

        const std::vector<double> stepLd = toDoubleVec (stepL);
        return (double) countPosCrossings (stepLd, 0, (size_t) nStep, hyst);
    }

    void engineScanDopplerGlideTest (double fs)
    {
        const double f = 2000.0, A = 0.5;
        const double v0 = 1.0 / 119.0, v1 = 1.1 / 119.0; // age0=1.0s, age1=1.1s
        const double T = 1.5, tFill = 1.2;
        const double tau = fc::OmoideEcho::kScanGlideTauMs * 1.0e-3; // 0.25s
        const double deltaA = 0.1;
        const double expectedCycles = f * (T - deltaA * (1.0 - std::exp (-T / tau)));

        long long nStep = 0;
        const double crossings = runDopplerCrossings (fs, v0, v1, f, A, tFill, T, 0.05 * A, nStep);

        if (std::abs (crossings - expectedCycles) > 0.03 * expectedCycles)
            fail ("engineScanDopplerGlideTest @Fs=" + std::to_string (fs) + ": crossings=" + std::to_string (crossings)
                + " expected=" + std::to_string (expectedCycles));
        if (! (crossings < f * T))
            fail ("engineScanDopplerGlideTest @Fs=" + std::to_string (fs) + ": deepening step did not reduce crossings below f*T");

        // Auxiliary (loose) instantaneous-frequency check in the first 50ms after the step.
        {
            fc::OmoideEcho eng;
            eng.setDelayMs (10000.0);
            eng.setRegen01 (0.0);
            eng.setScanLevel01 (1.0);
            eng.setMix01 (1.0);
            eng.setToneHz (11000.0);
            eng.setScan01 (v0);
            eng.prepare (fs, 2);
            const long long nFill = (long long) std::llround (tFill * fs);
            std::vector<float> fillL ((size_t) nFill), fillR ((size_t) nFill);
            for (long long n = 0; n < nFill; ++n)
            {
                const double s = A * std::sin (2.0 * kPi * f * (double) n / fs);
                fillL[(size_t) n] = (float) s; fillR[(size_t) n] = (float) s;
            }
            processFixedChunk (eng, fillL, fillR, 509);
            eng.setScan01 (v1);
            const long long n50 = (long long) std::llround (0.05 * fs);
            std::vector<float> stepL ((size_t) n50), stepR ((size_t) n50);
            for (long long n = 0; n < n50; ++n)
            {
                const double tGlobal = (double) (nFill + n) / fs;
                const double s = A * std::sin (2.0 * kPi * f * tGlobal);
                stepL[(size_t) n] = (float) s; stepR[(size_t) n] = (float) s;
            }
            processFixedChunk (eng, stepL, stepR, 509);
            const std::vector<double> d = toDoubleVec (stepL);
            const int c50 = countPosCrossings (d, 0, (size_t) n50, 0.05 * A);
            const double freqEst = (double) c50 / 0.05;
            const double expectedInstFreq = f * (1.0 - deltaA / tau);
            if (std::abs (freqEst - expectedInstFreq) > 0.10 * expectedInstFreq)
                fail ("engineScanDopplerGlideTest @Fs=" + std::to_string (fs) + ": instFreq=" + std::to_string (freqEst)
                    + " expected~" + std::to_string (expectedInstFreq) + " (auxiliary loose gate)");
        }

        // Form-independent invariant: shallowing step (v1<v0) increases crossings above f*T.
        {
            long long nStepRev = 0;
            const double crossingsRev = runDopplerCrossings (fs, v1, v0, f, A, tFill, T, 0.05 * A, nStepRev);
            if (! (crossingsRev > f * T))
                fail ("engineScanDopplerGlideTest @Fs=" + std::to_string (fs) + ": shallowing step did not increase crossings above f*T (got " + std::to_string (crossingsRev) + ")");
        }
    }
    // #5 (§5.6): 120 s worst-case history capacity, ALL 6 rates (no
    // reduction -- plan §0.3-3/§9 explicitly reconfirms full matrix).
    // Streamed (no full-output storage): running finite/peak over the whole
    // 120.2s run, plus two small captured windows (~60s zero-before-write
    // diff, ~119.5s marker replay).
    void engineHistoryCapacityLongRunTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);
        const double markerA = 0.3, markerF = 1000.0, noiseA = 0.02;
        const long long n0 = (long long) std::llround (0.5 * fs);
        const long long markerLen = (long long) std::llround (0.04 * fs); // 40ms

        // Self-contained stateful emitter: captured Lcg objects by value,
        // advanced exactly once per absolute sample index in strictly
        // increasing order (guaranteed by streamRun's call pattern), so two
        // separately-constructed Emitters with identical seeds produce
        // identical sequences over their overlapping prefix.
        struct Emitter
        {
            Lcg lcgL, lcgR;
            long long n0, markerLen;
            double markerA, markerF, noiseA, fs;
            void operator() (long long idx, float& l, float& r)
            {
                if (idx >= n0 && idx < n0 + markerLen)
                {
                    const double t = (double) (idx - n0) / fs;
                    const double s = markerA * std::sin (2.0 * kPi * markerF * t);
                    l = (float) s; r = (float) s;
                }
                else
                {
                    l = (float) (noiseA * lcgL.next());
                    r = (float) (noiseA * lcgR.next());
                }
            }
        };

        // ---- Main run: scanLevel=1 throughout, full 120.2s -----------------
        fc::OmoideEcho engMain;
        engMain.setScan01 (1.0); // age = 119s (< capacity-2 -- no saturation, per HistoryBuffer contract)
        engMain.setScanLevel01 (1.0);
        engMain.setRegen01 (0.0);
        engMain.setDelayMs (100.0);
        engMain.setToneHz (6000.0);
        engMain.setMix01 (1.0);
        engMain.prepare (fs, 2);
        const int L = engMain.latencySamples();

        const long long totalMain = (long long) std::llround (120.0 * fs) + (long long) std::llround (0.2 * fs);
        const long long expectedReplay = n0 + (long long) std::llround (119.0 * fs) + (long long) L;
        const long long margin8ms = (long long) std::llround (0.008 * fs);
        const long long replayWinLo = std::max<long long> (0, expectedReplay - margin8ms);
        const long long replayWinHi = expectedReplay + margin8ms + markerLen;

        const long long win60Center = (long long) std::llround (60.0 * fs);
        const long long win60Half = (long long) std::llround (0.05 * fs);
        const long long win60Lo = win60Center - win60Half;
        const long long win60Hi = win60Center + win60Half;

        std::vector<double> replayCapture, win60OnCapture;
        replayCapture.assign ((size_t) (replayWinHi - replayWinLo), 0.0);
        win60OnCapture.assign ((size_t) (win60Hi - win60Lo), 0.0);

        bool finiteOk = true;
        double runningPeak = 0.0;

        Emitter emitMain { Lcg (0xA5A5A5A5ULL), Lcg (0x5A5A5A5AULL), n0, markerLen, markerA, markerF, noiseA, fs };
        streamRun (engMain, totalMain, 2048, emitMain,
            [&] (long long offset, const float* l, const float*, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    const long long idx = offset + i;
                    if (idx >= replayWinLo && idx < replayWinHi)
                        replayCapture[(size_t) (idx - replayWinLo)] = (double) l[i];
                    if (idx >= win60Lo && idx < win60Hi)
                        win60OnCapture[(size_t) (idx - win60Lo)] = (double) l[i];
                }
            },
            finiteOk, runningPeak);

        if (! finiteOk) fail ("engineHistoryCapacityLongRunTest" + tag + ": non-finite sample during 120s run");
        if (runningPeak > fc::OmoideEcho::kPeakBound)
            fail ("engineHistoryCapacityLongRunTest" + tag + ": peak=" + std::to_string (runningPeak) + " exceeds kPeakBound=2.0");

        // Marker replay at ~119.5s (zero-extra-offset contract, OFF=0).
        std::vector<double> tmpl ((size_t) markerLen);
        for (long long n = 0; n < markerLen; ++n)
            tmpl[(size_t) n] = markerA * std::sin (2.0 * kPi * markerF * (double) n / fs);
        const long bestLag = xcorrArgmax (replayCapture, tmpl, 0, (long) (replayWinHi - replayWinLo - markerLen));
        const long long tol5ms = (long long) std::llround (0.005 * fs);
        const long long gotAbs = replayWinLo + bestLag;
        if (std::llabs (gotAbs - expectedReplay) > tol5ms)
            fail ("engineHistoryCapacityLongRunTest" + tag + ": marker replay at=" + std::to_string (gotAbs) + " expected=" + std::to_string (expectedReplay));

        // ---- Control run: scanLevel=0, only up to just past t=60s ----------
        fc::OmoideEcho engOff;
        engOff.setScan01 (1.0);
        engOff.setScanLevel01 (0.0);
        engOff.setRegen01 (0.0);
        engOff.setDelayMs (100.0);
        engOff.setToneHz (6000.0);
        engOff.setMix01 (1.0);
        engOff.prepare (fs, 2);

        const long long totalOff = win60Hi + 1;
        std::vector<double> win60OffCapture ((size_t) (win60Hi - win60Lo), 0.0);
        bool finiteOkOff = true;
        double peakOff = 0.0;
        Emitter emitOff { Lcg (0xA5A5A5A5ULL), Lcg (0x5A5A5A5AULL), n0, markerLen, markerA, markerF, noiseA, fs };
        streamRun (engOff, totalOff, 2048, emitOff,
            [&] (long long offset, const float* l, const float*, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    const long long idx = offset + i;
                    if (idx >= win60Lo && idx < win60Hi)
                        win60OffCapture[(size_t) (idx - win60Lo)] = (double) l[i];
                }
            },
            finiteOkOff, peakOff);

        double maxDiff = 0.0;
        for (size_t i = 0; i < win60OnCapture.size(); ++i)
            maxDiff = std::max (maxDiff, std::abs (win60OnCapture[i] - win60OffCapture[i]));
        if (maxDiff > 1.0e-6)
            fail ("engineHistoryCapacityLongRunTest" + tag + ": zero-before-write diff at t=60s=" + std::to_string (maxDiff) + " > 1e-6");
    }
    // #6 band (§5.7): passband (2000Hz, +/-0.75dB) + alias floor (14000Hz in
    // -> 10000Hz alias product, <= -40dB, ALL 6 rates per the index-stretch
    // rework -- R5). Combined single input (sum of two integer-period tones)
    // exploits DFT-bin orthogonality (both 2000 and 14000 have an integer
    // number of cycles over the 1s window) so one engine run yields all
    // probes; regen=0/scanLevel=0 makes the whole chain linear (no
    // intermodulation), so superposition of probes is valid.
    void engineBracketBandLimitTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);
        fc::OmoideEcho eng;
        eng.setRegen01 (0.0);
        eng.setScanLevel01 (0.0);
        eng.setMix01 (1.0);
        eng.setDelayMs (100.0);
        eng.setToneHz (11000.0);
        eng.prepare (fs, 2);
        const int L = eng.latencySamples();

        const double A = 0.5;
        const long long skip = (long long) L + (long long) std::llround (0.1 * fs);
        const long long N = (long long) std::llround (1.0 * fs); // 1.0s integer-period window
        const long long total = skip + N;

        std::vector<float> bufL ((size_t) total), bufR ((size_t) total);
        for (long long n = 0; n < total; ++n)
        {
            const double t = (double) n / fs;
            const double s = A * std::sin (2.0 * kPi * 2000.0 * t) + A * std::sin (2.0 * kPi * 14000.0 * t);
            bufL[(size_t) n] = (float) s; bufR[(size_t) n] = (float) s;
        }
        processFixedChunk (eng, bufL, bufR, 509);
        const std::vector<double> outL = toDoubleVec (bufL);

        const double magPass  = goertzelMag (outL, (size_t) skip, (size_t) N, 2000.0, fs);
        const double magAlias = goertzelMag (outL, (size_t) skip, (size_t) N, 10000.0, fs);
        const double magDirect14k = goertzelMag (outL, (size_t) skip, (size_t) N, 14000.0, fs); // secondary/non-gating

        const double passDb  = 20.0 * std::log10 (std::max (magPass, 1.0e-300) / A);
        const double aliasDb = 20.0 * std::log10 (std::max (magAlias, 1.0e-300) / A);

        if (std::abs (passDb) > 0.75)
            fail ("engineBracketBandLimitTest" + tag + ": passband@2000Hz=" + std::to_string (passDb) + "dB exceeds +/-0.75dB");
        if (aliasDb > -40.0)
            fail ("engineBracketBandLimitTest" + tag + ": alias@10000Hz(from 14000Hz in)=" + std::to_string (aliasDb) + "dB exceeds -40dB gate");
        if (! (magAlias < magPass))
            fail ("engineBracketBandLimitTest" + tag + ": alias magnitude not < passband magnitude (form-independent invariant)");

        g_aliasDbByRate.push_back (aliasDb);

        // Non-gating design cross-check (diagnostic only -- see designDownGainAt).
        const double designGain12k = designDownGainAt (12000.0, fs, fc::OmoideEcho::kInternalRateHz);
        const double designGain14k = designDownGainAt (14000.0, fs, fc::OmoideEcho::kInternalRateHz);
        std::fprintf (stdout, "  [diag] omoide #6%s: passDb=%.4f aliasDb=%.4f directLeak14kDb=%.4f designGain@12k=%.2fdB designGain@14k=%.2fdB\n",
                       tag.c_str(), passDb, aliasDb, 20.0 * std::log10 (std::max (magDirect14k, 1.0e-300) / A),
                       20.0 * std::log10 (std::max (designGain12k, 1.0e-300)), 20.0 * std::log10 (std::max (designGain14k, 1.0e-300)));
    }

    void checkAliasCrossRateSpread()
    {
        if (g_aliasDbByRate.size() < 2) return; // only meaningful for the full-matrix invocation
        double lo = g_aliasDbByRate[0], hi = g_aliasDbByRate[0];
        for (double v : g_aliasDbByRate) { lo = std::min (lo, v); hi = std::max (hi, v); }
        if (hi - lo > 6.0)
            fail ("checkAliasCrossRateSpread: alias dB spread across rates = " + std::to_string (hi - lo)
                + " dB (> 6dB) -- possible sign that index-stretch is not fsHost-independent at some rate");
    }
    // #6 dry/latency + contract pin (§5.8): quadruple-cross-check of L, then
    // mix=0 pure-delay identity (stereo + mono), then mix=1 wet-landing xcorr.
    void engineDryDelayLatencyTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);

        // ---- (0) contract pin: L quadruple cross-check ---------------------
        static const struct { double fs; int L; } kLatencyTable[] = {
            { 44100.0, 130 }, { 48000.0, 140 }, { 88200.0, 244 },
            { 96000.0, 264 }, { 176400.0, 472 }, { 192000.0, 512 }
        };
        int expectedL = -1;
        for (auto& e : kLatencyTable) if (std::abs (e.fs - fs) < 1.0) expectedL = e.L;
        if (expectedL < 0) { fail ("engineDryDelayLatencyTest" + tag + ": fs not in the standard rate table"); return; }

        const int Lpublic = fc::OmoideEcho::latencyForRate (fs);

        const double rDown = fs / fc::OmoideEcho::kInternalRateHz;
        const double rUp   = fc::OmoideEcho::kInternalRateHz / fs;
        const double g = fc::VariPolyphaseResampler::groupDelayInputSamples (rDown)
                        + fc::VariPolyphaseResampler::groupDelayInputSamples (rUp) * (fs / fc::OmoideEcho::kInternalRateHz);
        const int Lcontract = (int) std::lround (g) + fc::OmoideEcho::kFifoMargin;

        const int Lclosed = (int) std::lround (2.0 * 31.0 * fs / fc::OmoideEcho::kInternalRateHz) + 16;

        if (Lpublic != expectedL)
            fail ("engineDryDelayLatencyTest" + tag + ": latencyForRate=" + std::to_string (Lpublic) + " expected=" + std::to_string (expectedL));
        if (Lcontract != expectedL)
            fail ("engineDryDelayLatencyTest" + tag + ": contract-formula L=" + std::to_string (Lcontract) + " expected=" + std::to_string (expectedL));
        if (Lclosed != expectedL)
            fail ("engineDryDelayLatencyTest" + tag + ": closed-form L=" + std::to_string (Lclosed) + " expected=" + std::to_string (expectedL));

        fc::OmoideEcho engPin;
        engPin.prepare (fs, 2);
        if (engPin.latencySamples() != expectedL)
            fail ("engineDryDelayLatencyTest" + tag + ": engine latencySamples()=" + std::to_string (engPin.latencySamples()) + " expected=" + std::to_string (expectedL));

        if (fc::OmoideEcho::delaySamplesForMs (500.0) != 12000)
            fail ("engineDryDelayLatencyTest" + tag + ": delaySamplesForMs(500)=" + std::to_string (fc::OmoideEcho::delaySamplesForMs (500.0)));
        if (fc::OmoideEcho::delaySamplesForMs (100.0) != 2400)
            fail ("engineDryDelayLatencyTest" + tag + ": delaySamplesForMs(100)=" + std::to_string (fc::OmoideEcho::delaySamplesForMs (100.0)));
        if (fc::OmoideEcho::delaySamplesForMs (10000.0) != 240000)
            fail ("engineDryDelayLatencyTest" + tag + ": delaySamplesForMs(10000)=" + std::to_string (fc::OmoideEcho::delaySamplesForMs (10000.0)));
        if (std::abs (fc::OmoideEcho::scanAgeSecondsForScan01 (1.0) - 119.0) > 1.0e-12)
            fail ("engineDryDelayLatencyTest" + tag + ": scanAgeSecondsForScan01(1.0)=" + std::to_string (fc::OmoideEcho::scanAgeSecondsForScan01 (1.0)));

        const int L = expectedL;

        // ---- (a) mix=0: bit-for-bit L-sample pure delay (stereo) -----------
        {
            fc::OmoideEcho eng;
            eng.setMix01 (0.0);
            eng.setDelayMs (500.0);
            eng.setRegen01 (0.5);
            eng.setToneHz (6000.0);
            eng.setScan01 (0.0);
            eng.setScanLevel01 (0.0);
            eng.prepare (fs, 2);

            const long long N2s = (long long) std::llround (2.0 * fs);
            std::vector<float> inL ((size_t) N2s), inR ((size_t) N2s);
            Lcg lcgL (0x1111ULL), lcgR (0x2222ULL);
            for (long long n = 0; n < N2s; ++n) { inL[(size_t) n] = (float) lcgL.next(); inR[(size_t) n] = (float) lcgR.next(); }
            std::vector<float> outL (inL), outR (inR);
            processFixedChunk (eng, outL, outR, 509);

            double maxErr = 0.0;
            for (long long n = 0; n < N2s; ++n)
            {
                const double expL = (n >= L) ? (double) inL[(size_t) (n - L)] : 0.0;
                const double expR = (n >= L) ? (double) inR[(size_t) (n - L)] : 0.0;
                maxErr = std::max ({ maxErr, std::abs ((double) outL[(size_t) n] - expL), std::abs ((double) outR[(size_t) n] - expR) });
            }
            if (maxErr > 1.0e-6)
                fail ("engineDryDelayLatencyTest" + tag + ": mix=0 pure-delay maxErr=" + std::to_string (maxErr) + " (stereo)");
        }

        // ---- (a)2. mono path -----------------------------------------------
        {
            fc::OmoideEcho eng;
            eng.setMix01 (0.0);
            eng.setDelayMs (500.0);
            eng.setRegen01 (0.5);
            eng.setToneHz (6000.0);
            eng.prepare (fs, 1);

            const long long N05 = (long long) std::llround (0.5 * fs);
            std::vector<float> mono ((size_t) N05);
            Lcg lcgM (0x3333ULL);
            for (long long n = 0; n < N05; ++n) mono[(size_t) n] = (float) lcgM.next();
            std::vector<float> monoOut (mono);

            long long offset = 0;
            while (offset < N05)
            {
                const int h = (int) std::min<long long> (509, N05 - offset);
                float* ptrs[1] = { monoOut.data() + offset };
                eng.process (ptrs, 1, h);
                offset += h;
            }

            double maxErr = 0.0;
            for (long long n = 0; n < N05; ++n)
            {
                const double exp = (n >= L) ? (double) mono[(size_t) (n - L)] : 0.0;
                maxErr = std::max (maxErr, std::abs ((double) monoOut[(size_t) n] - exp));
            }
            if (maxErr > 1.0e-6)
                fail ("engineDryDelayLatencyTest" + tag + ": mix=0 pure-delay maxErr=" + std::to_string (maxErr) + " (mono)");
        }

        // ---- (b) mix=1 wet landing: xcorr(out,in) argmax == L +/-2 ---------
        // scanLevel=1/scan=0 isolates a near-instant tap (age clamps to the
        // HistoryBuffer floor of 1 internal sample) inside the narrow search
        // window; the echoTap's OWN (far later) copy at delay=500ms sits
        // outside [L-64,L+64] and cannot interfere.
        {
            fc::OmoideEcho eng;
            eng.setScanLevel01 (1.0);
            eng.setScan01 (0.0);
            eng.setRegen01 (0.0);
            eng.setMix01 (1.0);
            eng.setDelayMs (500.0);
            eng.setToneHz (6000.0);
            eng.prepare (fs, 2);

            const long long Nb = (long long) std::llround (2.0 * fs);
            std::vector<float> inL ((size_t) Nb), inR ((size_t) Nb);
            Lcg lcgL (0x5555ULL), lcgR (0x6666ULL);
            for (long long n = 0; n < Nb; ++n) { inL[(size_t) n] = (float) lcgL.next(); inR[(size_t) n] = (float) lcgR.next(); }
            std::vector<float> outL (inL), outR (inR);
            processFixedChunk (eng, outL, outR, 509);

            const std::vector<double> dIn  = toDoubleVec (inL);
            const std::vector<double> dOut = toDoubleVec (outL);
            // scan01=0 targets age=0 seconds, but HistoryBuffer's documented
            // causality floor clamps ageSamples to >= 1 internal sample (age 0
            // would name the in-flight, not-yet-written sample -- see
            // HistoryBuffer.h's read-before-write contract) -- this is NOT the
            // resampler's "OFF=0" extra-offset R2 refers to, it's a separate,
            // well-documented, deterministic +1-internal-sample floor. Fold it
            // into the expected centre (not into the +/-2 tolerance, which
            // remains exactly the plan's fractional-resampling-phase-jitter
            // allowance) so the isolated near-instant tap lands where the
            // engine actually (and correctly) places it.
            const long long ageFloorHost = (long long) std::llround (1.0 * fs / fc::OmoideEcho::kInternalRateHz);
            const long long expectedLag = (long long) L + ageFloorHost;
            const long lag = xcorrArgmax (dOut, dIn, (long) (expectedLag - 64), (long) (expectedLag + 64));
            if (std::llabs ((long long) lag - expectedLag) > 2)
                fail ("engineDryDelayLatencyTest" + tag + ": wet-landing xcorr argmax=" + std::to_string (lag) + " expected=" + std::to_string (expectedLag) + " (L=" + std::to_string (L) + " + age-floor=" + std::to_string (ageFloorHost) + ")");
        }
    }
    // #7 (§5.9): detector-floor / no phantom output (class J, absolute floor).
    void engineDetectorFloorTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);

        // 1. signal -> silence residual drains to <= 1e-12.
        {
            fc::OmoideEcho eng;
            eng.setScanLevel01 (0.0);
            eng.setRegen01 (0.0);
            eng.setDelayMs (100.0);
            eng.setMix01 (1.0);
            eng.setToneHz (6000.0);
            eng.prepare (fs, 2);

            const long long N1 = (long long) std::llround (1.0 * fs);
            std::vector<float> bufL ((size_t) N1), bufR ((size_t) N1);
            Lcg lcgL (0x9999ULL), lcgR (0xAAAAULL);
            for (long long n = 0; n < N1; ++n) { bufL[(size_t) n] = (float) (0.5 * lcgL.next()); bufR[(size_t) n] = (float) (0.5 * lcgR.next()); }
            processFixedChunk (eng, bufL, bufR, 509);

            const long long Ntail = (long long) std::llround (1.0 * fs);
            std::vector<float> silL ((size_t) Ntail, 0.0f), silR ((size_t) Ntail, 0.0f);
            processFixedChunk (eng, silL, silR, 509);

            const long long tailStart = Ntail - (long long) std::llround (0.5 * fs);
            double peak = 0.0;
            for (long long n = tailStart; n < Ntail; ++n)
                peak = std::max ({ peak, (double) std::fabs (silL[(size_t) n]), (double) std::fabs (silR[(size_t) n]) });
            if (peak > 1.0e-12)
                fail ("engineDetectorFloorTest" + tag + ": signal->silence residual=" + std::to_string (peak) + " > 1e-12");
        }

        // 2. silence -> silence (no phantom self-excitation).
        {
            fc::OmoideEcho eng;
            eng.setScanLevel01 (0.0);
            eng.setRegen01 (0.0);
            eng.setDelayMs (100.0);
            eng.setMix01 (1.0);
            eng.setToneHz (6000.0);
            eng.prepare (fs, 2);

            const long long N1 = (long long) std::llround (1.0 * fs);
            std::vector<float> silL ((size_t) N1, 0.0f), silR ((size_t) N1, 0.0f);
            processFixedChunk (eng, silL, silR, 509);

            const double peak = std::max (peakAbsF (silL), peakAbsF (silR));
            if (peak > 1.0e-12)
                fail ("engineDetectorFloorTest" + tag + ": silence->silence peak=" + std::to_string (peak) + " > 1e-12");
        }
    }

    // ---- shared churn schedule for #8a/#8b/#9b -----------------------------
    // A deterministic, cycling schedule of the six setters used both for the
    // plain determinism gate (#8a) and the param-NaN-guard A/B gate (#8b).
    // `injectNaN` additionally calls one rotating setter with a non-finite
    // value at every boundary (AFTER the legitimate scheduled call) -- since
    // every setter's isfinite guard makes that a no-op, run A (injectNaN=
    // false) and run B (injectNaN=true) must be bit-identical.
    static const double kChurnDelays[4]   = { 200.0, 800.0, 1500.0, 5000.0 };
    static const double kChurnRegens[4]   = { 0.0, 0.3, 0.7, 1.0 };
    static const double kChurnTones[4]    = { 500.0, 3000.0, 8000.0, 11000.0 };
    static const double kChurnScans[4]    = { 0.0, 0.2, 0.6, 1.0 };
    static const double kChurnScanLvls[4] = { 0.0, 0.5, 1.0, 0.3 };
    static const double kChurnMixes[4]    = { 0.0, 0.4, 0.8, 1.0 };

    void applyChurnValues (fc::OmoideEcho& eng, int i)
    {
        const int k = i % 4;
        eng.setDelayMs (kChurnDelays[k]);
        eng.setRegen01 (kChurnRegens[k]);
        eng.setToneHz (kChurnTones[k]);
        eng.setScan01 (kChurnScans[k]);
        eng.setScanLevel01 (kChurnScanLvls[k]);
        eng.setMix01 (kChurnMixes[k]);
    }

    double churnNanRotation (int idx)
    {
        switch (idx % 3)
        {
            case 0:  return std::numeric_limits<double>::quiet_NaN();
            case 1:  return std::numeric_limits<double>::infinity();
            default: return -std::numeric_limits<double>::infinity();
        }
    }

    void runChurnSchedule (fc::OmoideEcho& eng, int boundaryChunk,
                           std::vector<float>& bufL, std::vector<float>& bufR,
                           bool injectNaN)
    {
        const long long total = (long long) bufL.size();
        long long offset = 0;
        int stepIdx = 0;
        while (offset < total)
        {
            const int chunk = (int) std::min<long long> (boundaryChunk, total - offset);
            applyChurnValues (eng, stepIdx);
            if (injectNaN)
            {
                const double badVal = churnNanRotation (stepIdx);
                switch (stepIdx % 6)
                {
                    case 0: eng.setDelayMs (badVal); break;
                    case 1: eng.setRegen01 (badVal); break;
                    case 2: eng.setToneHz (badVal); break;
                    case 3: eng.setScan01 (badVal); break;
                    case 4: eng.setScanLevel01 (badVal); break;
                    case 5: eng.setMix01 (badVal); break;
                }
            }
            float* ptrs[2] = { bufL.data() + offset, bufR.data() + offset };
            eng.process (ptrs, 2, chunk);
            offset += chunk;
            ++stepIdx;
        }
    }

    std::vector<float> makeChurnInput (double fs, double durationSec, uint64_t seed, double amp = 0.4)
    {
        const long long n = (long long) std::llround (durationSec * fs);
        std::vector<float> v ((size_t) n);
        Lcg lcg (seed);
        for (long long i = 0; i < n; ++i) v[(size_t) i] = (float) (amp * lcg.next());
        return v;
    }
    // #8a (§5.10): two runs from fresh prepare, identical input + identical
    // scheduled setter mid-run, must be bit-identical.
    void engineDeterminismTest (double fs)
    {
        const std::vector<float> inL0 = makeChurnInput (fs, 1.5, 0xD00D0001ULL);
        const std::vector<float> inR0 = makeChurnInput (fs, 1.5, 0xD00D0002ULL);
        const int boundaryChunk = (int) std::max<long long> (1, (long long) std::llround (0.1 * fs));

        std::vector<float> outLA (inL0), outRA (inR0);
        fc::OmoideEcho engA;
        applyChurnValues (engA, 0);
        engA.prepare (fs, 2);
        runChurnSchedule (engA, boundaryChunk, outLA, outRA, false);

        std::vector<float> outLB (inL0), outRB (inR0);
        fc::OmoideEcho engB;
        applyChurnValues (engB, 0);
        engB.prepare (fs, 2);
        runChurnSchedule (engB, boundaryChunk, outLB, outRB, false);

        if (outLA != outLB || outRA != outRB)
            fail ("engineDeterminismTest @Fs=" + std::to_string (fs) + ": two runs from fresh prepare are not bit-identical");
    }

    // #8b (§5.11): run A (clean churn) vs run B (same churn + rotating
    // NaN/Inf injected setter calls) must be bit-identical -- every setter's
    // isfinite guard makes the injected call a no-op.
    void engineParamNanGuardTest (double fs)
    {
        const std::vector<float> inL0 = makeChurnInput (fs, 1.5, 0xBEEF0001ULL);
        const std::vector<float> inR0 = makeChurnInput (fs, 1.5, 0xBEEF0002ULL);
        const int boundaryChunk = (int) std::max<long long> (1, (long long) std::llround (0.1 * fs));

        std::vector<float> outLA (inL0), outRA (inR0);
        fc::OmoideEcho engA;
        applyChurnValues (engA, 0);
        engA.prepare (fs, 2);
        runChurnSchedule (engA, boundaryChunk, outLA, outRA, false);

        std::vector<float> outLB (inL0), outRB (inR0);
        fc::OmoideEcho engB;
        applyChurnValues (engB, 0);
        engB.prepare (fs, 2);
        runChurnSchedule (engB, boundaryChunk, outLB, outRB, true);

        if (outLA != outLB || outRA != outRB)
            fail ("engineParamNanGuardTest @Fs=" + std::to_string (fs) + ": NaN/Inf-injected run diverged from the clean run (isfinite guard not effective)");
    }
    // #9a (§5.12): input-side NaN/+Inf/-Inf injection (single samples) during
    // normal operation -- output must stay finite (input finite guard fires
    // before either path) and settle back under the realistic peak bound.
    void engineNanRecoveryTest (double fs)
    {
        fc::OmoideEcho eng;
        eng.setRegen01 (0.7);
        eng.setScanLevel01 (0.5);
        eng.setMix01 (0.7);
        eng.setDelayMs (300.0);
        eng.setToneHz (6000.0);
        eng.prepare (fs, 2);

        const double totalSec = 2.0;
        const long long N = (long long) std::llround (totalSec * fs);
        std::vector<float> inL ((size_t) N), inR ((size_t) N);
        Lcg lcgL (0x7777ULL), lcgR (0x8888ULL);
        for (long long n = 0; n < N; ++n) { inL[(size_t) n] = (float) (0.3 * lcgL.next()); inR[(size_t) n] = (float) (0.3 * lcgR.next()); }

        const long long p1 = N / 4, p2 = N / 2, p3 = 3 * N / 4;
        inL[(size_t) p1] = std::numeric_limits<float>::quiet_NaN();      inR[(size_t) p1] = std::numeric_limits<float>::quiet_NaN();
        inL[(size_t) p2] = std::numeric_limits<float>::infinity();       inR[(size_t) p2] = std::numeric_limits<float>::infinity();
        inL[(size_t) p3] = -std::numeric_limits<float>::infinity();      inR[(size_t) p3] = -std::numeric_limits<float>::infinity();

        std::vector<float> outL (inL), outR (inR);
        processFixedChunk (eng, outL, outR, 509);

        if (! allFiniteF (outL) || ! allFiniteF (outR))
            fail ("engineNanRecoveryTest @Fs=" + std::to_string (fs) + ": non-finite output sample after input-side NaN/Inf injection");

        const long long win1s = (long long) std::llround (1.0 * fs);
        for (long long p : { p1, p2, p3 })
        {
            const long long winEnd = std::min (N, p + win1s);
            double peak = 0.0;
            for (long long n = p; n < winEnd; ++n)
                peak = std::max ({ peak, (double) std::fabs (outL[(size_t) n]), (double) std::fabs (outR[(size_t) n]) });
            if (peak > fc::OmoideEcho::kPeakBound)
                fail ("engineNanRecoveryTest @Fs=" + std::to_string (fs) + ": post-injection (p=" + std::to_string (p) + ") peak=" + std::to_string (peak) + " exceeds kPeakBound");
        }
    }
    // #9b (§5.13): reset() residue + reset-determinism + state-reset-on-prepare.
    void engineResetResidueTest (double fs)
    {
        const std::string tag = " @Fs=" + std::to_string (fs);

        // 1. Excite all paths, reset(), then silence -> residue <= 1e-12.
        {
            fc::OmoideEcho eng;
            eng.setRegen01 (0.9);
            eng.setScanLevel01 (1.0);
            eng.setMix01 (1.0);
            eng.setDelayMs (300.0);
            eng.setToneHz (6000.0);
            eng.setScan01 (0.0);
            eng.prepare (fs, 2);

            const long long Nexcite = (long long) std::llround (1.5 * fs);
            const long long stepSamples = std::max<long long> (1, (long long) std::llround (0.05 * fs));
            Lcg lcgL (0xE1E1ULL), lcgR (0xE2E2ULL);
            long long offset = 0, nextBoundary = 0;
            while (offset < Nexcite)
            {
                if (offset >= nextBoundary)
                {
                    const double t = (double) nextBoundary / fs;
                    eng.setScan01 (std::fmod (t, 1.0));
                    nextBoundary += stepSamples;
                }
                const int chunk = (int) std::min<long long> (509, Nexcite - offset);
                std::vector<float> bl ((size_t) chunk), br ((size_t) chunk);
                for (int i = 0; i < chunk; ++i) { bl[(size_t) i] = (float) (0.5 * lcgL.next()); br[(size_t) i] = (float) (0.5 * lcgR.next()); }
                float* ptrs[2] = { bl.data(), br.data() };
                eng.process (ptrs, 2, chunk);
                offset += chunk;
            }

            eng.reset();

            const long long Nsilence = (long long) std::llround (0.5 * fs);
            std::vector<float> silL ((size_t) Nsilence, 0.0f), silR ((size_t) Nsilence, 0.0f);
            processFixedChunk (eng, silL, silR, 509);
            const double peak = std::max (peakAbsF (silL), peakAbsF (silR));
            if (peak > 1.0e-12)
                fail ("engineResetResidueTest" + tag + ": reset() residue=" + std::to_string (peak) + " > 1e-12");
        }

        // 2. reset() equivalence to fresh prepare(): align targets to the
        //    churn schedule's initial values BEFORE reset(), so reset()'s
        //    Sm-snap matches a fresh prepare()'s Sm-snap exactly.
        {
            const std::vector<float> inL0 = makeChurnInput (fs, 1.5, 0xF1F10001ULL);
            const std::vector<float> inR0 = makeChurnInput (fs, 1.5, 0xF1F10002ULL);
            const int boundaryChunk = (int) std::max<long long> (1, (long long) std::llround (0.1 * fs));

            fc::OmoideEcho engDirty;
            engDirty.setRegen01 (0.9);
            engDirty.setScanLevel01 (1.0);
            engDirty.setMix01 (1.0);
            engDirty.setDelayMs (300.0);
            engDirty.setToneHz (6000.0);
            engDirty.prepare (fs, 2);
            {
                const long long Ndirty = (long long) std::llround (0.5 * fs);
                std::vector<float> dl ((size_t) Ndirty), dr ((size_t) Ndirty);
                Lcg lcgL (0x1234ULL), lcgR (0x4321ULL);
                for (long long n = 0; n < Ndirty; ++n) { dl[(size_t) n] = (float) (0.5 * lcgL.next()); dr[(size_t) n] = (float) (0.5 * lcgR.next()); }
                processFixedChunk (engDirty, dl, dr, 509);
            }
            applyChurnValues (engDirty, 0);
            engDirty.reset();

            std::vector<float> outLDirty (inL0), outRDirty (inR0);
            runChurnSchedule (engDirty, boundaryChunk, outLDirty, outRDirty, false);

            fc::OmoideEcho engFresh;
            applyChurnValues (engFresh, 0);
            engFresh.prepare (fs, 2);
            std::vector<float> outLFresh (inL0), outRFresh (inR0);
            runChurnSchedule (engFresh, boundaryChunk, outLFresh, outRFresh, false);

            if (outLDirty != outLFresh || outRDirty != outRFresh)
                fail ("engineResetResidueTest" + tag + ": reset() after excitation does not match fresh-prepare run (reset determinism)");
        }

        // 3. state-reset-on-prepare: calling prepare() twice re-initialises
        //    fully, independent of what happened between the two calls.
        {
            const std::vector<float> inL0 = makeChurnInput (fs, 1.5, 0xF2F20001ULL);
            const std::vector<float> inR0 = makeChurnInput (fs, 1.5, 0xF2F20002ULL);
            const int boundaryChunk = (int) std::max<long long> (1, (long long) std::llround (0.1 * fs));

            fc::OmoideEcho engRe;
            applyChurnValues (engRe, 0);
            engRe.prepare (fs, 2); // 1st prepare
            {
                const long long Ndirty = (long long) std::llround (0.5 * fs);
                std::vector<float> dl ((size_t) Ndirty), dr ((size_t) Ndirty);
                Lcg lcgL (0x5678ULL), lcgR (0x8765ULL);
                for (long long n = 0; n < Ndirty; ++n) { dl[(size_t) n] = (float) (0.5 * lcgL.next()); dr[(size_t) n] = (float) (0.5 * lcgR.next()); }
                engRe.setRegen01 (1.0); engRe.setScanLevel01 (1.0); engRe.setDelayMs (5000.0); // dirty targets
                processFixedChunk (engRe, dl, dr, 509);
            }
            applyChurnValues (engRe, 0);
            engRe.prepare (fs, 2); // 2nd prepare -- must fully re-initialise

            std::vector<float> outLRe (inL0), outRRe (inR0);
            runChurnSchedule (engRe, boundaryChunk, outLRe, outRRe, false);

            fc::OmoideEcho engFresh2;
            applyChurnValues (engFresh2, 0);
            engFresh2.prepare (fs, 2);
            std::vector<float> outLFresh2 (inL0), outRFresh2 (inR0);
            runChurnSchedule (engFresh2, boundaryChunk, outLFresh2, outRFresh2, false);

            if (outLRe != outLFresh2 || outRRe != outRFresh2)
                fail ("engineResetResidueTest" + tag + ": second prepare() does not fully re-initialise state (state-reset-on-prepare)");
        }
    }
    // determinism derivative (§5.14): STATIC parameters (no mid-run setter
    // calls -- R3 snap means all Sm increments are exactly 0, so chunk-split
    // bit-invariance holds; this does NOT extend to schedules with mid-run
    // setter calls, see §5.11's same-chunk-sequence gate instead).
    //
    // STATUS: RED at non-integer fs/kInternalRateHz ratios, pending a human/
    // orchestrator decision (do NOT touch this oracle/tolerance). Measured
    // divergence is ~4e-6, consistent with FP add non-associativity across
    // different chunk-boundary accumulation orders (same bug class already
    // tolerance-ized elsewhere, e.g. madoromi) rather than a real determinism
    // break; a candidate fix is loosening the bit-exact compare to a <= 1e-5
    // per-sample tolerance, but that is a test-tolerance change and must be
    // decided by a human, not applied autonomously here.
    void engineChunkInvarianceTest (double fs)
    {
        fc::OmoideEcho engFixed, engPrime;
        for (fc::OmoideEcho* e : { &engFixed, &engPrime })
        {
            e->setRegen01 (0.5);
            e->setScan01 (0.3);
            e->setScanLevel01 (0.4);
            e->setToneHz (6000.0);
            e->setDelayMs (400.0);
            e->setMix01 (0.7);
            e->prepare (fs, 2);
        }

        const std::vector<float> inL0 = makeChurnInput (fs, 2.0, 0x0BADCAFEULL);
        const std::vector<float> inR0 = makeChurnInput (fs, 2.0, 0xCAFE0BADULL);

        std::vector<float> outLFixed (inL0), outRFixed (inR0);
        processFixedChunk (engFixed, outLFixed, outRFixed, 512);

        std::vector<float> outLPrime (inL0), outRPrime (inR0);
        processPrimeChunks (engPrime, outLPrime, outRPrime);

        if (outLFixed != outLPrime || outRFixed != outRPrime)
            fail ("engineChunkInvarianceTest @Fs=" + std::to_string (fs) + ": fixed-512 vs prime-chunk runs are not bit-identical (static parameters)");
    }

    void coreTests (double fs)
    {
        historyBufferInterpExactnessTest (fs);
        historyBufferWrapGuardResetTest (fs);

        engineEchoScheduleRatioTest (fs);
        engineLoopGainStabilityTest (fs);
        engineLongHoldPeakTest (fs);
        engineScanAgeAccuracyTest (fs);
        engineScanDopplerGlideTest (fs);
        engineHistoryCapacityLongRunTest (fs);
        engineBracketBandLimitTest (fs);
        engineDryDelayLatencyTest (fs);
        engineDetectorFloorTest (fs);
        engineDeterminismTest (fs);
        engineParamNanGuardTest (fs);
        engineNanRecoveryTest (fs);
        engineResetResidueTest (fs);
        engineChunkInvarianceTest (fs);
    }
} // namespace

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))   // all 6 rates by default (hand-written subsets forbidden)
        coreTests (Fs);

    checkAliasCrossRateSpread();

    if (g_failures > 0) { std::fprintf (stderr, "%d failure(s)\n", g_failures); return 1; }
    std::puts ("omoide-echo dsp_test: all OK");
    return 0;
}
