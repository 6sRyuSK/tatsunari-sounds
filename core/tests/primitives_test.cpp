//
// core/tests/primitives_test.cpp — direct, per-primitive spec tests for the
// shared factory_core DSP primitives.
//
// Why this exists: the primitives in core/include/factory_core/ are only ever
// exercised *transitively* through the plugins that compose them. A regression
// in a shared primitive therefore surfaces as several plugin test failures with
// no primitive-level localisation. This suite pins each primitive directly
// against an INDEPENDENT oracle so a break points straight at the primitive.
//
// Conventions (see .claude/skills/write-dsp-test/SKILL.md):
//   * links only factory_core (no JUCE, headless); one CTest case per rate.
//   * accumulate failures in g_failures / fail(), return 1 at the end.
//   * default rate set == factory_core::testing::sampleRatesFromArgs (all 6).
//   * filters evaluated in the z-domain: H(e^jw) is measured as the direct DTFT
//     of the primitive's *measured* impulse response — never from copied
//     coefficient formulas. This is exact for a truncated stable IIR as long as
//     the tail has decayed (we use long captures so it has).
//   * oracles are separate code paths; frequencies are derived from Fs.
//
#include "factory_core/testing/DspInvariants.h"

#include "factory_core/LinkwitzRiley.h"
#include "factory_core/OnePole.h"
#include "factory_core/DelayLine.h"
#include "factory_core/Compressor.h"
#include "factory_core/EnvelopeFollower.h"
#include "factory_core/Crossover5.h"
#include "factory_core/Crossover3.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fct = factory_core::testing;
using factory_core::LinkwitzRiley;
using factory_core::OnePole;
using factory_core::DelayLine;
using factory_core::Compressor;
using factory_core::EnvelopeFollower;
using factory_core::Crossover5;

namespace
{
constexpr double kPi = 3.14159265358979323846;

int g_failures = 0;

void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("FAIL: %s\n", msg.c_str());
}

void check (bool cond, const std::string& msg)
{
    if (! cond)
        fail (msg);
}

// Assert |actual - expected| <= tol, reporting the numbers on failure.
void checkNear (double actual, double expected, double tol, const std::string& what)
{
    if (! (std::abs (actual - expected) <= tol))
        fail (what + ": got " + std::to_string (actual)
              + ", expected " + std::to_string (expected)
              + " +/- " + std::to_string (tol));
}

// ---- z-domain evaluation -------------------------------------------------
//
// H(e^jw) evaluated as the DTFT of a measured impulse response h[n]:
//   H(f) = sum_n h[n] e^{-j w n},  w = 2*pi*f/Fs.
// Uses an incremental phasor (no per-sample std::exp). For a stable IIR
// truncated once its tail is negligible this equals the true transfer function
// to within the (tiny) truncation error; captures below are long enough.
std::complex<double> freqResponse (const std::vector<double>& h, double freqHz, double Fs)
{
    const double w = 2.0 * kPi * freqHz / Fs;
    const std::complex<double> step (std::cos (-w), std::sin (-w));
    std::complex<double> ph (1.0, 0.0), acc (0.0, 0.0);
    for (double v : h)
    {
        acc += v * ph;
        ph *= step;
    }
    return acc;
}

double magDb (const std::complex<double>& H)
{
    return 20.0 * std::log10 (std::max (std::abs (H), 1.0e-300));
}

// Capture the impulse response (length N) of any single-in/single-out process.
std::vector<double> impulseResponse (const std::function<double (double)>& process, int N)
{
    std::vector<double> h ((size_t) N);
    for (int n = 0; n < N; ++n)
        h[(size_t) n] = process (n == 0 ? 1.0 : 0.0);
    return h;
}

