//
// dsp_test.cpp — headless verification of the mochi-stretch DSP core.
//
// Spec-based suite per scratchpad/mochi-test-plan.md (RECONCILE-complete,
// real-header edition). Oracles are independent of the implementation under
// test: published pure functions (pitchFactor / passthroughDelaySamples) are
// transcribed test-side and cross-checked against the header (contract
// verification, not derivation-from-process-path); analytic trajectories
// (one-pole glide closed form, wrap-period arithmetic, equal-power crossfade
// envelope) are derived from the spec, never from MochiStretch::process().
//
#include "factory_core/MochiStretch.h"
#include "factory_core/PitchShifter.h"
#include "factory_core/HistoryBuffer.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace fct = factory_core::testing;
namespace fc  = factory_core;

static int g_failures = 0;
static void fail (const std::string& m)
{ ++g_failures; std::fprintf (stderr, "FAIL: %s\n", m.c_str()); }

// =====================================================================
// 3. Common helpers
// =====================================================================

constexpr double kPi = 3.14159265358979323846;

// Deterministic LCG, uniform on [-1,1). Self-contained (no header dependency).
struct Lcg
{
    uint64_t s;
    explicit Lcg (uint64_t seed) : s (seed) {}
    double next() noexcept
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        // NOTE: cast to int64_t FIRST, then arithmetic-shift (sign-extending) --
        // shifting the unsigned s first (as the plan's §3 snippet literally
        // wrote it: `(int64_t)(s >> 11)`) always yields a non-negative value,
        // silently breaking the documented "[-1,1) uniform" contract (it would
        // only ever produce [0,2) -- top 53 bits * 2^-52, mean ~1.0, NOT
        // [0,1) as an earlier draft of this comment stated). This is a
        // test-helper bug fix, not an oracle/tolerance change -- see the
        // final report.
        return (double) ((int64_t) s >> 11) * (1.0 / 4503599627370496.0);
    }
};

// Independent pitch-law oracle (spec formula transcribed test-side; used for
// double cross-check against the published pure function).
static double compRef (double s)
{
    return 1.0 / std::clamp (std::abs (s), fc::MochiStretch::kCompMin, fc::MochiStretch::kCompMax);
}
static double pitchFactorRef (double s, double p)
{
    return std::abs (s) * compRef (s) * std::pow (2.0, p / 12.0);
}

// Independent D0 table (hand-transcribed from the plan's §0.1 integer proof,
// not from the implementation) -- used as a double cross-check on top of the
// header-formula transcription.
static long expectedD0ForRate (double fs)
{
    const int r = (int) std::llround (fs);
    switch (r)
    {
        case  44100: return 1765;
        case  48000: return 1921;
        case  88200: return 3529;
        case  96000: return 3841;
        case 176400: return 7057;
        case 192000: return 7681;
        default: return (long) (1.0 + 0.5 * std::max (8.0, 0.080 * fs));
    }
}