// ==========================================================================
// 1. LinkwitzRiley (LR4) — highest blast radius: used by Crossover3 & Crossover5
// ==========================================================================
//
// Spec facts (formula-independent):
//   * LR4 low & high are each -6.02 dB at the crossover fc.
//   * low + high sum to an ALLPASS: |LP(e^jw) + HP(e^jw)| == 1 at all w (this is
//     the defining reconstruction property of Linkwitz-Riley).
//   * allpass() == low+high, so |allpass(e^jw)| == 1 at all w.
//   * an LR4 stopband rolls off asymptotically at 24 dB/oct (a factor-2 step in
//     frequency drops magnitude ~24 dB). 4th order => 24 dB/oct, oracle-free.
void testLinkwitzRiley (double Fs)
{
    const int N = 1 << 16;                 // long: LR4 tail is negligible well before this
    const double fc = 0.02 * Fs;           // rate-derived; keeps 8*fc = 0.16*Fs < Nyquist

    // Capture LP and HP impulse responses from a single impulse pass.
    LinkwitzRiley lr;
    lr.setCutoff (fc, Fs);
    std::vector<double> lpIR ((size_t) N), hpIR ((size_t) N);
    for (int n = 0; n < N; ++n)
    {
        double lo, hi;
        lr.process (n == 0 ? 1.0 : 0.0, lo, hi);
        lpIR[(size_t) n] = lo;
        hpIR[(size_t) n] = hi;
    }

    // (a) -6.02 dB at fc, each band.
    const double lpAtFc = magDb (freqResponse (lpIR, fc, Fs));
    const double hpAtFc = magDb (freqResponse (hpIR, fc, Fs));
    std::printf ("[LR Fs=%.0f] LP(fc)=%.4f dB  HP(fc)=%.4f dB\n", Fs, lpAtFc, hpAtFc);
    checkNear (lpAtFc, -6.0206, 0.2, "LR4 LP magnitude at fc");
    checkNear (hpAtFc, -6.0206, 0.2, "LR4 HP magnitude at fc");

    // (b) flat-magnitude reconstruction: |LP + HP| == 1 across the band.
    double worstRecon = 0.0;
    for (double f : { fc / 8.0, fc / 4.0, fc / 2.0, fc, 2.0 * fc, 4.0 * fc, 8.0 * fc })
    {
        const std::complex<double> sum = freqResponse (lpIR, f, Fs) + freqResponse (hpIR, f, Fs);
        worstRecon = std::max (worstRecon, std::abs (std::abs (sum) - 1.0));
        // Measured worst ~2e-11 (IR-truncation limited); gate at 1e-6, still 5
        // orders of margin but tight enough to catch a real reconstruction break.
        checkNear (std::abs (sum), 1.0, 1.0e-6, "LR4 low+high reconstruction flatness");
    }
    std::printf ("[LR Fs=%.0f] worst |LP+HP|-1 = %.2e\n", Fs, worstRecon);

    // (c) allpass() variant: |allpass(e^jw)| == 1 across frequencies.
    LinkwitzRiley lrap;
    lrap.setCutoff (fc, Fs);
    auto apIR = impulseResponse ([&] (double x) { return lrap.allpass (x); }, N);
    double worstAp = 0.0;
    for (double f : { fc / 8.0, fc / 4.0, fc / 2.0, fc, 2.0 * fc, 4.0 * fc, 8.0 * fc })
    {
        const double m = std::abs (freqResponse (apIR, f, Fs));
        worstAp = std::max (worstAp, std::abs (m - 1.0));
        checkNear (m, 1.0, 1.0e-6, "LR4 allpass() magnitude flatness");
    }
    std::printf ("[LR Fs=%.0f] worst |allpass|-1 = %.2e\n", Fs, worstAp);

    // (d) 24 dB/oct stopband rolloff: one octave (4fc -> 8fc), both deep in the
    // LP stopband, should drop ~24 dB. Formula-independent (4th-order slope).
    const double lp4 = magDb (freqResponse (lpIR, 4.0 * fc, Fs));
    const double lp8 = magDb (freqResponse (lpIR, 8.0 * fc, Fs));
    std::printf ("[LR Fs=%.0f] LP(4fc)=%.2f dB  LP(8fc)=%.2f dB  slope/oct=%.2f\n",
                 Fs, lp4, lp8, lp8 - lp4);
    checkNear (lp8 - lp4, -24.0, 3.0, "LR4 stopband rolloff ~24 dB/oct");
}