// Integer-period Goertzel magnitude (2/N normalised amplitude) over
// x[start, start+N).
static double goertzelMag (const std::vector<float>& x, size_t start, size_t N, double f, double fs)
{
    const double w  = 2.0 * kPi * f / fs;
    const double cw = std::cos (w);
    const double coeff = 2.0 * cw;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < N; ++i)
    {
        s0 = (double) x[start + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * cw;
    const double imag = s2 * std::sin (w);
    const double mag  = std::sqrt (real * real + imag * imag);
    return (2.0 / (double) N) * mag;
}

// Positive-going zero crossing counter with hysteresis +-hyst*A (A assumed
// normalised to peak 1 for the hyst argument passed in absolute units).
static int countPosCrossings (const std::vector<float>& x, size_t start, size_t n, double hyst)
{
    int count = 0;
    bool armed = true; // ready to detect an upward crossing of +hyst after having been below -hyst
    bool below = true;
    for (size_t i = 0; i < n; ++i)
    {
        const double v = (double) x[start + i];
        if (v < -hyst) { below = true; }
        if (below && v > hyst) { ++count; below = false; }
    }
    (void) armed;
    return count;
}

struct XcorrResult { long lag = 0; double rho = 0.0; };

// Normalised cross-correlation: slide tmpl (length M) over sig at lags
// [lo, hi]; rho(lag) = sum(sig[lag+k]*tmpl[k]) / sqrt(sum(sig^2)*sum(tmpl^2)).
static XcorrResult normXcorr (const std::vector<float>& sig, const std::vector<float>& tmpl,
                               long lo, long hi)
{
    double tmplEnergy = 0.0;
    for (double t : tmpl) tmplEnergy += t * t;

    XcorrResult best;
    for (long lag = lo; lag <= hi; ++lag)
    {
        if (lag < 0 || lag + (long) tmpl.size() > (long) sig.size())
            continue;
        double dot = 0.0, sigEnergy = 0.0;
        for (size_t k = 0; k < tmpl.size(); ++k)
        {
            const double sv = (double) sig[(size_t) lag + k];
            dot       += sv * tmpl[k];
            sigEnergy += sv * sv;
        }
        const double denom = std::sqrt (sigEnergy * tmplEnergy);
        const double rho = (denom > 1e-300) ? dot / denom : 0.0;
        if (lag == lo || std::abs (rho) > std::abs (best.rho))
        {
            best.lag = lag;
            best.rho = rho;
        }
    }
    return best;
}

// Linear chirp, [0,L) samples, instantaneous frequency f_lo -> f_hi, amplitude A.
static void makeChirp (std::vector<float>& c, size_t L, double f_lo, double f_hi, double A, double fs)
{
    c.assign (L, 0.0f);
    const double T = (double) L / fs;
    const double k = (f_hi - f_lo) / T; // Hz/s
    double phase = 0.0;
    for (size_t n = 0; n < L; ++n)
    {
        const double t = (double) n / fs;
        const double instF = f_lo + k * t;
        phase += 2.0 * kPi * instF / fs;
        c[n] = (float) (A * std::sin (phase));
    }
}

static const int kPrimeChunks[] = { 1, 2, 3, 5, 7, 13, 31, 61, 127, 251, 509 };

// =====================================================================
// primitive tests (§4)
// =====================================================================
static void pitchShifterUnityDelayTest (double fs);

// =====================================================================
// engine tests (§5)
// =====================================================================
static void enginePureDelayPassthroughTest  (double fs);  // #1
static void enginePitchLawTest              (double fs);  // #2
static void engineReversePlaybackTest       (double fs);  // #3
static void engineTapeStopTrajectoryTest    (double fs);  // #4
static void engineHoldLoopPeriodicityTest   (double fs);  // #5
static void engineWrapCrossfadeContinuityTest (double fs); // #6
static void engineLongHoldPeakTest          (double fs);  // #7
static void engineWindowChangeRecoveryTest  (double fs);  // #9
static void engineDeterminismTest           (double fs);  // #8 (2-run bit)
static void engineParamNanGuardTest         (double fs);  // #8 (param-NaN A/B)
static void engineNanRecoveryTest           (double fs);  // #8 (input NaN recovery)
static void engineResetResidueTest          (double fs);  // #8 (reset residue + silence->0 + prepare reinit)
static void engineChunkInvarianceTest        (double fs); // #8 (static-parameter chunk invariance)

static void coreTests (double fs)
{
    pitchShifterUnityDelayTest (fs);
    enginePureDelayPassthroughTest (fs);
    enginePitchLawTest (fs);
    engineReversePlaybackTest (fs);
    engineTapeStopTrajectoryTest (fs);
    engineHoldLoopPeriodicityTest (fs);
    engineWrapCrossfadeContinuityTest (fs);
    engineLongHoldPeakTest (fs);
    engineWindowChangeRecoveryTest (fs);
    engineDeterminismTest (fs);
    engineParamNanGuardTest (fs);
    engineNanRecoveryTest (fs);
    engineResetResidueTest (fs);
    engineChunkInvarianceTest (fs);
}

int main (int argc, char** argv)
{
    for (double fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (fs);

    if (g_failures > 0) { std::fprintf (stderr, "%d failure(s)\n", g_failures); return 1; }
    std::puts ("mochi-stretch dsp_test: all OK");
    return 0;
}

// =====================================================================
// Test bodies filled in below (placeholders replaced by subsequent edits)
// =====================================================================

// ---------------------------------------------------------------------
// pitchShifterUnityDelayTest -- plan §4.1 (primitive; #1 anchor)
// ---------------------------------------------------------------------
static void pitchShifterUnityDelayTest (double fs)
{
    fc::PitchShifter ps;
    ps.prepare (fs, 80.0);
    const long H = (long) std::llround (0.040 * fs);

    // 1. window/2 integer pin (exact equality -- §0.1 floating-point proof).
    const double w = std::max (8.0, 80.0 * 1.0e-3 * fs);
    if (w != 2.0 * (double) H)
    {
        fail ("pitchShifterUnityDelayTest: window/2 not an exact integer at fs=" + std::to_string (fs));
        return; // premise changed -- Ask a human, do not proceed on a broken assumption.
    }

    // 2. ratio=1 pure delay: internal DelayLine is double-storage -> 1e-9 is valid.
    ps.setRatio (1.0);
    Lcg lcg (0xF00DF00DULL);
    const long total = 4 * H + 1000;
    std::vector<double> x ((size_t) total), y ((size_t) total);
    for (long n = 0; n < total; ++n)
    {
        x[(size_t) n] = 0.5 * lcg.next();
        y[(size_t) n] = ps.process (x[(size_t) n]);
    }

    double maxErr = 0.0;
    for (long n = H; n < total; ++n)
        maxErr = std::max (maxErr, std::abs (y[(size_t) n] - x[(size_t) (n - H)]));
    if (maxErr > 1e-9)
        fail ("pitchShifterUnityDelayTest: pure-delay tolerance exceeded (fs=" + std::to_string (fs) +
              " err=" + std::to_string (maxErr) + ")");

    double maxUnfilled = 0.0;
    for (long n = 0; n < H; ++n)
        maxUnfilled = std::max (maxUnfilled, std::abs (y[(size_t) n]));
    if (maxUnfilled > 1e-12)
        fail ("pitchShifterUnityDelayTest: pre-fill samples not silent (fs=" + std::to_string (fs) + ")");

    // 3. positive control: ratio=2 -> +1 octave (shifter is actually shifting).
    fc::PitchShifter ps2;
    ps2.prepare (fs, 80.0);
    ps2.setRatio (2.0);

    const double f0 = 1000.0;
    const long totalSamples = (long) std::llround (1.0 * fs);
    std::vector<float> y2 ((size_t) totalSamples);
    for (long n = 0; n < totalSamples; ++n)
        y2[(size_t) n] = (float) ps2.process (0.5 * std::sin (2.0 * kPi * f0 * (double) n / fs));

    const long skip   = 2 * H + (long) std::llround (0.1 * fs);
    const long winLen = (long) std::llround (0.5 * fs);
    if (skip + winLen <= totalSamples)
    {
        const double magOctave = goertzelMag (y2, (size_t) skip, (size_t) winLen, 2.0 * f0, fs);
        const double magFund   = goertzelMag (y2, (size_t) skip, (size_t) winLen, f0, fs);
        if (magOctave < 10.0 * magFund)
            fail ("pitchShifterUnityDelayTest: ratio=2 octave-shift control failed (fs=" + std::to_string (fs) + ")");
    }
    else
    {
        fail ("pitchShifterUnityDelayTest: octave-control window does not fit (test bug, fs=" + std::to_string (fs) + ")");
    }
}

// ---------------------------------------------------------------------
// enginePureDelayPassthroughTest -- plan §5.1 (contract pin, once) + §5.2 (#1)
// ---------------------------------------------------------------------
static void enginePureDelayPassthroughTest (double fs)
{
    // ---- §5.1 contract pin: published pure functions vs. transcribed formulas ----
    {
        const double windowRef = std::max (8.0, 80.0 * 1.0e-3 * fs);
        const double D0ref     = 1.0 + 0.5 * windowRef;
        const double D0d       = fc::MochiStretch::passthroughDelaySamples (fs);
        if (D0d != D0ref)
            fail ("enginePureDelayPassthroughTest: passthroughDelaySamples != transcribed formula (fs=" + std::to_string (fs) + ")");
        if (D0d != std::floor (D0d))
            fail ("enginePureDelayPassthroughTest: D0 not an integer at a standard rate (fs=" + std::to_string (fs) + ")");
        const long D0chk = (long) D0d;
        if (D0chk != expectedD0ForRate (fs))
            fail ("enginePureDelayPassthroughTest: D0 != independent §0.1 table (fs=" + std::to_string (fs) +
                  " got=" + std::to_string (D0chk) + ")");

        static const double speeds[]  = { -2.0, -1.0, -0.5, 0.25, 0.5, 1.0, 2.0 };
        static const double pitches[] = { -12.0, 0.0, 12.0 };
        for (double s : speeds)
        {
            for (double p : pitches)
            {
                const double got  = fc::MochiStretch::pitchFactor (s, p);
                const double ref  = pitchFactorRef (s, p);
                if (std::abs (got - ref) > 1e-12)
                    fail ("enginePureDelayPassthroughTest: pitchFactor != transcribed ref (fs=" + std::to_string (fs) + ")");
                const double formIndep = std::pow (2.0, p / 12.0);
                if (std::abs (got - formIndep) > 1e-12)
                    fail ("enginePureDelayPassthroughTest: pitchFactor != 2^(p/12) form-independent check (fs=" + std::to_string (fs) + ")");
            }
        }
        if (fc::MochiStretch::pitchFactor (0.0, 0.0) != 0.0)
            fail ("enginePureDelayPassthroughTest: pitchFactor(0,0) != 0 (fs=" + std::to_string (fs) + ")");
    }

    const double D0d = fc::MochiStretch::passthroughDelaySamples (fs);
    const long   D0  = (long) D0d;

    // (a) mix=1, s=1, p=0 -> D0 pure integer delay, stereo, distinct L/R seeds.
    {
        fc::MochiStretch eng;
        eng.setSpeed (1.0);
        eng.setPitchSemis (0.0);
        eng.setWindowMs (1000.0);
        eng.setHold (false);
        eng.setMix01 (1.0);
        eng.prepare (fs, 2);

        const long total = 2 * D0 + (long) std::llround (2.0 * fs);
        std::vector<float> inL ((size_t) total), inR ((size_t) total);
        Lcg lcgL (0x9E3779B9ULL), lcgR (0xC2B2AE35ULL);
        for (long n = 0; n < total; ++n)
        {
            inL[(size_t) n] = (float) (0.5 * lcgL.next());
            inR[(size_t) n] = (float) (0.5 * lcgR.next());
        }
        std::vector<float> outL = inL, outR = inR;
        float* ptrs[2] = { outL.data(), outR.data() };
        eng.process (ptrs, 2, (int) total);

        double maxErr = 0.0;
        for (long n = D0 + 8; n < total; ++n)
        {
            maxErr = std::max (maxErr, std::abs ((double) outL[(size_t) n] - (double) inL[(size_t) (n - D0)]));
            maxErr = std::max (maxErr, std::abs ((double) outR[(size_t) n] - (double) inR[(size_t) (n - D0)]));
        }
        if (maxErr > 1e-6)
            fail ("enginePureDelayPassthroughTest(a): float-floor pure-delay tolerance exceeded (fs=" + std::to_string (fs) +
                  " err=" + std::to_string (maxErr) + ")");

        double maxUnfilled = 0.0;
        for (long n = 0; n < D0 - 8; ++n)
        {
            maxUnfilled = std::max (maxUnfilled, std::abs ((double) outL[(size_t) n]));
            maxUnfilled = std::max (maxUnfilled, std::abs ((double) outR[(size_t) n]));
        }
        if (maxUnfilled > 1e-12)
            fail ("enginePureDelayPassthroughTest(a): pre-fill samples not silent (fs=" + std::to_string (fs) + ")");
    }

    // (b) mix=0 -> dry bit-exact passthrough (stereo).
    {
        fc::MochiStretch eng;
        eng.setSpeed (1.0);
        eng.setPitchSemis (3.0); // arbitrary -- dry must be unaffected by wet-side params
        eng.setWindowMs (1000.0);
        eng.setHold (false);
        eng.setMix01 (0.0);
        eng.prepare (fs, 2);

        const long total = (long) std::llround (1.0 * fs);
        std::vector<float> inL ((size_t) total), inR ((size_t) total);
        Lcg lcgL (0x1234ABCDULL), lcgR (0x87654321ULL);
        for (long n = 0; n < total; ++n)
        {
            inL[(size_t) n] = (float) (0.5 * lcgL.next());
            inR[(size_t) n] = (float) (0.5 * lcgR.next());
        }
        std::vector<float> outL = inL, outR = inR;
        float* ptrs[2] = { outL.data(), outR.data() };
        eng.process (ptrs, 2, (int) total);

        if (std::memcmp (outL.data(), inL.data(), sizeof (float) * (size_t) total) != 0 ||
            std::memcmp (outR.data(), inR.data(), sizeof (float) * (size_t) total) != 0)
            fail ("enginePureDelayPassthroughTest(b): mix=0 dry stereo not bit-exact (fs=" + std::to_string (fs) + ")");
    }

    // mono pass: re-confirm (a) pure-delay identity and (b) bit passthrough, single channel.
    {
        fc::MochiStretch eng;
        eng.setSpeed (1.0);
        eng.setPitchSemis (0.0);
        eng.setWindowMs (1000.0);
        eng.setHold (false);
        eng.setMix01 (1.0);
        eng.prepare (fs, 1);

        const long total = 2 * D0 + (long) std::llround (0.5 * fs);
        std::vector<float> in ((size_t) total);
        Lcg lcg (0xDEADBEEFULL);
        for (long n = 0; n < total; ++n) in[(size_t) n] = (float) (0.5 * lcg.next());

        std::vector<float> out = in;
        float* ptrs[1] = { out.data() };
        eng.process (ptrs, 1, (int) total);

        double maxErr = 0.0;
        for (long n = D0 + 8; n < total; ++n)
            maxErr = std::max (maxErr, std::abs ((double) out[(size_t) n] - (double) in[(size_t) (n - D0)]));
        if (maxErr > 1e-6)
            fail ("enginePureDelayPassthroughTest(mono a): float-floor tolerance exceeded (fs=" + std::to_string (fs) + ")");

        fc::MochiStretch eng2;
        eng2.setSpeed (1.0);
        eng2.setPitchSemis (0.0);
        eng2.setWindowMs (1000.0);
        eng2.setHold (false);
        eng2.setMix01 (0.0);
        eng2.prepare (fs, 1);

        std::vector<float> out2 = in;
        float* ptrs2[1] = { out2.data() };
        eng2.process (ptrs2, 1, (int) total);

        if (std::memcmp (out2.data(), in.data(), sizeof (float) * (size_t) total) != 0)
            fail ("enginePureDelayPassthroughTest(mono b): mix=0 not bit-exact (fs=" + std::to_string (fs) + ")");
    }
}

// ---------------------------------------------------------------------
// enginePitchLawTest -- plan §5.3 (#2, dominant-peak pitch law)
// ---------------------------------------------------------------------
static void enginePitchLawTest (double fs)
{
    const double f0  = 1000.0;
    const double A   = 0.5;
    const double Wms = 500.0;
    const long   S   = (long) std::llround (Wms * fs / 1000.0) + (long) std::llround (0.2 * fs);
    const long   N   = (long) std::llround (fs * 0.01) * 100; // 1.0 s, integer-period at 100 Hz multiples

    static const double speeds[]  = { -2.0, -1.0, -0.5, 0.25, 0.5, 1.0, 2.0 };
    static const double pitches[] = { -12.0, 0.0, 12.0 };

    for (double s : speeds)
    {
        for (double p : pitches)
        {
            fc::MochiStretch eng;
            eng.setSpeed (s);
            eng.setPitchSemis (p);
            eng.setWindowMs (Wms);
            eng.setMix01 (1.0);
            eng.setHold (false);
            eng.prepare (fs, 1);

            const long total = S + N;
            std::vector<float> buf ((size_t) total);
            for (long n = 0; n < total; ++n)
                buf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * f0 * (double) n / fs));

            float* ptrs[1] = { buf.data() };
            eng.process (ptrs, 1, (int) total);

            const double f_true  = f0 * pitchFactorRef (s, p);
            const double f_naive = std::abs (s) * f0 * std::pow (2.0, p / 12.0);

            const double candidates[] = { f_true, f_naive, f0, 2.0 * f_true, 0.5 * f_true };
            double maxMag = -1.0, maxFreq = 0.0;
            for (double fcand : candidates)
            {
                const double m = goertzelMag (buf, (size_t) S, (size_t) N, fcand, fs);
                if (m > maxMag) { maxMag = m; maxFreq = fcand; }
            }
            if (std::abs (maxFreq - f_true) > 1e-9)
                fail ("enginePitchLawTest: dominant peak not at f_true (s=" + std::to_string (s) +
                      ",p=" + std::to_string (p) + ",fs=" + std::to_string (fs) + ")");

            const double magTrue = goertzelMag (buf, (size_t) S, (size_t) N, f_true, fs);
            if (std::abs (std::abs (s) - 1.0) > 1e-9)
            {
                const double magNaive = goertzelMag (buf, (size_t) S, (size_t) N, f_naive, fs);
                if (magTrue < 10.0 * magNaive)
                    fail ("enginePitchLawTest: compensation ratio < 10x (s=" + std::to_string (s) +
                          ",p=" + std::to_string (p) + ",fs=" + std::to_string (fs) + ")");
            }

            if (magTrue < 0.3 * A)
                fail ("enginePitchLawTest: dominant peak below floor 0.3A (s=" + std::to_string (s) +
                      ",p=" + std::to_string (p) + ",fs=" + std::to_string (fs) + ")");

            const int crossings = countPosCrossings (buf, (size_t) S, (size_t) N, 0.1 * A);
            const double measuredFreq = (double) crossings / ((double) N / fs);
            if (std::abs (measuredFreq - f_true) > 0.03 * f_true)
                fail ("enginePitchLawTest: crossing-derived freq outside +-3% (s=" + std::to_string (s) +
                      ",p=" + std::to_string (p) + ",fs=" + std::to_string (fs) + ")");
        }
    }
}

// ---------------------------------------------------------------------
// engineReversePlaybackTest -- plan §5.4 (#3, time-reversal cross-correlation)
// ---------------------------------------------------------------------
static void engineReversePlaybackTest (double fs)
{
    fc::MochiStretch eng;
    eng.setWindowMs (4000.0);
    eng.setPitchSemis (0.0);
    eng.setMix01 (1.0);
    eng.setHold (false);
    eng.setSpeed (1.0);
    eng.prepare (fs, 1);

    const double tauS  = 0.080 * fs; // speed glide tau, in SAMPLES
    const long   Lc    = (long) std::llround (0.2 * fs);
    const long   nFlip = (long) std::llround (1.0 * fs);
    const long   H2    = (long) std::llround (0.040 * fs); // window/2; kExtra=0 per [R1]

    std::vector<float> chirp;
    makeChirp (chirp, (size_t) Lc, 300.0, 3000.0, 0.8, fs);

    const long total = 2 * nFlip + Lc + (long) std::llround (0.3 * fs);
    std::vector<float> buf ((size_t) total, 0.0f);
    for (long n = 0; n < Lc; ++n) buf[(size_t) n] = chirp[(size_t) n];

    // Phase 1: run to n_flip at speed=1 (chirp then silence already in buf).
    float* p1 = buf.data();
    eng.process (&p1, 1, (int) nFlip);

    // Phase 2: flip speed at the block boundary, run the remainder.
    eng.setSpeed (-1.0);
    float* p2 = buf.data() + nFlip;
    eng.process (&p2, 1, (int) (total - nFlip));

    // Time-reversed template.
    std::vector<float> revTmpl ((size_t) Lc);
    for (long k = 0; k < Lc; ++k) revTmpl[(size_t) k] = chirp[(size_t) (Lc - 1 - k)];

    const double nRevStartD = 2.0 * (double) nFlip - (double) Lc + 2.0 * tauS + (double) H2;
    const long   delta = (long) std::llround (0.008 * fs);
    const long   center = (long) std::llround (nRevStartD);
    // [reviewer-adjudicated] The reversed segment's group delay is NOT a
    // constant window/2 through the variable-ratio flip: PitchShifter's phase
    // integrator drifts while comp(sSm) sweeps up (as sSm glides through
    // small magnitudes toward the -1 target, comp(s)=1/clamp(|s|,kCompMin,
    // kCompMax) briefly reaches kCompMax=... up to 1/kCompMin=8x), smearing
    // the true reversed-segment lag across up to a further window's worth of
    // samples. `center` above (== the ORIGINAL transport-only prediction +
    // H2) already sits at reviewerCenter+H2 (H2=window/2), so widening the
    // bracket to [reviewerCenter+window/2-delta, reviewerCenter+3*window/2+delta]
    // (reviewer notation) becomes, in terms of `center`: [center-delta,
    // center+2*H2+delta]. This WIDENS the search bracket only -- the
    // acceptance gates below (rho_rev>=0.7, rho_fwd<=rho_rev/3) are UNCHANGED,
    // so the reversal physics is still fully enforced.
    const long   lo = center - delta;
    const long   hi = center + 2 * H2 + delta;

    XcorrResult rev = normXcorr (buf, revTmpl, lo, hi);
    XcorrResult fwd = normXcorr (buf, chirp,   lo, hi);

    if (rev.rho < 0.7)
        fail ("engineReversePlaybackTest: rho_rev below 0.7 (fs=" + std::to_string (fs) +
              " rho=" + std::to_string (rev.rho) + ")");
    if (std::abs (fwd.rho) > std::abs (rev.rho) / 3.0)
        fail ("engineReversePlaybackTest: rho_fwd not much smaller than rho_rev (fs=" + std::to_string (fs) +
              " rho_fwd=" + std::to_string (fwd.rho) + " rho_rev=" + std::to_string (rev.rho) + ")");
}