// ==========================================================================
// 2. OnePole — 4 consumers; only DC is ever asserted elsewhere.
// ==========================================================================
//
// Cutoff convention (from the header): a = exp(-2*pi*fc/Fs), y = (1-a)x + a y.
// That is the standard RC-equivalent one-pole; for fc << Fs its -3 dB point sits
// at ~fc. We pick a low fc (0.01*Fs) so the small-angle regime holds and assert
// the measured -3 dB crossing lands within a few % of the configured cutoff.
// Independent oracle: the -3 dB level is a fixed target (-3.0103 dB) found by
// scanning the measured magnitude response — not derived from `a`.
void testOnePole (double Fs)
{
    const int N = 1 << 14;                 // one-pole tail decays fast; plenty
    const double fc = 0.01 * Fs;

    OnePole lp;
    lp.setCutoff (fc, Fs);
    auto lpIR = impulseResponse ([&] (double x) { return lp.lp (x); }, N);

    OnePole hpf;
    hpf.setCutoff (fc, Fs);
    auto hpIR = impulseResponse ([&] (double x) { return hpf.hp (x); }, N);

    // DC / Nyquist invariants.
    const double lpDc  = std::abs (freqResponse (lpIR, 0.0, Fs));
    const double hpDc  = std::abs (freqResponse (hpIR, 0.0, Fs));
    const double lpNyq = std::abs (freqResponse (lpIR, 0.5 * Fs, Fs));
    const double hpNyq = std::abs (freqResponse (hpIR, 0.5 * Fs, Fs));
    checkNear (lpDc, 1.0, 1.0e-6, "OnePole lp DC gain == 1");
    checkNear (hpDc, 0.0, 1.0e-6, "OnePole hp DC gain == 0");
    check (lpNyq < 0.05, "OnePole lp attenuates at Nyquist");
    check (hpNyq > 0.95, "OnePole hp passes at Nyquist");

    // -3 dB crossing: scan around fc, linear-interpolate the crossing.
    const double target = -3.0102999566; // 10*log10(0.5)
    double prevF = 0.0, prevDb = 0.0, crossF = -1.0;
    const int steps = 400;
    for (int i = 1; i <= steps; ++i)
    {
        const double f  = fc * (0.4 + 1.6 * (double) i / steps); // 0.4*fc .. 2.0*fc
        const double db = magDb (freqResponse (lpIR, f, Fs));
        if (i > 1 && prevDb >= target && db < target)
        {
            const double t = (target - prevDb) / (db - prevDb);
            crossF = prevF + t * (f - prevF);
            break;
        }
        prevF = f; prevDb = db;
    }
    std::printf ("[OnePole Fs=%.0f] fc=%.2f  -3dB@%.2f  err=%.2f%%  lpNyq=%.2e hpNyq=%.4f\n",
                 Fs, fc, crossF, 100.0 * (crossF - fc) / fc, lpNyq, hpNyq);
    check (crossF > 0.0, "OnePole -3 dB crossing found");
    if (crossF > 0.0)
        checkNear (crossF, fc, 0.05 * fc, "OnePole lp -3 dB point ~ configured cutoff");

    // Monotonicity: lp magnitude non-increasing, hp non-decreasing with freq.
    double lastLp = 1e9, lastHp = -1.0;
    bool lpMono = true, hpMono = true;
    for (int i = 0; i <= 200; ++i)
    {
        const double f  = 0.5 * Fs * (double) i / 200.0;
        const double ml = std::abs (freqResponse (lpIR, f, Fs));
        const double mh = std::abs (freqResponse (hpIR, f, Fs));
        if (ml > lastLp + 1e-9) lpMono = false;
        if (mh < lastHp - 1e-9) hpMono = false;
        lastLp = ml; lastHp = mh;
    }
    check (lpMono, "OnePole lp magnitude monotonically non-increasing");
    check (hpMono, "OnePole hp magnitude monotonically non-decreasing");
}