// ---------------------------------------------------------------------
// engineTapeStopTrajectoryTest -- plan §5.5 (#4, tape-stop trajectory)
//
// [RESOLVED per decision D5] The original oracle asserted the crossing
// count matched an INSTANTANEOUS one-pole closed-form integral of
// pitchFactor(s(t),0) to +-3%. Root cause (independent DSP review):
// kShifterWindowMs == kSpeedGlideTauMs (both 80 ms), so PitchShifter's own
// rotating-head window-averaging lags the speed glide it is being fed,
// producing a +30-59% transient pitch "smear" relative to that
// closed-form prediction during the fast part of the transient. D5
// ACCEPTS this smear as tape-stop CHARACTER (not a bug to fix by
// re-tuning kShifterWindowMs/kSpeedGlideTauMs apart) and REFRAMES this
// test to a FORMAT-INDEPENDENT behavioural gate that does not pin an
// absolute count: it measures the zero-crossing rate over successive
// short segments as speed glides 1->0 and asserts
//   (a) the rate trends DOWNWARD, successive segment over successive
//       segment (small measurement-noise wobble tolerated, but never a
//       real increase),
//   (b) the overall drop from the first to the last segment is
//       substantial -- genuine tape-stop character, not a flat no-op,
//   (c) the LATE segments settle toward a stable value -- the tail's
//       internal wobble is small relative to the overall drop already
//       established by (b), i.e. the trajectory converges toward an
//       asymptotic floor rather than continuing to change at the same
//       pace (independent review: cumulative crossings settle near ~246
//       at T=0.50s for this f0/tau/window combination; that specific
//       count is NOT asserted here -- only the qualitative settling
//       trend it reflects, so the gate stays valid if window/tau ever
//       change).
// A broken tape-stop that increases pitch, or that never falls, still
// fails (a)-(b) unconditionally. The STEADY-STATE pitch law (test #2,
// enginePitchLawTest) already passes and is unchanged by this reframe.
// ---------------------------------------------------------------------
static void engineTapeStopTrajectoryTest (double fs)
{
    fc::MochiStretch eng;
    eng.setWindowMs (2000.0);
    eng.setPitchSemis (0.0);
    eng.setMix01 (1.0);
    eng.setHold (false);
    eng.setSpeed (1.0);
    eng.prepare (fs, 1);

    const double f0  = 1000.0;
    const double A   = 0.5;

    const long nStep    = (long) std::llround (0.5 * fs);
    const double Tmax   = 0.5;
    const long nMeasure = (long) std::llround (Tmax * fs);
    const long total    = nStep + nMeasure;

    std::vector<float> buf ((size_t) total);
    for (long n = 0; n < total; ++n)
        buf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * f0 * (double) n / fs));

    float* p1 = buf.data();
    eng.process (&p1, 1, (int) nStep);

    eng.setSpeed (0.0);
    float* p2 = buf.data() + nStep;
    eng.process (&p2, 1, (int) nMeasure);

    // Four equal successive segments spanning the whole glide-to-stop
    // window (125 ms each at Tmax=0.5s) -- "successive short segments" per
    // D5's reframe. hyst matches the plan's existing 0.1*A crossing
    // hysteresis convention used throughout this file.
    const int    kSegments = 4;
    const long   segLen    = nMeasure / kSegments;
    const double segSec    = (double) segLen / fs;
    const double hyst      = 0.1 * A;

    double rate[kSegments];
    for (int i = 0; i < kSegments; ++i)
    {
        const int crossings = countPosCrossings (buf, (size_t) (nStep + (long) i * segLen),
                                                  (size_t) segLen, hyst);
        rate[i] = (double) crossings / segSec;
    }

    // (a) Downward trend, successive segments, small-noise tolerance: a
    // real regression (pitch flat or rising) blows well past this margin.
    const double kTrendTol = 0.10; // 10% wobble allowance per adjacent pair
    for (int i = 1; i < kSegments; ++i)
    {
        if (rate[i] > rate[i - 1] * (1.0 + kTrendTol))
            fail ("engineTapeStopTrajectoryTest: crossing-rate rose seg " + std::to_string (i - 1) +
                  "->" + std::to_string (i) + " (fs=" + std::to_string (fs) +
                  " rate[i-1]=" + std::to_string (rate[i - 1]) + " rate[i]=" + std::to_string (rate[i]) + ")");
    }

    // (b) Substantial overall drop, first segment -> last segment: genuine
    // tape-stop character, not noise/flatline. A broken engine that
    // increases pitch, or holds it flat, fails this unconditionally.
    const double kFloorFrac = 0.7; // last segment must be <=70% of first (>=30% drop)
    if (rate[kSegments - 1] > kFloorFrac * rate[0])
        fail ("engineTapeStopTrajectoryTest: insufficient pitch drop toward tape-stop floor (fs=" +
              std::to_string (fs) + " rate0=" + std::to_string (rate[0]) +
              " rateLast=" + std::to_string (rate[kSegments - 1]) + ")");

    // (c) Settling / asymptotic convergence: split the LAST segment into
    // two halves and confirm the residual tail wobble is small relative
    // to the overall drop already established by (b) -- i.e. the
    // trajectory has substantially levelled off by T=Tmax, converging
    // toward a floor, rather than still changing at the same pace.
    const long halfLen = segLen / 2;
    if (halfLen > 0)
    {
        const long   lastSegStart = nStep + (long) (kSegments - 1) * segLen;
        const double halfSec      = (double) halfLen / fs;
        const int crossA = countPosCrossings (buf, (size_t) lastSegStart, (size_t) halfLen, hyst);
        const int crossB = countPosCrossings (buf, (size_t) (lastSegStart + halfLen), (size_t) halfLen, hyst);
        const double rateA = (double) crossA / halfSec;
        const double rateB = (double) crossB / halfSec;
        const double tailWobble  = std::abs (rateB - rateA);
        const double overallDrop = rate[0] - rate[kSegments - 1];

        const double kConvergeFrac = 0.25; // tail wobble <= 25% of the total observed drop
        if (overallDrop > 1e-9 && tailWobble > kConvergeFrac * overallDrop)
            fail ("engineTapeStopTrajectoryTest: tail has not settled toward an asymptotic floor (fs=" +
                  std::to_string (fs) + " tailWobble=" + std::to_string (tailWobble) +
                  " overallDrop=" + std::to_string (overallDrop) + ")");
    }
}

// ---------------------------------------------------------------------
// engineHoldLoopPeriodicityTest -- plan §5.6 (#5, HOLD loop periodicity)
// ---------------------------------------------------------------------
static void engineHoldLoopPeriodicityTest (double fs)
{
    fc::MochiStretch eng;
    eng.setWindowMs (1000.0);
    eng.setSpeed (1.0);
    eng.setPitchSemis (0.0);
    eng.setMix01 (1.0);
    eng.setHold (false);
    eng.prepare (fs, 1);

    const long Wlen  = (long) std::llround (1000.0 * fs / 1000.0); // == llround(fs)
    const long Ploop = Wlen - 1;
    const long Wfade = (long) std::llround (0.010 * fs);
    const double fFrozen = 500.0;
    const double fNew    = 1300.0;
    const double A       = 0.6;

    const long recordLen = Wlen + (long) std::llround (0.1 * fs);
    std::vector<float> recBuf ((size_t) recordLen);
    for (long n = 0; n < recordLen; ++n)
        recBuf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * fFrozen * (double) n / fs));

    float* p1 = recBuf.data();
    eng.process (&p1, 1, (int) recordLen);

    eng.setHold (true);

    const long skipLen    = (long) std::llround (0.05 * fs);
    const long measureLen = 4 * Ploop + 2 * Wfade;
    const long holdTotal  = skipLen + measureLen;

    std::vector<float> holdBuf ((size_t) holdTotal);
    for (long n = 0; n < holdTotal; ++n)
        holdBuf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * fNew * (double) n / fs));

    float* p2 = holdBuf.data();
    eng.process (&p2, 1, (int) holdTotal);

    std::vector<double> holdBufD (holdBuf.begin(), holdBuf.end());
    if (! fct::allFinite (holdBufD))
        fail ("engineHoldLoopPeriodicityTest: non-finite output (fs=" + std::to_string (fs) + ")");
    if (fct::peakAbs (holdBufD) > 1.2)
        fail ("engineHoldLoopPeriodicityTest: peak exceeds 1.2 (fs=" + std::to_string (fs) + ")");

    const size_t base = (size_t) skipLen;

    // (a) periodicity: wet[n+Ploop] == wet[n], bit-exact, over the measured window.
    const size_t measureCount = (size_t) measureLen - (size_t) Ploop;
    bool periodOk = true;
    for (size_t n = 0; n < measureCount; ++n)
    {
        if (holdBuf[base + n] != holdBuf[base + n + (size_t) Ploop]) { periodOk = false; break; }
    }
    if (! periodOk)
        fail ("engineHoldLoopPeriodicityTest: wet[n+Ploop] != wet[n] bit-exact (fs=" + std::to_string (fs) + ")");

    // Measured minimal period: quick-reject candidates on a short prefix, full-verify survivors.
    {
        const size_t prefixLen = std::min<size_t> ((size_t) measureLen, 256);
        long foundP = -1;
        for (long P = 1; P < Ploop; ++P)
        {
            const size_t cnt = std::min (prefixLen, (size_t) measureLen - (size_t) P);
            if (cnt == 0) continue;
            bool ok = true;
            for (size_t n = 0; n < cnt; ++n)
                if (holdBuf[base + n] != holdBuf[base + n + (size_t) P]) { ok = false; break; }
            if (ok)
            {
                const size_t fullCnt = (size_t) measureLen - (size_t) P;
                bool fullOk = true;
                for (size_t n = 0; n < fullCnt; ++n)
                    if (holdBuf[base + n] != holdBuf[base + n + (size_t) P]) { fullOk = false; break; }
                if (fullOk) { foundP = P; break; }
            }
        }
        if (foundP == -1) foundP = Ploop; // Ploop already verified as a valid period above
        if (foundP != Ploop)
            fail ("engineHoldLoopPeriodicityTest: measured minimal period != Wlen-1 (fs=" + std::to_string (fs) +
                  " got=" + std::to_string (foundP) + " expected=" + std::to_string (Ploop) + ")");
    }

    // (b) input non-recording (Goertzel over the measured window).
    const double magFrozen = goertzelMag (holdBuf, base, (size_t) measureLen, fFrozen, fs);
    const double magNew    = goertzelMag (holdBuf, base, (size_t) measureLen, fNew, fs);
    if (magFrozen < 0.3 * A)
        fail ("engineHoldLoopPeriodicityTest: frozen tone below floor (fs=" + std::to_string (fs) + ")");
    if (magNew > 0.02 * magFrozen)
        fail ("engineHoldLoopPeriodicityTest: new tone leaking into HOLD output (fs=" + std::to_string (fs) + ")");
}

// ---------------------------------------------------------------------
// engineWrapCrossfadeContinuityTest -- plan §5.7 (#6, click-free wrap crossfade)
// ---------------------------------------------------------------------
static void engineWrapCrossfadeContinuityTest (double fs)
{
    fc::MochiStretch eng;
    eng.setWindowMs (200.0);
    eng.setSpeed (1.0);
    eng.setPitchSemis (0.0);
    eng.setMix01 (1.0);
    eng.setHold (false);
    eng.prepare (fs, 1);

    const long Wlen  = (long) std::llround (0.2 * fs);
    const long Wfade = (long) std::llround (0.010 * fs);
    const double A   = 0.5;
    const int kcyc   = 20;
    const double f0  = (kcyc + 0.5) * fs / (double) (Wlen - 1);

    const long margin    = Wfade;
    const long recordLen = Wlen + margin;

    const int  kWrapsToCheck = 3;
    const long holdLen = (long) kWrapsToCheck * (Wlen - 1) + 2 * Wfade + 50;

    std::vector<float> buf ((size_t) (recordLen + holdLen), 0.0f);
    for (long n = 0; n < recordLen; ++n)
        buf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * f0 * (double) n / fs));
    // Hold-phase input: silence (buffer already zero-initialised there).

    float* p1 = buf.data();
    eng.process (&p1, 1, (int) recordLen);

    eng.setHold (true);

    float* p2 = buf.data() + recordLen;
    eng.process (&p2, 1, (int) holdLen);

    std::vector<double> bufD (buf.begin(), buf.end());
    if (! fct::allFinite (bufD))
        fail ("engineWrapCrossfadeContinuityTest: non-finite output (fs=" + std::to_string (fs) + ")");
    if (fct::peakAbs (bufD) > 1.2)
        fail ("engineWrapCrossfadeContinuityTest: peak exceeds 1.2 (fs=" + std::to_string (fs) + ")");

    for (int k = 0; k < kWrapsToCheck; ++k)
    {
        const long wrapIdx = recordLen + (long) k * (Wlen - 1);
        const long lo = std::max<long> (1, wrapIdx - Wfade - 8);
        const long hi = std::min<long> ((long) buf.size() - 1, wrapIdx + Wfade + 8);

        double maxDelta = 0.0;
        for (long n = lo; n <= hi; ++n)
            maxDelta = std::max (maxDelta, std::abs ((double) buf[(size_t) n] - (double) buf[(size_t) (n - 1)]));

        if (maxDelta > 0.05 * A)
            fail ("engineWrapCrossfadeContinuityTest: click at wrap k=" + std::to_string (k) +
                  " fs=" + std::to_string (fs) + " maxDelta=" + std::to_string (maxDelta));
    }
}

// ---------------------------------------------------------------------
// engineLongHoldPeakTest -- plan §5.8 (#7, long-hold worst-case peak bound)
// ---------------------------------------------------------------------
static void engineLongHoldPeakTest (double fs)
{
    fc::MochiStretch eng;
    eng.setSpeed (1.0);
    eng.setPitchSemis (0.0);
    eng.setWindowMs (1000.0);
    eng.setMix01 (0.5);
    eng.setHold (false);
    eng.prepare (fs, 2);

    const long blockLen = (long) std::llround (0.050 * fs); // 50 ms churn granularity
    const long totalLen = (long) std::llround (8.0 * fs);
    const long numBlocks = totalLen / blockLen;

    static const double speedMags[] = { 2.0, 1.5, 1.0, 0.5, 0.25, 0.05 };
    static const double pitches[]   = { -12.0, -6.0, 0.0, 6.0, 12.0, -12.0 };
    static const double windows[]   = { 100.0, 4000.0, 5000.0, 50.0, 500.0, 2000.0 };
    static const double mixes[]     = { 0.0, 1.0, 0.5, 0.25, 0.75 };

    Lcg lcgL (0x51ED270BULL), lcgR (0x2545F491ULL);
    std::vector<float> bufL ((size_t) totalLen), bufR ((size_t) totalLen);
    for (long n = 0; n < totalLen; ++n)
    {
        bufL[(size_t) n] = (float) lcgL.next();
        bufR[(size_t) n] = (float) lcgR.next();
    }

    int  sign = 1;
    bool burstDone = false;

    long offset = 0;
    for (long b = 0; b < numBlocks; ++b)
    {
        const long h = std::min (blockLen, totalLen - offset);
        if (h <= 0) break;

        if (b % 6 == 0 && b != 0) sign = -sign; // direction reversal every ~0.3s

        const double speed = sign * speedMags[(size_t) b % (sizeof (speedMags) / sizeof (double))];
        eng.setSpeed (speed);
        eng.setPitchSemis (pitches[(size_t) b % (sizeof (pitches) / sizeof (double))]);
        eng.setWindowMs (windows[(size_t) b % (sizeof (windows) / sizeof (double))]);
        eng.setMix01 (mixes[(size_t) b % (sizeof (mixes) / sizeof (double))]);

        // Hold toggling every ~0.7s (14 blocks); one burst does on/off/on at 50ms spacing.
        if (! burstDone && b == 28)          { eng.setHold (true);  burstDone = true; }
        else if (burstDone && b == 29)        eng.setHold (false);
        else if (burstDone && b == 30)        eng.setHold (true);
        else if (b % 14 == 0 && b != 28)      eng.setHold ((b / 14) % 2 == 1);

        float* ptrs[2] = { bufL.data() + offset, bufR.data() + offset };
        eng.process (ptrs, 2, (int) h);

        offset += h;
    }
    if (offset < totalLen)
    {
        float* ptrs[2] = { bufL.data() + offset, bufR.data() + offset };
        eng.process (ptrs, 2, (int) (totalLen - offset));
    }

    std::vector<double> allD;
    allD.reserve ((size_t) totalLen * 2);
    for (long n = 0; n < totalLen; ++n) { allD.push_back (bufL[(size_t) n]); allD.push_back (bufR[(size_t) n]); }

    if (! fct::allFinite (allD))
        fail ("engineLongHoldPeakTest: non-finite during churn (fs=" + std::to_string (fs) + ")");
    const double peak = fct::peakAbs (allD);
    if (peak > fc::MochiStretch::kPeakBound)
        fail ("engineLongHoldPeakTest: peak " + std::to_string (peak) + " exceeds kPeakBound (fs=" + std::to_string (fs) + ")");

    // Post-churn: settle, then verify the output decays to silence (no-feedback stability, §0.2).
    // [reviewer-adjudicated] age-recovery is bounded to <=1 sample/sample
    // (dA/dt>=-1), and worst-case churn can freeze `age` near
    // kMaxWindowSec*fs = 4*fs, so the post-churn silence legitimately takes
    // up to ~4s (measured ~3.6s at 44.1k) to flush to bit-zero -- matching
    // the declared getTailLengthSeconds()=5.0. 1.0*fs was too short a
    // silence-tail observation window (test-timing bug, not a loosening --
    // the engine actually reaches exact 0; the tailPeak<=1e-3 gate below is
    // UNCHANGED).
    eng.setMix01 (1.0);
    eng.setSpeed (1.0);
    eng.setHold (false);

    const long silenceLen = (long) std::llround (5.0 * fs);
    std::vector<float> silL ((size_t) silenceLen, 0.0f), silR ((size_t) silenceLen, 0.0f);
    float* sptrs[2] = { silL.data(), silR.data() };
    eng.process (sptrs, 2, (int) silenceLen);

    const long tailStart = silenceLen - (long) std::llround (0.3 * fs);
    std::vector<double> tailD;
    for (long n = tailStart; n < silenceLen; ++n) { tailD.push_back (silL[(size_t) n]); tailD.push_back (silR[(size_t) n]); }
    const double tailPeak = fct::peakAbs (tailD);
    if (tailPeak > 1e-3)
        fail ("engineLongHoldPeakTest: post-churn silence tail did not decay (fs=" + std::to_string (fs) +
              " peak=" + std::to_string (tailPeak) + ")");
}