// ==========================================================================
// 3. DelayLine — fractional (LINEAR) interpolation.
// ==========================================================================
//
// The header documents a LINEAR interpolation kernel. Linear interpolation of a
// sine of angular frequency w has a bounded per-sample error ~ (w^2/8)*A at the
// worst fractional phase (frac=0.5). Independent oracle: the analytically
// delayed sine, sin(w*(n - D)). We measure the actual max error, confirm it is
// consistent with a linear kernel (NOT a higher-order one), and gate just above
// the measured value. A linear-interp *regression* (e.g. to nearest-neighbour)
// would jump to ~O(w) error — far above this gate; a higher-order kernel would
// be far below it. We gate at linear accuracy because the kernel IS linear.
void testDelayLine (double Fs)
{
    const double f = Fs / 40.0;            // rate-derived tone
    const double w = 2.0 * kPi * f / Fs;   // = 2*pi/40
    const double amp = 0.9;
    const double predictedLinearMax = (w * w / 8.0) * amp; // ~0.0031 at this w

    DelayLine dl;
    dl.prepare (4096);

    // Fractional delay: read D = 10.5 samples behind, compare to analytic sine.
    const double D = 10.5;
    double maxErr = 0.0;
    const int total = 4000;
    for (int n = 0; n < total; ++n)
    {
        dl.write (amp * std::sin (w * (double) n));
        if ((double) n > D + 4.0)          // warmup past the delay
        {
            const double got = dl.readInterpolated (D);
            const double want = amp * std::sin (w * ((double) n - D));
            maxErr = std::max (maxErr, std::abs (got - want));
        }
    }
    std::printf ("[DelayLine Fs=%.0f] frac maxErr=%.3e (linear predict ~%.3e)\n",
                 Fs, maxErr, predictedLinearMax);
    // Consistent with a linear kernel: below ~2x the linear prediction, and well
    // above a higher-order kernel's error (which would be << 1e-4 here).
    check (maxErr < 2.0 * predictedLinearMax,
           "DelayLine fractional error consistent with linear kernel (<2x w^2/8)");
    check (maxErr > 0.2 * predictedLinearMax,
           "DelayLine fractional error is linear-kernel sized (not higher-order)");

    // Integer delay must be exact (any interpolator reads a grid sample exactly).
    DelayLine di;
    di.prepare (4096);
    double maxIntErr = 0.0;
    for (int n = 0; n < total; ++n)
    {
        di.write (amp * std::sin (w * (double) n));
        if (n >= 7)
            maxIntErr = std::max (maxIntErr,
                                  std::abs (di.readInterpolated (7.0)
                                            - amp * std::sin (w * ((double) n - 7.0))));
    }
    std::printf ("[DelayLine Fs=%.0f] integer(7) maxErr=%.3e\n", Fs, maxIntErr);
    check (maxIntErr < 1.0e-12, "DelayLine integer delay is exact");

    // Sweeping the delay across the whole buffer stays finite & in-bounds (the
    // read wraps; never reads out of range).
    DelayLine ds;
    ds.prepare (512);
    std::vector<double> out;
    out.reserve ((size_t) total);
    for (int n = 0; n < total; ++n)
    {
        ds.write (amp * std::sin (w * (double) n));
        const double d = 0.5 + 510.0 * (0.5 + 0.5 * std::sin (0.01 * n)); // 0.5..510.x
        out.push_back (ds.readInterpolated (d));
    }
    check (fct::allFinite (out), "DelayLine stays finite across a full-range delay sweep");
    check (fct::peakAbs (out) <= amp + 1e-9, "DelayLine sweep output bounded by input peak");
}

// ==========================================================================
// 4. Compressor — soft-knee static curve (ships in 2 plugins, zero coverage).
// ==========================================================================
//
// Independent oracle (Reiss/Giannoulis standard quadratic soft knee), computed
// here separately from Compressor::staticOutputDb:
//   L <= T - W/2 : GR = 0
//   L >= T + W/2 : GR = (1/R - 1)(L - T)
//   in knee      : GR = (1/R - 1)(L - T + W/2)^2 / (2W)
// Measurement: drive processDetector with a CONSTANT detector level; with a
// constant target the decoupled ballistic converges exactly to the static
// target, so currentGainReductionDb() reads the static curve. Settle time
// scales with Fs (we run 30 * releaseMs, so coeff^N -> 0 at every rate).
double kneeOracleGrDb (double L, double T, double W, double R)
{
    const double slope = (1.0 / R - 1.0);  // <= 0
    const double half = W * 0.5;
    if (L <= T - half)
        return 0.0;
    if (L >= T + half)
        return slope * (L - T);
    const double x = L - T + half;
    return slope * (x * x) / (2.0 * W);
}

double settledGrDb (Compressor& c, double levelDb, double seconds, double Fs)
{
    const double lin = std::pow (10.0, levelDb / 20.0);
    const int n = (int) (seconds * Fs);
    for (int i = 0; i < n; ++i)
        c.processDetector (lin);
    return c.currentGainReductionDb();
}