// ---------------------------------------------------------------------
// engineWindowChangeRecoveryTest -- plan §5.9 (#9, out-of-domain recovery on
// a live W_len change).
//
// [reviewer-adjudicated redesign] Two independent reviewers confirmed the
// ORIGINAL test (speed=0.5, 1.0s pre-change, windowStartMs=4000->100) never
// drove A out of domain: A never reaches the OLD Wlen (=4000ms worth of
// samples) before the shrink, so WlenPending is never adopted at a wrap and
// the domain-overflow/recovery path is never exercised (a VACUOUS pass).
// This redesign instead derives the start window and phase lengths directly
// from the transport dynamics (dA/dt = 1-sSm, constant at speed=0.5; wrap at
// A>Wlen -> A-=(Wlen-1), only adopting WlenPending AT that wrap): pick a
// windowStartMs small enough, relative to a phase1 spanning several full
// wrap periods, that wrapping is ALREADY periodic ("active") when the
// change lands -- so the very next wrap adopts the new Wlen while A is
// still near the OLD (much larger) Wlen, forcing a genuine, large domain
// overshoot that the engine must correct across several subsequent wraps.
// The original fragile `reverseSpike > forwardSpike` relative comparison is
// REPLACED with absolute bounds only (finite, peak<=kPeakBound, a spike
// bound over the guaranteed-to-contain-the-adopting-wrap window, and pitch
// re-lock after settling) -- tightening, not loosening.
// ---------------------------------------------------------------------
static void engineWindowChangeRecoveryTest (double fs)
{
    const double f0    = 1000.0;
    const double A     = 0.5;
    const double kSpeed = 0.5; // dA/dt = 1-kSpeed = 0.5 (constant; sSm snaps to target in reset())

    // One wrap-cycle period (samples), climbing from kAgeMin to Wlen at the
    // constant dA/dt above -- see the header's transport dynamics contract.
    auto periodSeconds = [&] (double windowMs)
    {
        const double wlen = (double) std::llround (windowMs * fs / 1000.0);
        const double periodSamples = (wlen - 1.0) / (1.0 - kSpeed);
        return periodSamples / fs;
    };

    auto runOnce = [&] (double windowStartMs, double windowAfterMs)
    {
        fc::MochiStretch eng;
        eng.setSpeed (kSpeed);
        eng.setPitchSemis (0.0);
        eng.setMix01 (1.0);
        eng.setHold (false);
        eng.setWindowMs (windowStartMs);
        eng.prepare (fs, 1);

        const double periodStartSec = periodSeconds (windowStartMs);

        // phase1: several full wrap cycles at the START window, so wrapping
        // is demonstrably ACTIVE (periodic, not a one-off) at the instant of
        // the change -- not just "long enough to maybe catch one".
        const long phase1Len = (long) std::llround (4.0 * periodStartSec * fs);

        // phase2: worst-case wait for the NEXT natural wrap is one full
        // start-window period (the active Wlen only updates AT a wrap, so
        // the change can land anywhere in the current cycle), plus settle +
        // measurement margin for the post-recovery pitch check.
        const double worstWaitSec = periodStartSec;
        const double settleSec    = 0.3;
        const double measureSec   = 0.5;
        const double marginSec    = 0.3;
        const long phase2Len = (long) std::llround ((worstWaitSec + settleSec + measureSec + marginSec) * fs);

        const long total = phase1Len + phase2Len;
        std::vector<float> buf ((size_t) total);
        for (long n = 0; n < total; ++n)
            buf[(size_t) n] = (float) (A * std::sin (2.0 * kPi * f0 * (double) n / fs));

        float* p1 = buf.data();
        eng.process (&p1, 1, (int) phase1Len);

        eng.setWindowMs (windowAfterMs);

        float* p2 = buf.data() + phase1Len;
        eng.process (&p2, 1, (int) phase2Len);

        std::vector<double> bufD (buf.begin(), buf.end());
        if (! fct::allFinite (bufD))
            fail ("engineWindowChangeRecoveryTest: non-finite (fs=" + std::to_string (fs) +
                  " " + std::to_string (windowStartMs) + "->" + std::to_string (windowAfterMs) + ")");
        if (fct::peakAbs (bufD) > fc::MochiStretch::kPeakBound)
            fail ("engineWindowChangeRecoveryTest: peak exceeds kPeakBound (fs=" + std::to_string (fs) +
                  " " + std::to_string (windowStartMs) + "->" + std::to_string (windowAfterMs) + ")");

        // Spike bound over the ENTIRE "waiting for the adopting wrap, then
        // recovering across a few corrective wraps" window: the adopting
        // wrap is guaranteed (by the period-cycle argument above) to land
        // within one worstWaitSec of the change; a fadeLen margin covers the
        // crossfade that wrap itself spawns.
        const long fadeLenSamples = std::max<long> (1, (long) std::llround (0.010 * fs));
        const long spikeWindowLen = (long) std::llround (worstWaitSec * fs) + fadeLenSamples + 64;
        const long spikeLo = phase1Len;
        const long spikeHi = std::min<long> (total - 1, phase1Len + spikeWindowLen);

        double maxSpike = 0.0;
        for (long n = std::max<long> (1, spikeLo); n <= spikeHi; ++n)
            maxSpike = std::max (maxSpike, std::abs ((double) buf[(size_t) n] - (double) buf[(size_t) (n - 1)]));

        if (maxSpike > 0.5 * A)
            fail ("engineWindowChangeRecoveryTest: recovery-transient spike exceeds 0.5A (fs=" + std::to_string (fs) +
                  " " + std::to_string (windowStartMs) + "->" + std::to_string (windowAfterMs) +
                  " spike=" + std::to_string (maxSpike) + ")");

        // Pitch recovery: well past the worst-case wrap timing, the engine
        // must have re-locked onto the steady pitchFactor(kSpeed,0) tone
        // (==f0, since |kSpeed|=0.5 is inside [kCompMin,kCompMax] -> unity)
        // -- i.e. A has settled back into normal periodic operation under
        // the NEW Wlen.
        const long measureStart = phase1Len + (long) std::llround ((worstWaitSec + settleSec) * fs);
        const long measureLen   = (long) std::llround (measureSec * fs);
        const long measureEnd   = std::min<long> (total, measureStart + measureLen);
        const long actualLen    = measureEnd - measureStart;
        if (actualLen > 0)
        {
            const double f_true = f0 * pitchFactorRef (kSpeed, 0.0);
            const double mag = goertzelMag (buf, (size_t) measureStart, (size_t) actualLen, f_true, fs);
            if (mag < 0.3 * A)
                fail ("engineWindowChangeRecoveryTest: pitch not recovered after transient (fs=" + std::to_string (fs) +
                      " " + std::to_string (windowStartMs) + "->" + std::to_string (windowAfterMs) + ")");
        }
        else
        {
            fail ("engineWindowChangeRecoveryTest: measurement window does not fit (test bug, fs=" + std::to_string (fs) + ")");
        }
    };

    // Primary case: GENUINE shrink-triggered domain overshoot. windowStartMs
    // is small enough, relative to phase1's several wrap periods, that the
    // adopting wrap lands with A near the OLD (5x bigger) Wlen while the new
    // Wlen is already much smaller -- A massively overshoots [1, Wlen_new]
    // and must recover across several corrective wraps (the ">Wlen" branch).
    runOnce (500.0, 100.0);

    // Secondary case: growth exercises the SAME wrap-check code's OTHER
    // branch (A < kAgeMin, since adopting a much BIGGER Wlen right after an
    // old-Wlen-triggered wrap drives A deeply negative until the next
    // sample's "<kAgeMin" correction fires). Same absolute bounds; no
    // relative comparison between the two cases (the plan's original
    // reverseSpike > forwardSpike comparison was fragile/ill-founded and is
    // not used here).
    runOnce (100.0, 500.0);
}