void testCompressor (double Fs)
{
    const double T = -20.0, W = 10.0, R = 4.0;
    const double settle = 30.0 * 0.050;    // 30 * releaseMs(50ms) = 1.5 s

    // Soft knee across all three regions.
    for (double L : { -40.0, -25.0, -22.5, -20.0, -17.5, -15.0, -6.0, 0.0 })
    {
        Compressor c;
        c.prepare (Fs);
        c.setThresholdDb (T);
        c.setRatio (R);
        c.setKneeDb (W);
        c.setMakeupDb (0.0);
        c.setAttackMs (2.0);
        c.setReleaseMs (50.0);

        const double measured = settledGrDb (c, L, settle, Fs);
        const double oracle   = kneeOracleGrDb (L, T, W, R);
        if (L == -20.0)
            std::printf ("[Comp Fs=%.0f] L=%+.1f measGR=%.5f oracleGR=%.5f\n",
                         Fs, L, measured, oracle);
        checkNear (measured, oracle, 0.02, "Compressor soft-knee static curve");
    }

    // knee = 0 degenerates to a hard knee: no reduction at/below T, immediate
    // full-ratio reduction just above T.
    {
        Compressor c;
        c.prepare (Fs);
        c.setThresholdDb (T);
        c.setRatio (R);
        c.setKneeDb (0.0);
        c.setAttackMs (2.0);
        c.setReleaseMs (50.0);

        const double below = settledGrDb (c, T - 3.0, settle, Fs);
        checkNear (below, 0.0, 1.0e-6, "Compressor hard-knee: no GR below threshold");

        Compressor c2;
        c2.prepare (Fs);
        c2.setThresholdDb (T);
        c2.setRatio (R);
        c2.setKneeDb (0.0);
        c2.setAttackMs (2.0);
        c2.setReleaseMs (50.0);
        const double aboveL = T + 6.0;
        const double above  = settledGrDb (c2, aboveL, settle, Fs);
        const double hardOracle = (1.0 / R - 1.0) * (aboveL - T); // hard knee, no smoothing
        checkNear (above, hardOracle, 0.02, "Compressor hard-knee: full-ratio GR above threshold");
    }
}

// ==========================================================================
// 5. EnvelopeFollower — attack/release ballistics.
// ==========================================================================
//
// Convention (from the header): coeff = exp(-1/(t*fs)) with t = ms/1000, and
// env = c*env + (1-c)*d. So `attackMs` is the one-pole time constant tau: after
// exactly tau seconds of a constant step the envelope reaches (1 - 1/e) of the
// target. Independent oracle: the continuous exponential 1 - e^{-1} = 0.6321
// (attack) and e^{-1} = 0.3679 (release), evaluated at n = round(tau*Fs) samples
// — a fixed analytic target, not the discrete coeff. Sample counts scale w/ Fs.
void testEnvelopeFollower (double Fs)
{
    const double A = 0.8;
    const double attMs = 10.0, relMs = 40.0;

    // Attack: step from 0 to A; after attMs, env ~ A*(1 - 1/e).
    {
        EnvelopeFollower e;
        e.prepare (Fs);
        e.setTimes (attMs, relMs);
        e.reset();
        const int nTau = (int) std::lround (attMs * 1.0e-3 * Fs);
        double envAtTau = 0.0;
        for (int n = 1; n <= nTau; ++n)
            envAtTau = e.process (A);
        const double oracle = A * (1.0 - std::exp (-1.0));
        std::printf ("[Env Fs=%.0f] attack env(tau)=%.5f oracle=%.5f (n=%d)\n",
                     Fs, envAtTau, oracle, nTau);
        checkNear (envAtTau, oracle, 0.02 * A, "EnvelopeFollower attack reaches 1-1/e at tau");
    }

    // Release: saturate to ~A, then feed 0; after relMs, env ~ A/e.
    {
        EnvelopeFollower e;
        e.prepare (Fs);
        e.setTimes (attMs, relMs);
        e.reset();
        for (int n = 0; n < (int) (0.5 * Fs); ++n) // long hold -> env == A
            e.process (A);
        const double start = e.value();
        const int nTau = (int) std::lround (relMs * 1.0e-3 * Fs);
        double envAtTau = start;
        for (int n = 1; n <= nTau; ++n)
            envAtTau = e.process (0.0);
        const double oracle = start * std::exp (-1.0);
        std::printf ("[Env Fs=%.0f] release env(tau)=%.5f oracle=%.5f start=%.5f\n",
                     Fs, envAtTau, oracle, start);
        checkNear (start, A, 1.0e-4, "EnvelopeFollower saturates to input level");
        checkNear (envAtTau, oracle, 0.02 * A, "EnvelopeFollower release decays to 1/e at tau");
    }
}