// ---------------------------------------------------------------------
// engineDeterminismTest -- plan §5.10 (#8, 2-run bit-exact determinism)
// ---------------------------------------------------------------------
static void engineDeterminismTest (double fs)
{
    auto runScenario = [&] (std::vector<float>& outL, std::vector<float>& outR)
    {
        fc::MochiStretch eng;
        eng.setSpeed (1.0);
        eng.setPitchSemis (0.0);
        eng.setWindowMs (1000.0);
        eng.setMix01 (0.5);
        eng.setHold (false);
        eng.prepare (fs, 2);

        const long total = (long) std::llround (1.5 * fs);
        Lcg lcgL (0x1111111111ULL), lcgR (0x2222222222ULL);
        outL.assign ((size_t) total, 0.0f);
        outR.assign ((size_t) total, 0.0f);
        for (long n = 0; n < total; ++n)
        {
            outL[(size_t) n] = (float) (0.5 * lcgL.next());
            outR[(size_t) n] = (float) (0.5 * lcgR.next());
        }

        long offset = 0;
        size_t chunkIdx = 0;
        const size_t numChunks = sizeof (kPrimeChunks) / sizeof (int);
        int step = 0;
        while (offset < total)
        {
            const long h = std::min<long> (kPrimeChunks[chunkIdx % numChunks], total - offset);

            switch (step % 6)
            {
                case 0: eng.setSpeed (-1.0); break;
                case 1: eng.setPitchSemis (7.0); break;
                case 2: eng.setWindowMs (300.0); break;
                case 3: eng.setHold (true); break;
                case 4: eng.setHold (false); break;
                case 5: eng.setMix01 (0.8); break;
                default: break;
            }
            // One explicit freeze round-trip (on->off->on) partway through.
            if (step == 12) eng.setHold (true);
            if (step == 13) eng.setHold (false);
            if (step == 14) eng.setHold (true);

            float* ptrs[2] = { outL.data() + offset, outR.data() + offset };
            eng.process (ptrs, 2, (int) h);

            offset += h;
            ++chunkIdx;
            ++step;
        }
    };

    std::vector<float> aL, aR, bL, bR;
    runScenario (aL, aR);
    runScenario (bL, bR);

    if (aL.size() != bL.size() ||
        std::memcmp (aL.data(), bL.data(), sizeof (float) * aL.size()) != 0 ||
        std::memcmp (aR.data(), bR.data(), sizeof (float) * aR.size()) != 0)
        fail ("engineDeterminismTest: two runs not bit-identical (fs=" + std::to_string (fs) + ")");
}

// ---------------------------------------------------------------------
// engineParamNanGuardTest -- plan §5.11 (#8, param-NaN A/B bit-exact)
// ---------------------------------------------------------------------
static void engineParamNanGuardTest (double fs)
{
    auto runScenario = [&] (bool injectNan, std::vector<float>& outL, std::vector<float>& outR)
    {
        fc::MochiStretch eng;
        eng.setSpeed (0.8);
        eng.setPitchSemis (-3.0);
        eng.setWindowMs (700.0);
        eng.setMix01 (0.6);
        eng.setHold (false);
        eng.prepare (fs, 2);

        const long total = (long) std::llround (2.0 * fs);
        Lcg lcgL (0x3333333333ULL), lcgR (0x4444444444ULL);
        outL.assign ((size_t) total, 0.0f);
        outR.assign ((size_t) total, 0.0f);
        for (long n = 0; n < total; ++n)
        {
            outL[(size_t) n] = (float) (0.5 * lcgL.next());
            outR[(size_t) n] = (float) (0.5 * lcgR.next());
        }

        const double nanRotation[] = {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()
        };

        long offset = 0;
        size_t chunkIdx = 0;
        const size_t numChunks = sizeof (kPrimeChunks) / sizeof (int);
        int step = 0;
        while (offset < total)
        {
            const long h = std::min<long> (kPrimeChunks[chunkIdx % numChunks], total - offset);

            switch (step % 5)
            {
                case 0: eng.setSpeed (-0.5 + 0.1 * (step % 7)); break;
                case 1: eng.setPitchSemis (5.0 - (double) (step % 5)); break;
                case 2: eng.setWindowMs (200.0 + 50.0 * (step % 6)); break;
                case 3: eng.setHold ((step / 5) % 3 == 0); break;
                case 4: eng.setMix01 (0.2 + 0.1 * (step % 6)); break;
                default: break;
            }

            if (injectNan)
            {
                const double bad = nanRotation[(size_t) step % 3];
                eng.setSpeed (bad);
                eng.setPitchSemis (bad);
                eng.setWindowMs (bad);
                eng.setMix01 (bad);
            }

            float* ptrs[2] = { outL.data() + offset, outR.data() + offset };
            eng.process (ptrs, 2, (int) h);

            offset += h;
            ++chunkIdx;
            ++step;
        }
    };

    std::vector<float> aL, aR, bL, bR;
    runScenario (false, aL, aR);
    runScenario (true,  bL, bR);

    if (aL.size() != bL.size() ||
        std::memcmp (aL.data(), bL.data(), sizeof (float) * aL.size()) != 0 ||
        std::memcmp (aR.data(), bR.data(), sizeof (float) * aR.size()) != 0)
        fail ("engineParamNanGuardTest: param-NaN A/B not bit-identical (fs=" + std::to_string (fs) + ")");
}

// ---------------------------------------------------------------------
// engineNanRecoveryTest -- plan §5.12 (#8, input NaN/Inf recovery)
// ---------------------------------------------------------------------
static void engineNanRecoveryTest (double fs)
{
    fc::MochiStretch eng;
    eng.setSpeed (0.5);
    eng.setPitchSemis (3.0);
    eng.setMix01 (0.7);
    eng.setWindowMs (800.0);
    eng.setHold (false);
    eng.prepare (fs, 2);

    const long total = (long) std::llround (2.0 * fs);
    Lcg lcgL (0x5555555555ULL), lcgR (0x6666666666ULL);
    std::vector<float> bufL ((size_t) total), bufR ((size_t) total);
    for (long n = 0; n < total; ++n)
    {
        bufL[(size_t) n] = (float) (0.5 * lcgL.next());
        bufR[(size_t) n] = (float) (0.5 * lcgR.next());
    }

    const long holdStart = total / 3;
    const long holdEnd   = 2 * total / 3;

    const long inj1 = total / 4, inj2 = total / 2, inj3 = (3 * total) / 4;
    bufL[(size_t) inj1] = std::numeric_limits<float>::quiet_NaN();
    bufR[(size_t) inj1] = std::numeric_limits<float>::quiet_NaN();
    bufL[(size_t) inj2] = std::numeric_limits<float>::infinity();
    bufR[(size_t) inj2] = std::numeric_limits<float>::infinity();
    bufL[(size_t) inj3] = -std::numeric_limits<float>::infinity();
    bufR[(size_t) inj3] = -std::numeric_limits<float>::infinity();

    auto processSegment = [&] (long start, long len)
    {
        long off = start;
        size_t chunkIdx = 0;
        const size_t numChunks = sizeof (kPrimeChunks) / sizeof (int);
        while (off < start + len)
        {
            const long h = std::min<long> (kPrimeChunks[chunkIdx % numChunks], start + len - off);
            float* ptrs[2] = { bufL.data() + off, bufR.data() + off };
            eng.process (ptrs, 2, (int) h);
            off += h;
            ++chunkIdx;
        }
    };

    processSegment (0, holdStart);
    eng.setHold (true);
    processSegment (holdStart, holdEnd - holdStart);
    eng.setHold (false);
    processSegment (holdEnd, total - holdEnd);

    // [R5 confirmed] gate applies over the FULL output, injected indices included.
    std::vector<double> full;
    full.reserve ((size_t) total * 2);
    for (long n = 0; n < total; ++n) { full.push_back (bufL[(size_t) n]); full.push_back (bufR[(size_t) n]); }

    if (! fct::allFinite (full))
        fail ("engineNanRecoveryTest: non-finite output anywhere in stream, incl. injected indices (fs=" +
              std::to_string (fs) + ")");
    if (fct::peakAbs (full) > 1.2)
        fail ("engineNanRecoveryTest: peak exceeds 1.2 anywhere in stream (fs=" + std::to_string (fs) + ")");
}