// ==========================================================================
// 6. Crossover5 — 5-band IIR splitter (only transitively tested elsewhere).
// ==========================================================================
//
// Spec: sum of all 5 bands == allpass(x) (flat magnitude reconstruction);
// adjacent bands cross at ~-6 dB at each crossover fc; bands are isolated
// (energy well outside a band's range is strongly attenuated). All measured in
// the z-domain from impulse responses; no coefficient formula copied.
void testCrossover5 (double Fs)
{
    const int N = 1 << 16;
    // Rate-derived crossovers, ascending with > 1/3-octave spacing, all in band.
    const double f1 = 0.010 * Fs, f2 = 0.030 * Fs, f3 = 0.080 * Fs, f4 = 0.180 * Fs;

    Crossover5 xo;
    xo.prepare (Fs);
    xo.setFrequencies (f1, f2, f3, f4);
    const double fc[4] = { xo.effectiveCrossoverHz (0), xo.effectiveCrossoverHz (1),
                           xo.effectiveCrossoverHz (2), xo.effectiveCrossoverHz (3) };

    // Capture each band's IR and the band-sum IR from one impulse pass.
    std::vector<double> bandIR[5];
    for (auto& b : bandIR) b.assign ((size_t) N, 0.0);
    std::vector<double> sumIR ((size_t) N, 0.0);
    for (int n = 0; n < N; ++n)
    {
        double bands[5];
        xo.process (n == 0 ? 1.0 : 0.0, bands);
        double s = 0.0;
        for (int b = 0; b < 5; ++b) { bandIR[b][(size_t) n] = bands[b]; s += bands[b]; }
        sumIR[(size_t) n] = s;
    }

    // (a) sum(bands) is magnitude-flat (== allpass): |sum(e^jw)| == 1.
    double worstFlat = 0.0;
    for (double f : { f1 / 4.0, f1, 0.5 * (f1 + f2), f2, f3, f4, 2.0 * f4 })
    {
        const double m = std::abs (freqResponse (sumIR, f, Fs));
        worstFlat = std::max (worstFlat, std::abs (m - 1.0));
        // Measured worst ~3e-11; gate at 1e-6 (a comb from a phase-compensation
        // mismatch — class L — would blow far past this).
        checkNear (m, 1.0, 1.0e-6, "Crossover5 band-sum magnitude flatness");
    }
    std::printf ("[XO5 Fs=%.0f] worst |sum|-1 = %.2e\n", Fs, worstFlat);

    // (b) each adjacent pair crosses at ~-6 dB at its crossover fc.
    for (int i = 0; i < 4; ++i)
    {
        const double lo = magDb (freqResponse (bandIR[i],     fc[i], Fs));
        const double hi = magDb (freqResponse (bandIR[i + 1], fc[i], Fs));
        std::printf ("[XO5 Fs=%.0f] fc[%d]=%.1f  band%d=%.3f dB  band%d=%.3f dB\n",
                     Fs, i, fc[i], i, lo, i + 1, hi);
        // A single LR4 split is exactly -6.02 dB at fc; in the 5-band tree the
        // bands are cascaded splits + matched allpasses, so the measured crossing
        // sits near -6 dB with a small tree-structure offset (worst ~-6.41 dB).
        // 0.6 dB brackets that while still asserting a real ~-6 dB crossover.
        checkNear (lo, -6.0206, 0.6, "Crossover5 lower band -6 dB at crossover");
        checkNear (hi, -6.0206, 0.6, "Crossover5 upper band -6 dB at crossover");
    }

    // (c) band isolation: energy well outside a band's range is attenuated.
    const double b0hi = magDb (freqResponse (bandIR[0], f2, Fs));       // lowest band, above it
    const double b4lo = magDb (freqResponse (bandIR[4], f3, Fs));       // highest band, below it
    std::printf ("[XO5 Fs=%.0f] band0@f2=%.2f dB  band4@f3=%.2f dB\n", Fs, b0hi, b4lo);
    check (b0hi < -20.0, "Crossover5 lowest band isolated above its range");
    check (b4lo < -20.0, "Crossover5 highest band isolated below its range");
}

} // namespace

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        testLinkwitzRiley (Fs);
        testOnePole (Fs);
        testDelayLine (Fs);
        testCompressor (Fs);
        testEnvelopeFollower (Fs);
        testCrossover5 (Fs);
    }

    if (g_failures > 0)
    {
        std::printf ("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf ("all primitive checks passed\n");
    return 0;
}