// ---------------------------------------------------------------------
// engineResetResidueTest -- plan §5.13 (#8, reset residue + silence->0 +
// prepare re-init)
// ---------------------------------------------------------------------
static void engineResetResidueTest (double fs)
{
    // (1) reset residue: excite all paths, explicit setHold(false), reset(),
    //     then silence -> ~0.
    {
        fc::MochiStretch eng;
        eng.setSpeed (0.5);
        eng.setPitchSemis (5.0);
        eng.setWindowMs (600.0);
        eng.setMix01 (0.8);
        eng.setHold (false);
        eng.prepare (fs, 2);

        const long exciteLen = (long) std::llround (1.5 * fs);
        Lcg lcgL (0x7777777777ULL), lcgR (0x8888888888ULL);
        std::vector<float> bufL ((size_t) exciteLen), bufR ((size_t) exciteLen);
        for (long n = 0; n < exciteLen; ++n)
        {
            bufL[(size_t) n] = (float) (0.7 * lcgL.next());
            bufR[(size_t) n] = (float) (0.7 * lcgR.next());
        }

        const long seg = exciteLen / 3;
        float* p1[2] = { bufL.data(), bufR.data() };
        eng.process (p1, 2, (int) seg);
        eng.setSpeed (-1.2);
        eng.setHold (true);
        float* p2[2] = { bufL.data() + seg, bufR.data() + seg };
        eng.process (p2, 2, (int) seg);
        eng.setHold (false);
        eng.setPitchSemis (-8.0);
        float* p3[2] = { bufL.data() + 2 * seg, bufR.data() + 2 * seg };
        eng.process (p3, 2, (int) (exciteLen - 2 * seg));

        eng.setHold (false); // explicit per plan, before reset()
        eng.reset();

        const long silLen = (long) std::llround (0.5 * fs);
        std::vector<float> silL ((size_t) silLen, 0.0f), silR ((size_t) silLen, 0.0f);
        float* sp[2] = { silL.data(), silR.data() };
        eng.process (sp, 2, (int) silLen);

        std::vector<double> silD;
        for (long n = 0; n < silLen; ++n) { silD.push_back (silL[(size_t) n]); silD.push_back (silR[(size_t) n]); }
        if (fct::peakAbs (silD) > 1e-12)
            fail ("engineResetResidueTest(1): reset residue > 1e-12 (fs=" + std::to_string (fs) + ")");
    }

    // (2) fresh prepare, silence from the start -> exact silence (class J floor).
    {
        fc::MochiStretch eng;
        eng.setHold (false);
        eng.prepare (fs, 2);

        const long len = (long) std::llround (1.0 * fs);
        std::vector<float> silL ((size_t) len, 0.0f), silR ((size_t) len, 0.0f);
        float* sp[2] = { silL.data(), silR.data() };
        eng.process (sp, 2, (int) len);

        std::vector<double> silD;
        for (long n = 0; n < len; ++n) { silD.push_back (silL[(size_t) n]); silD.push_back (silR[(size_t) n]); }
        if (fct::peakAbs (silD) > 1e-12)
            fail ("engineResetResidueTest(2): fresh-prepare silence not exact (fs=" + std::to_string (fs) + ")");
    }

    // (3) reset() determinism: identical sequence from reset() vs. fresh prepare,
    //     both explicitly forcing setHold(false) first (hold latches across reset()).
    {
        auto runSeqA = [&] (fc::MochiStretch& eng, std::vector<float>& outL, std::vector<float>& outR)
        {
            eng.setHold (false);
            const long total = (long) std::llround (1.0 * fs);
            Lcg lcgL (0x9999999999ULL), lcgR (0xAAAAAAAAAAULL);
            outL.assign ((size_t) total, 0.0f);
            outR.assign ((size_t) total, 0.0f);
            for (long n = 0; n < total; ++n)
            {
                outL[(size_t) n] = (float) (0.5 * lcgL.next());
                outR[(size_t) n] = (float) (0.5 * lcgR.next());
            }
            const long seg = total / 2;
            float* p1[2] = { outL.data(), outR.data() };
            eng.process (p1, 2, (int) seg);
            eng.setSpeed (0.3);
            float* p2[2] = { outL.data() + seg, outR.data() + seg };
            eng.process (p2, 2, (int) (total - seg));
        };

        fc::MochiStretch engFresh;
        engFresh.setSpeed (1.0);
        engFresh.setPitchSemis (0.0);
        engFresh.setWindowMs (900.0);
        engFresh.setMix01 (0.5);
        engFresh.setHold (false);
        engFresh.prepare (fs, 2);
        std::vector<float> freshL, freshR;
        runSeqA (engFresh, freshL, freshR);

        fc::MochiStretch engReset;
        engReset.setSpeed (1.0);
        engReset.setPitchSemis (0.0);
        engReset.setWindowMs (900.0);
        engReset.setMix01 (0.5);
        engReset.setHold (true); // deliberately latched pre-reset; hold persists across reset()
        engReset.prepare (fs, 2);
        {
            const long warm = (long) std::llround (0.2 * fs);
            std::vector<float> wL ((size_t) warm, 0.3f), wR ((size_t) warm, -0.3f);
            float* wp[2] = { wL.data(), wR.data() };
            engReset.process (wp, 2, (int) warm);
        }
        engReset.reset();
        std::vector<float> resetL, resetR;
        runSeqA (engReset, resetL, resetR);

        if (freshL.size() != resetL.size() ||
            std::memcmp (freshL.data(), resetL.data(), sizeof (float) * freshL.size()) != 0 ||
            std::memcmp (freshR.data(), resetR.data(), sizeof (float) * freshR.size()) != 0)
            fail ("engineResetResidueTest(3): reset() run != fresh-prepare run bit-identical (fs=" +
                  std::to_string (fs) + ")");
    }

    // (4) prepare() re-init: calling prepare() again reproduces a fresh-prepare run.
    {
        fc::MochiStretch engOnce;
        engOnce.setSpeed (0.7);
        engOnce.setPitchSemis (2.0);
        engOnce.setWindowMs (450.0);
        engOnce.setMix01 (0.4);
        engOnce.setHold (false);
        engOnce.prepare (fs, 2);

        const long total = (long) std::llround (0.5 * fs);
        Lcg lcgL (0xBBBBBBBBBBULL), lcgR (0xCCCCCCCCCCULL);
        std::vector<float> inL ((size_t) total), inR ((size_t) total);
        for (long n = 0; n < total; ++n)
        {
            inL[(size_t) n] = (float) (0.5 * lcgL.next());
            inR[(size_t) n] = (float) (0.5 * lcgR.next());
        }

        std::vector<float> onceL = inL, onceR = inR;
        float* p1[2] = { onceL.data(), onceR.data() };
        engOnce.process (p1, 2, (int) total);

        fc::MochiStretch engTwice;
        engTwice.setSpeed (0.7);
        engTwice.setPitchSemis (2.0);
        engTwice.setWindowMs (450.0);
        engTwice.setMix01 (0.4);
        engTwice.setHold (false);
        engTwice.prepare (fs, 2);
        {
            const long warm = (long) std::llround (0.3 * fs);
            std::vector<float> wL ((size_t) warm, 0.2f), wR ((size_t) warm, -0.2f);
            float* wp[2] = { wL.data(), wR.data() };
            engTwice.process (wp, 2, (int) warm);
        }
        engTwice.prepare (fs, 2); // re-init

        std::vector<float> twiceL = inL, twiceR = inR;
        float* p2[2] = { twiceL.data(), twiceR.data() };
        engTwice.process (p2, 2, (int) total);

        if (std::memcmp (onceL.data(), twiceL.data(), sizeof (float) * (size_t) total) != 0 ||
            std::memcmp (onceR.data(), twiceR.data(), sizeof (float) * (size_t) total) != 0)
            fail ("engineResetResidueTest(4): re-prepare() run != fresh-prepare run bit-identical (fs=" +
                  std::to_string (fs) + ")");
    }
}

// ---------------------------------------------------------------------
// engineChunkInvarianceTest -- plan §5.14 (#8, static-parameter chunk invariance)
// ---------------------------------------------------------------------
static void engineChunkInvarianceTest (double fs)
{
    const long total = (long) std::llround (2.0 * fs);
    Lcg lcgL (0xDDDDDDDDDDULL), lcgR (0xEEEEEEEEEEULL);
    std::vector<float> inL ((size_t) total), inR ((size_t) total);
    for (long n = 0; n < total; ++n)
    {
        inL[(size_t) n] = (float) (0.5 * lcgL.next());
        inR[(size_t) n] = (float) (0.5 * lcgR.next());
    }

    auto configureEngine = [&] (fc::MochiStretch& eng)
    {
        eng.setSpeed (0.5);
        eng.setPitchSemis (3.0);
        eng.setWindowMs (800.0);
        eng.setMix01 (0.7);
        eng.setHold (false);
        eng.prepare (fs, 2);
    };

    // (a) fixed 512-sample chunking.
    fc::MochiStretch engA;
    configureEngine (engA);
    std::vector<float> aL = inL, aR = inR;
    {
        long offset = 0;
        while (offset < total)
        {
            const long h = std::min<long> (512, total - offset);
            float* ptrs[2] = { aL.data() + offset, aR.data() + offset };
            engA.process (ptrs, 2, (int) h);
            offset += h;
        }
    }

    // (b) prime-chunk cycling.
    fc::MochiStretch engB;
    configureEngine (engB);
    std::vector<float> bL = inL, bR = inR;
    {
        long offset = 0;
        size_t chunkIdx = 0;
        const size_t numChunks = sizeof (kPrimeChunks) / sizeof (int);
        while (offset < total)
        {
            const long h = std::min<long> (kPrimeChunks[chunkIdx % numChunks], total - offset);
            float* ptrs[2] = { bL.data() + offset, bR.data() + offset };
            engB.process (ptrs, 2, (int) h);
            offset += h;
            ++chunkIdx;
        }
    }

    if (std::memcmp (aL.data(), bL.data(), sizeof (float) * (size_t) total) != 0 ||
        std::memcmp (aR.data(), bR.data(), sizeof (float) * (size_t) total) != 0)
        fail ("engineChunkInvarianceTest: chunk-split not bit-identical for static params (fs=" +
              std::to_string (fs) + ")");
}
