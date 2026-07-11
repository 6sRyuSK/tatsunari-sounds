//
// dsp_test.cpp -- headless verification of the surikire DSP core.
//
// Covers factory_core::WowFlutter (tests 1-4) and factory_core::Surikire
// (tests 5-11 plus an auxiliary IIR-stability sanity check). Every expected
// value is an INDEPENDENT oracle -- Bessel series, z-domain one-pole products,
// the LCG/dropout schedule re-implemented from the header's published spec,
// uniform-noise variance times the one-pole power gain -- never derived from
// the implementation under test. Runs across the full standard sample-rate
// matrix via fct::sampleRatesFromArgs.
//
#include "factory_core/Surikire.h"
#include "factory_core/WowFlutter.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace fct = factory_core::testing;
using factory_core::Surikire;
using factory_core::WowFlutter;

static constexpr double kPi    = 3.14159265358979323846;
static constexpr double kTwoPi = 6.283185307179586476925286766559;

// ---- failure bookkeeping ---------------------------------------------------
static int g_failures = 0;

static void fail (const std::string& msg)
{
    std::fprintf (stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
}

static void check (bool ok, const std::string& msg)
{
    if (! ok)
        fail (msg);
}

static void checkRel (double got, double expected, double relTol, const std::string& msg)
{
    const double denom = std::max (1e-300, std::abs (expected));
    if (! (std::abs (got - expected) / denom <= relTol))
        fail (msg + " got=" + std::to_string (got) + " exp=" + std::to_string (expected));
}

static void checkAbs (double got, double expected, double absTol, const std::string& msg)
{
    if (! (std::abs (got - expected) <= absTol))
        fail (msg + " got=" + std::to_string (got) + " exp=" + std::to_string (expected));
}

static std::string at (const std::string& what, double Fs)
{
    return what + " @Fs=" + std::to_string ((long long) Fs);
}

// ---- deterministic LCG (independent re-implementation of the header spec) --
static inline std::uint64_t lcgNext (std::uint64_t& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL; // uint64 wrap
    return s;
}

static inline double lcgU01 (std::uint64_t s)
{
    return (double) (s >> 11) * (1.0 / 9007199254740992.0); // top 53 bits -> [0,1)
}

// Advance the state FIRST, then map to [0,1) -- the published draw order.
static inline double draw (std::uint64_t& s)
{
    return lcgU01 (lcgNext (s));
}

// ---- rectangular-window Goertzel (exact for integer-period bins) -----------
static double goertzelMag (const std::vector<double>& x, std::size_t start,
                           std::size_t N, long long k)
{
    const double w     = 2.0 * kPi * (double) k / (double) N;
    const double cw    = std::cos (w);
    const double sw    = std::sin (w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t n = 0; n < N; ++n)
    {
        const double s0 = x[start + n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return std::sqrt (re * re + im * im);
}

// ---- Bessel J0 / J1 power series (|x| <~ 1.3 here; converges in a few terms;
//      std::cyl_bessel_j is forbidden -- unimplemented on libc++) ------------
static double besselJ0 (double x)
{
    const double t2 = 0.25 * x * x;
    double term = 1.0, sum = 1.0;
    for (int m = 1; m < 40; ++m)
    {
        term *= -t2 / ((double) m * (double) m);
        sum += term;
        if (std::abs (term) < 1e-18 * std::abs (sum))
            break;
    }
    return sum;
}

static double besselJ1 (double x)
{
    const double t = 0.5 * x, t2 = t * t;
    double term = t, sum = t;
    for (int m = 1; m < 40; ++m)
    {
        term *= -t2 / ((double) m * (double) (m + 1));
        sum += term;
        if (std::abs (term) < 1e-18 * std::abs (sum))
            break;
    }
    return sum;
}

// ---- z-domain one-pole oracle (built from the spec's closed forms only) ----
static std::complex<double> onePoleLp (double fc, double fp, double Fs)
{
    const double a = std::exp (-2.0 * kPi * std::clamp (fc, 1.0, 0.49 * Fs) / Fs);
    const double w = 2.0 * kPi * fp / Fs;
    return std::complex<double> (1.0 - a, 0.0)
         / (std::complex<double> (1.0, 0.0) - a * std::exp (std::complex<double> (0.0, -w)));
}

// |H| of the four-section cascade HP(user) * HP(gen) * LP(gen) * LP(user) at
// probe frequency fp, with the generation cutoffs from the spec closed forms.
static double filterMag (double g, double userHp, double userLp, double fp, double Fs)
{
    const double fHpGen = 20.0 * std::pow (20.0, g);
    const double fLpGen = 20000.0 * std::pow (20.0, -g);
    return std::abs (1.0 - onePoleLp (userHp, fp, Fs))
         * std::abs (1.0 - onePoleLp (fHpGen, fp, Fs))
         * std::abs (onePoleLp (fLpGen, fp, Fs))
         * std::abs (onePoleLp (userLp, fp, Fs));
}

// ---- dropout schedule predictor (full re-implementation of the header spec:
//      per event draw u_gap, u_depth, u_width IN THAT ORDER; the first gap is
//      measured from t = 0; gap and width are llround-ed to whole samples) ---
struct DropEvent
{
    long long startSamp;
    long long widthSamp;
    double    depth;
};

static std::vector<DropEvent> predictDropouts (double failure01, long long totalSamples, double Fs)
{
    std::uint64_t s = 0x5150D120D0FF5EEDULL; // kDropoutSeed, restated independently
    std::vector<DropEvent> ev;
    long long tEnd = 0;
    for (;;)
    {
        const double ug = draw (s);
        const double ud = draw (s);
        const double uw = draw (s);
        const long long gap   = std::llround ((0.25 + 1.75 * ug) * Fs);
        const double    depth = failure01 * (0.5 + 0.5 * ud) * 0.9; // kDropMaxDepth
        const long long width = std::llround ((0.03 + 0.05 * uw) * Fs);
        const long long start = tEnd + gap;
        if (start >= totalSamples)
            break;
        ev.push_back ({ start, width, depth });
        tEnd = start + width;
    }
    return ev;
}

// ---- prefix-sum energy for O(1) short-time RMS ------------------------------
struct Prefix
{
    std::vector<double> p; // p[i] = sum of x[0..i)^2

    explicit Prefix (const std::vector<double>& x) : p (x.size() + 1, 0.0)
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            p[i + 1] = p[i] + x[i] * x[i];
    }

    double rms (long long c, long long half) const
    {
        const long long n = (long long) p.size() - 1;
        const long long a = std::max (0LL, c - half);
        const long long b = std::min (n, c + half);
        if (b <= a)
            return 0.0;
        return std::sqrt ((p[(std::size_t) b] - p[(std::size_t) a]) / (double) (b - a));
    }
};

// ---- zero-crossing local frequency (upward crossings, linear interp) -------
static double localFreq (const std::vector<double>& y, std::size_t c, std::size_t half, double Fs)
{
    std::vector<double> cr;
    const std::size_t lo = (c >= half) ? c - half + 1 : 1;
    const std::size_t hi = std::min (y.size(), c + half);
    for (std::size_t n = lo; n < hi; ++n)
        if (y[n - 1] <= 0.0 && y[n] > 0.0)
        {
            const double frac = -y[n - 1] / (y[n] - y[n - 1]);
            cr.push_back ((double) (n - 1) + frac);
        }
    if (cr.size() < 2)
        return 0.0;
    return Fs * (double) (cr.size() - 1) / (cr.back() - cr.front());
}

// ---- drive helpers ----------------------------------------------------------
static std::vector<float> makeSine (double A, double f, double Fs, std::size_t N, double ph = 0.0)
{
    std::vector<float> v (N);
    const double w = kTwoPi * f / Fs;
    for (std::size_t n = 0; n < N; ++n)
        v[n] = (float) (A * std::sin (w * (double) n + ph));
    return v;
}

static std::vector<double> toDouble (const std::vector<float>& v)
{
    std::vector<double> d (v.size());
    for (std::size_t n = 0; n < v.size(); ++n)
        d[n] = (double) v[n];
    return d;
}

static std::vector<double> runWowMono (WowFlutter& w, const std::vector<float>& in)
{
    std::vector<float> buf (in);
    float* ch[1] = { buf.data() };
    w.process (ch, 1, (int) buf.size());
    return toDouble (buf);
}

static std::vector<double> runSurikireMono (Surikire& e, const std::vector<float>& in,
                                            std::size_t chunk = 0)
{
    std::vector<float> buf (in);
    if (chunk == 0)
        chunk = buf.size();
    for (std::size_t off = 0; off < buf.size(); off += chunk)
    {
        const std::size_t len = std::min (chunk, buf.size() - off);
        float* ch[1] = { buf.data() + off };
        e.process (ch, 1, (int) len);
    }
    return toDouble (buf);
}

static void runSurikireStereo (Surikire& e, std::vector<float>& L, std::vector<float>& R,
                               std::size_t chunk = 0)
{
    if (chunk == 0)
        chunk = L.size();
    for (std::size_t off = 0; off < L.size(); off += chunk)
    {
        const std::size_t len = std::min (chunk, L.size() - off);
        float* ch[2] = { L.data() + off, R.data() + off };
        e.process (ch, 2, (int) len);
    }
}

// Linear interpolation into a float buffer at a (possibly negative) absolute
// sample position; samples outside [0, size) read as 0 (the delay line starts
// cleared). Mirrors the DelayLine neighbour/frac convention.
static double interpAt (const std::vector<float>& in, double pos)
{
    const double fl = std::floor (pos);
    const long long i0 = (long long) fl;
    const double frac = pos - fl;
    const long long n = (long long) in.size();
    const double s0 = (i0 >= 0 && i0 < n) ? (double) in[(std::size_t) i0] : 0.0;
    const double s1 = (i0 + 1 >= 0 && i0 + 1 < n) ? (double) in[(std::size_t) (i0 + 1)] : 0.0;
    return s0 + frac * (s1 - s0);
}

// =============================================================================
// #1 depth = 0 transparency: WowFlutter must be a pure integer-sample delay.
// Rate gate: center = llround(kCenterDelayMs * Fs / 1000) is derived from Fs.
// =============================================================================
static void wowFlutterTransparencyTest (double Fs)
{
    WowFlutter w;
    w.prepare (Fs, 1);
    w.setWowDepth01 (0.0);
    w.setFlutterDepth01 (0.0);
    w.reset();

    const long long center = std::llround (WowFlutter::kCenterDelayMs * Fs / 1000.0);
    const std::size_t N = (std::size_t) center + 4096;

    std::uint64_t s = 0x0123456789ABCDEFULL; // test-local seed, any fixed value
    std::vector<float> in (N);
    for (auto& v : in)
        v = (float) (0.5 * (2.0 * draw (s) - 1.0));

    const std::vector<double> y = runWowMono (w, in);

    double maxErr = 0.0, maxHead = 0.0;
    for (std::size_t n = 0; n < N; ++n)
    {
        const double expv = (n >= (std::size_t) center) ? (double) in[n - (std::size_t) center] : 0.0;
        maxErr = std::max (maxErr, std::abs (y[n] - expv));
        if (n < (std::size_t) center)
            maxHead = std::max (maxHead, std::abs (y[n]));
    }
    check (maxErr <= 1e-6, at ("wowflutter depth=0 must be an exact center-delay copy, maxErr="
                               + std::to_string (maxErr), Fs));
    check (maxHead <= 1e-6, at ("wowflutter head (first center samples) must be silent, peak="
                                + std::to_string (maxHead), Fs));
}

// =============================================================================
// #2 Bessel sidebands: sine-modulated delay is phase modulation, so the first
// sideband / carrier ratio must equal |J1(beta)/J0(beta)| with
// beta = 2*pi*f0*depthMs/1000 (sample-rate INDEPENDENT).
// =============================================================================
static void wowFlutterBesselSidebandTest (double Fs)
{
    WowFlutter w;
    w.prepare (Fs, 1);
    w.setWowDepth01 (0.05); // 0.05 * kMaxWowDepthMs = 0.5 ms
    w.setFlutterDepth01 (0.0);
    w.setWowRateHz (6.0);
    w.reset();

    const std::size_t warm = (std::size_t) std::llround (0.1 * Fs);
    const std::size_t N    = (std::size_t) std::llround (2.0 * Fs); // T = 2 s window
    const std::vector<float> in = makeSine (0.5, 400.0, Fs, warm + N);
    const std::vector<double> y = runWowMono (w, in);

    const double C   = goertzelMag (y, warm, N, 800); // 400 Hz
    const double Uhi = goertzelMag (y, warm, N, 812); // 406 Hz
    const double Ulo = goertzelMag (y, warm, N, 788); // 394 Hz

    const double beta = 2.0 * kPi * 400.0 * 0.5 / 1000.0; // 0.4*pi, Fs-independent
    const double expRatio = std::abs (besselJ1 (beta) / besselJ0 (beta));

    checkRel (Uhi / C, expRatio, 0.03, at ("wowflutter upper sideband J1/J0", Fs));
    checkRel (Ulo / C, expRatio, 0.03, at ("wowflutter lower sideband J1/J0", Fs));
    check (std::abs (Uhi - Ulo) / C <= 0.01,
           at ("wowflutter sideband symmetry |Uhi-Ulo|/C=" + std::to_string (std::abs (Uhi - Ulo) / C), Fs));
}

// =============================================================================
// #3 wow + flutter simultaneously: carrier amplitude scales as
// J0(beta_wow) * J0(beta_flutter), independent of the modulation rates.
// NOTE: the plan asked for a 40 Hz flutter rate, but the header clamps rates
// to kMaxRateHz = 16 -- use 16 Hz explicitly (32 integer cycles in 2 s).
// =============================================================================
static void wowFlutterDualModCarrierTest (double Fs)
{
    WowFlutter w;
    w.prepare (Fs, 1);
    w.setWowDepth01 (0.05);    // 0.5 ms
    w.setFlutterDepth01 (1.0); // kMaxFlutterDepthMs = 0.45 ms
    w.setWowRateHz (6.0);
    w.setFlutterRateHz (16.0); // kMaxRateHz; J0*J0 oracle is rate-independent
    w.reset();

    const std::size_t warm = (std::size_t) std::llround (0.1 * Fs);
    const std::size_t N    = (std::size_t) std::llround (2.0 * Fs);
    const std::vector<float> in = makeSine (0.5, 400.0, Fs, warm + N);

    const std::vector<double> yMod = runWowMono (w, in);
    const double cMod = goertzelMag (yMod, warm, N, 800);

    w.setWowDepth01 (0.0);
    w.setFlutterDepth01 (0.0);
    w.reset();
    const std::vector<double> yRef = runWowMono (w, in);
    const double cRef = goertzelMag (yRef, warm, N, 800);

    const double betaW = 2.0 * kPi * 400.0 * 0.5 / 1000.0;  // 1.2566
    const double betaF = 2.0 * kPi * 400.0 * 0.45 / 1000.0; // 1.1310
    const double expRatio = besselJ0 (betaW) * besselJ0 (betaF);

    checkRel (cMod / cRef, expRatio, 0.05, at ("wowflutter dual-mod carrier J0*J0", Fs));
}

// =============================================================================
// #4 max wow depth, no clamp: the instantaneous pitch must reach the analytic
// extrema f0*(1 +- 2*pi*f_m*depthMs/1000). A buffer that cannot hold the
// worst-case modulated delay would clip the sweep and miss the extrema.
// Secondary: exact time-domain reconstruction from the header's recursion.
// =============================================================================
static void wowFlutterMaxDepthNoClampTest (double Fs)
{
    WowFlutter w;
    w.prepare (Fs, 1);
    w.setWowDepth01 (1.0); // kMaxWowDepthMs = 10 ms
    w.setFlutterDepth01 (0.0);
    w.setWowRateHz (2.0);
    w.reset();

    const std::size_t N = (std::size_t) std::llround (0.7 * Fs);
    const std::vector<float> in = makeSine (0.5, 1000.0, Fs, N);
    const std::vector<double> y = runWowMono (w, in);

    const std::size_t half = (std::size_t) std::llround (0.004 * Fs); // ~4 carrier periods
    const double fMax = localFreq (y, (std::size_t) std::llround (Fs / 4.0), half, Fs); // phase pi
    const double fMin = localFreq (y, (std::size_t) std::llround (Fs / 2.0), half, Fs); // phase 2*pi

    const double r = 2.0 * kPi * 2.0 * 10.0 / 1000.0; // 0.12566, Fs-independent
    checkRel (fMax, 1000.0 * (1.0 + r), 0.05, at ("wowflutter max-depth instantaneous f max", Fs));
    checkRel (fMin, 1000.0 * (1.0 - r), 0.05, at ("wowflutter max-depth instantaneous f min", Fs));

    // Secondary reconstruction oracle: D[n] from the contract recursion
    // (evaluate at the current phase, then advance and wrap at 2*pi).
    const long long center = std::llround (WowFlutter::kCenterDelayMs * Fs / 1000.0);
    const double inc = kTwoPi * 2.0 / Fs;
    double phase = 0.0, maxErr = 0.0;
    for (std::size_t n = 0; n < N; ++n)
    {
        const double D = (double) center + (10.0 * std::sin (phase)) * Fs / 1000.0;
        phase += inc;
        if (phase >= kTwoPi)
            phase -= kTwoPi;
        const double yExp = interpAt (in, (double) n - D);
        maxErr = std::max (maxErr, std::abs (y[n] - yExp));
    }
    check (maxErr <= 1e-6, at ("wowflutter max-depth exact reconstruction, maxErr="
                               + std::to_string (maxErr), Fs));
}

// =============================================================================
// #5 filter cascade vs the z-domain |H| product (the engine's main rate gate:
// the one-pole coefficient a = exp(-2*pi*fc/Fs) bakes Fs into the oracle).
// The saturator is tanh even at s = 0, so probe at -40 dBFS to stay linear.
// =============================================================================
static void filterResponseTest (double Fs)
{
    const double gGrid[5]     = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    const double userGrid[2][2] = { { 20.0, 20000.0 }, { 200.0, 5000.0 } };
    const double fpGrid[6]    = { 100.0, 200.0, 500.0, 1000.0, 2000.0, 4000.0 };

    const std::size_t warm = (std::size_t) std::llround (0.05 * Fs);
    const std::size_t N    = (std::size_t) std::llround (Fs); // T = 1 s -> bin k == fp in Hz

    double measBase[5][6];
    for (auto& row : measBase)
        for (double& v : row)
            v = -1.0;

    Surikire e;
    e.prepare (Fs, 1);

    for (int ui = 0; ui < 2; ++ui)
        for (int gi = 0; gi < 5; ++gi)
            for (int fi = 0; fi < 6; ++fi)
            {
                const double g  = gGrid[gi];
                const double hp = userGrid[ui][0];
                const double lp = userGrid[ui][1];
                const double fp = fpGrid[fi];

                const double expMag = filterMag (g, hp, lp, fp, Fs);
                if (expMag < 0.02)
                    continue; // below the small-signal measurement floor

                e.setWow01 (0.0);
                e.setFlutter01 (0.0);
                e.setNoise01 (0.0);
                e.setFailure01 (0.0);
                e.setSaturate01 (0.0);
                e.setMix01 (1.0);
                e.setGeneration01 (g);
                e.setUserHpHz (hp);
                e.setUserLpHz (lp);
                e.reset();

                const std::vector<float> in = makeSine (0.01, fp, Fs, warm + N);
                const std::vector<double> y = runSurikireMono (e, in);
                const std::vector<double> inD = toDouble (in);

                const long long k = std::llround (fp);
                const double meas = goertzelMag (y, warm, N, k) / goertzelMag (inD, warm, N, k);

                checkRel (meas, expMag, 0.02,
                          at ("filter |H| g=" + std::to_string (g) + " hp=" + std::to_string (hp)
                              + " lp=" + std::to_string (lp) + " fp=" + std::to_string (fp), Fs));
                if (ui == 0)
                    measBase[gi][fi] = meas;
            }

    // Formula-independent invariants: more generations must cut more highs
    // (gen LP falls with g) and more lows (gen HP rises with g).
    if (measBase[0][5] > 0.0 && measBase[4][5] > 0.0)
        check (measBase[4][5] < measBase[0][5],
               at ("gen monotonicity at 4 kHz: |H|(g=1) < |H|(g=0)", Fs));
    if (measBase[0][0] > 0.0 && measBase[4][0] > 0.0)
        check (measBase[4][0] < measBase[0][0],
               at ("gen monotonicity at 100 Hz: |H|(g=1) < |H|(g=0)", Fs));
}

// =============================================================================
// #6 saturation: (a) unity small-signal gain for every s (slope of
// (1/d)*tanh(d*x) at 0 is exactly 1); (b) full-scale H3/H1 strictly monotone
// in s and above -30 dB at s = 1; (c) odd symmetry -> H2/H1 <= 1e-3.
// fp = 1009 Hz (prime) so no aliased harmonic lands exactly on the H2/H3 bins.
// =============================================================================
static void saturationTest (double Fs)
{
    const double sGrid[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    const double fp = 1009.0;
    const long long k1 = 1009, k2 = 2018, k3 = 3027;
    const std::size_t warm = (std::size_t) std::llround (0.05 * Fs);
    const std::size_t N    = (std::size_t) std::llround (Fs);

    Surikire e;
    e.prepare (Fs, 1);
    auto configure = [&e] (double s)
    {
        e.setWow01 (0.0);
        e.setFlutter01 (0.0);
        e.setNoise01 (0.0);
        e.setFailure01 (0.0);
        e.setGeneration01 (0.0); // widest gen band: HP20 / LP20000
        e.setUserHpHz (20.0);
        e.setUserLpHz (20000.0);
        e.setMix01 (1.0);
        e.setSaturate01 (s);
        e.reset();
    };

    const double expMag = filterMag (0.0, 20.0, 20000.0, fp, Fs);

    // (a) small signal (-40 dBFS): gain independent of s, equal to the filter |H|.
    const std::vector<float> inSmall = makeSine (0.01, fp, Fs, warm + N);
    const std::vector<double> inSmallD = toDouble (inSmall);
    const double inMag = goertzelMag (inSmallD, warm, N, k1);
    double g1[5];
    for (int si = 0; si < 5; ++si)
    {
        configure (sGrid[si]);
        const std::vector<double> y = runSurikireMono (e, inSmall);
        g1[si] = goertzelMag (y, warm, N, k1) / inMag;
    }
    for (int si = 0; si < 5; ++si)
        check (std::abs (20.0 * std::log10 (g1[si] / g1[0])) <= 0.1,
               at ("sat small-signal gain flat in s, s=" + std::to_string (sGrid[si]), Fs));
    check (std::abs (20.0 * std::log10 (g1[0] / expMag)) <= 0.1,
           at ("sat small-signal gain == filter |H| at 1009 Hz", Fs));

    // (b)(c) full-scale THD.
    const std::vector<float> inFull = makeSine (1.0, fp, Fs, warm + N);
    double r3[5];
    for (int si = 0; si < 5; ++si)
    {
        configure (sGrid[si]);
        const std::vector<double> y = runSurikireMono (e, inFull);
        const double h1 = goertzelMag (y, warm, N, k1);
        const double h2 = goertzelMag (y, warm, N, k2);
        const double h3 = goertzelMag (y, warm, N, k3);
        check (h2 / h1 <= 1e-3,
               at ("sat odd symmetry H2/H1=" + std::to_string (h2 / h1)
                   + " s=" + std::to_string (sGrid[si]), Fs));
        r3[si] = h3 / h1;
    }
    for (int si = 0; si + 1 < 5; ++si)
        check (r3[si + 1] > r3[si],
               at ("sat H3/H1 strictly monotone in s, step " + std::to_string (si), Fs));
    check (20.0 * std::log10 (r3[4]) > -30.0,
           at ("sat H3/H1 at s=1 above -30 dB, got dB=" + std::to_string (20.0 * std::log10 (r3[4])), Fs));

    // Independent pure-tanh oracle at s = 1. The chain filters BEFORE the
    // saturator (harmonics see no further filtering with noise off, dropout
    // gain 1 and mix 1), so drive the closed-form tanh at the pre-filtered
    // amplitude A * |H(1009)| instead of post-correcting by a filter ratio.
    {
        const double d = 8.0; // 1 + 7*1
        const double A = 1.0 * expMag;
        std::vector<double> u (N);
        const double w = kTwoPi * fp / Fs;
        for (std::size_t n = 0; n < N; ++n)
            u[n] = (1.0 / d) * std::tanh (d * A * std::sin (w * (double) n));
        const double rPure = goertzelMag (u, 0, N, k3) / goertzelMag (u, 0, N, k1);
        checkRel (r3[4], rPure, 0.15, at ("sat H3/H1 vs pure-tanh oracle at s=1", Fs));
    }
}

// =============================================================================
// #7 deterministic hiss: (a) RMS matches uniform variance (1/3) times the
// one-pole power gain (1-a)/(1+a) at kNoiseShapeHz, scaled by kNoiseMaxAmp --
// a is Fs-dependent, so this is a rate gate; (b) noise = 0 -> true silence;
// (c) reset() reproduces the exact same sequence (bit-identical).
// =============================================================================
static void noiseFloorTest (double Fs)
{
    Surikire e;
    e.prepare (Fs, 1);
    auto configure = [&e] (double noiseAmt)
    {
        e.setWow01 (0.0);
        e.setFlutter01 (0.0);
        e.setGeneration01 (0.0);
        e.setSaturate01 (0.0);
        e.setUserHpHz (20.0);
        e.setUserLpHz (20000.0);
        e.setFailure01 (0.0);
        e.setMix01 (1.0);
        e.setNoise01 (noiseAmt);
        e.reset();
    };

    // (a) hiss RMS vs the closed-form power oracle.
    configure (1.0);
    const std::size_t N4 = (std::size_t) std::llround (4.0 * Fs);
    const std::vector<float> silence4 (N4, 0.0f);
    const std::vector<double> out = runSurikireMono (e, silence4);
    const std::size_t skip = (std::size_t) std::llround (0.02 * Fs);
    double energy = 0.0;
    for (std::size_t n = skip; n < N4; ++n)
        energy += out[n] * out[n];
    const double rms = std::sqrt (energy / (double) (N4 - skip));
    const double a = std::exp (-2.0 * kPi * 8000.0 / Fs); // kNoiseShapeHz
    const double expRms = 0.02 * std::sqrt ((1.0 / 3.0) * (1.0 - a) / (1.0 + a)); // kNoiseMaxAmp
    checkRel (rms, expRms, 0.10, at ("hiss RMS vs uniform*onepole power oracle", Fs));

    // (b) noise = 0: absolute silence floor (no phantom output).
    configure (0.0);
    const std::vector<float> silenceHalf ((std::size_t) std::llround (0.5 * Fs), 0.0f);
    const std::vector<double> outQ = runSurikireMono (e, silenceHalf);
    check (fct::peakAbs (outQ) <= 1e-12,
           at ("noise=0 silence floor, peak=" + std::to_string (fct::peakAbs (outQ)), Fs));

    // (c) determinism: reset() must restart the exact same sequence.
    configure (1.0);
    const std::vector<float> silence1 ((std::size_t) std::llround (1.0 * Fs), 0.0f);
    const std::vector<double> outA = runSurikireMono (e, silence1);
    e.reset();
    const std::vector<double> outB = runSurikireMono (e, silence1);
    bool identical = outA.size() == outB.size();
    for (std::size_t n = 0; identical && n < outA.size(); ++n)
        identical = outA[n] == outB[n];
    check (identical, at ("hiss sequence bit-identical after reset()", Fs));
}

// =============================================================================
// #8 deterministic dropouts: predict the whole event table from the published
// LCG spec, then verify dip depth (+-10%), dip timing (+-5 ms), flat gaps, and
// a flat envelope at failure = 0. The gain envelope is measured as the exact
// short-time-RMS ratio of the failure=1 and failure=0 twin runs (noise off,
// mix 1, dropout is the final pure gain, so outA[n] = outB[n]*g[n]).
// Rate gate: the schedule is drawn in seconds and llround-ed to samples.
// =============================================================================
static void dropoutScheduleTest (double Fs)
{
    const std::size_t N = (std::size_t) std::llround (6.0 * Fs);
    const std::vector<float> in = makeSine (0.1, 1000.0, Fs, N);

    Surikire e;
    e.prepare (Fs, 1);
    auto configure = [&e] (double failureAmt)
    {
        e.setWow01 (0.0);
        e.setFlutter01 (0.0);
        e.setGeneration01 (0.0);
        e.setSaturate01 (0.0);
        e.setUserHpHz (20.0);
        e.setUserLpHz (20000.0);
        e.setNoise01 (0.0);
        e.setMix01 (1.0);
        e.setFailure01 (failureAmt);
        e.reset();
    };

    configure (1.0);
    const std::vector<double> outA = runSurikireMono (e, in);
    configure (0.0);
    const std::vector<double> outB = runSurikireMono (e, in);

    const Prefix pa (outA), pb (outB);
    const long long half   = std::llround (0.005 * Fs) / 2; // 5 ms RMS window
    const long long settle = std::llround (0.05 * Fs);
    auto gEnv = [&] (long long c) { return pa.rms (c, half) / pb.rms (c, half); };

    const std::vector<DropEvent> ev = predictDropouts (1.0, (long long) N, Fs);
    check (ev.size() >= 2, at ("expected >= 2 predicted dropout events in 6 s, got "
                               + std::to_string (ev.size()), Fs));

    std::size_t tested = 0;
    for (std::size_t i = 0; i < ev.size(); ++i)
    {
        const DropEvent& d = ev[i];
        if (d.startSamp < settle + half || d.startSamp + d.widthSamp + half >= (long long) N)
            continue;
        ++tested;

        const long long centerS  = d.startSamp + d.widthSamp / 2;
        const double centerSec   = ((double) d.startSamp + 0.5 * (double) d.widthSamp) / Fs;

        checkRel (1.0 - gEnv (centerS), d.depth, 0.10,
                  at ("dropout depth, event " + std::to_string (i), Fs));

        const long long lo = std::max (half, centerS - std::llround (0.02 * Fs));
        const long long hi = std::min ((long long) N - half - 1, centerS + std::llround (0.02 * Fs));
        long long cMin = lo;
        double best = std::numeric_limits<double>::max();
        for (long long c = lo; c <= hi; ++c)
        {
            const double gv = gEnv (c);
            if (gv < best)
            {
                best = gv;
                cMin = c;
            }
        }
        checkAbs ((double) cMin / Fs, centerSec, 0.005,
                  at ("dropout dip timing (s), event " + std::to_string (i), Fs));
    }
    check (tested >= 2, at ("expected >= 2 fully-contained dropout events, got "
                            + std::to_string (tested), Fs));

    // Gaps between events: gain envelope must be 1 (no phantom dips).
    for (std::size_t i = 0; i + 1 < ev.size(); ++i)
    {
        const long long gapMid = (ev[i].startSamp + ev[i].widthSamp + ev[i + 1].startSamp) / 2;
        if (gapMid < settle + half || gapMid + half >= (long long) N)
            continue;
        check (std::abs (gEnv (gapMid) - 1.0) < 0.02,
               at ("gap envelope flat after event " + std::to_string (i), Fs));
    }
    if (! ev.empty() && ev[0].startSamp / 2 >= settle + half)
        check (std::abs (gEnv (ev[0].startSamp / 2) - 1.0) < 0.02,
               at ("gap envelope flat before first event", Fs));

    // failure = 0: short-time RMS flat over time (max/min <= 0.1 dB).
    double mn = std::numeric_limits<double>::max(), mx = 0.0;
    const long long from = std::llround (0.25 * Fs);
    const long long to   = (long long) N - std::llround (0.05 * Fs);
    const long long step = std::llround (0.025 * Fs);
    for (long long c = from; c <= to; c += step)
    {
        const double r = pb.rms (c, half);
        mn = std::min (mn, r);
        mx = std::max (mx, r);
    }
    check (20.0 * std::log10 (mx / mn) <= 0.1,
           at ("failure=0 envelope flat, spread dB=" + std::to_string (20.0 * std::log10 (mx / mn)), Fs));
}

// =============================================================================
// #9 long-hold worst case: every stage maxed (user filters wide open = the
// peak-worst setting), 8 s stereo white noise, prime chunk length 173.
// Realistic peak bound 1.25 -- not a "not NaN" 1e6 tolerance.
// =============================================================================
static void longHoldWorstCaseTest (double Fs)
{
    Surikire e;
    e.prepare (Fs, 2);
    e.setWow01 (1.0);
    e.setFlutter01 (1.0);
    e.setGeneration01 (1.0);
    e.setSaturate01 (1.0);
    e.setNoise01 (1.0);
    e.setFailure01 (1.0);
    e.setUserHpHz (20.0);
    e.setUserLpHz (20000.0);
    e.setMix01 (1.0);
    e.reset();

    const std::size_t N = (std::size_t) std::llround (8.0 * Fs);
    std::vector<float> L (N), R (N);
    std::uint64_t sL = 0xC0FFEE0000000001ULL, sR = 0xC0FFEE0000000002ULL; // test-local seeds
    for (std::size_t n = 0; n < N; ++n)
    {
        L[n] = (float) (2.0 * draw (sL) - 1.0);
        R[n] = (float) (2.0 * draw (sR) - 1.0);
    }

    runSurikireStereo (e, L, R, 173); // prime chunk length

    const std::vector<double> dL = toDouble (L), dR = toDouble (R);
    check (fct::allFinite (dL) && fct::allFinite (dR), at ("worst-case 8 s hold stays finite", Fs));
    check (fct::peakAbs (dL) <= 1.25 && fct::peakAbs (dR) <= 1.25,
           at ("worst-case peak <= 1.25, got L=" + std::to_string (fct::peakAbs (dL))
               + " R=" + std::to_string (fct::peakAbs (dR)), Fs));
}

// =============================================================================
// #10 NaN/Inf self-recovery: inject 8 NaN + 2 Inf samples mid-stream; output
// must stay finite and reconverge to the clean run within ~100 ms.
// =============================================================================
static void nanRecoveryTest (double Fs)
{
    Surikire e;
    e.prepare (Fs, 1);
    e.setWow01 (0.3);
    e.setFlutter01 (0.2);
    e.setGeneration01 (0.3);
    e.setSaturate01 (0.3);
    e.setNoise01 (0.0);
    e.setFailure01 (0.0);
    e.setUserHpHz (20.0);
    e.setUserLpHz (20000.0);
    e.setMix01 (1.0);
    e.reset();

    const std::size_t N = (std::size_t) std::llround (1.0 * Fs);
    const std::vector<float> inClean = makeSine (0.5, 440.0, Fs, N);
    std::vector<float> inBad = inClean;
    const std::size_t inj = (std::size_t) std::llround (0.4 * Fs);
    for (std::size_t k = 0; k < 8; ++k)
        inBad[inj + k] = std::numeric_limits<float>::quiet_NaN();
    inBad[inj + 8] = std::numeric_limits<float>::infinity();
    inBad[inj + 9] = std::numeric_limits<float>::infinity();

    const std::vector<double> out = runSurikireMono (e, inBad);
    check (fct::allFinite (out), at ("output finite through NaN/Inf injection", Fs));
    check (fct::peakAbs (out) <= 1.25,
           at ("peak <= 1.25 through NaN/Inf injection, got " + std::to_string (fct::peakAbs (out)), Fs));

    e.reset();
    const std::vector<double> outClean = runSurikireMono (e, inClean);
    double maxDiff = 0.0;
    for (std::size_t n = inj + (std::size_t) std::llround (0.1 * Fs); n < N; ++n)
        maxDiff = std::max (maxDiff, std::abs (out[n] - outClean[n]));
    check (maxDiff <= 1e-6,
           at ("recovery to clean run within 100 ms, maxDiff=" + std::to_string (maxDiff), Fs));
}

// =============================================================================
// #11 reset residue: charge every state with a big signal, reset(), then feed
// silence -- with noise = 0 the output must be exactly zero (<= 1e-12).
// =============================================================================
static void resetResidueTest (double Fs)
{
    Surikire e;
    e.prepare (Fs, 1);
    e.setWow01 (0.5);
    e.setFlutter01 (0.5);
    e.setGeneration01 (0.5);
    e.setSaturate01 (0.5);
    e.setNoise01 (0.0);
    e.setFailure01 (0.0);
    e.setUserHpHz (20.0);
    e.setUserLpHz (20000.0);
    e.setMix01 (1.0);
    e.reset();

    const std::vector<float> charge = makeSine (0.9, 500.0, Fs, (std::size_t) std::llround (0.5 * Fs));
    (void) runSurikireMono (e, charge);

    e.reset();
    const std::vector<float> silence ((std::size_t) std::llround (0.2 * Fs), 0.0f);
    const std::vector<double> out = runSurikireMono (e, silence);
    check (fct::peakAbs (out) <= 1e-12,
           at ("post-reset residue, peak=" + std::to_string (fct::peakAbs (out)), Fs));
}

// =============================================================================
// Auxiliary: the chain is fully feedforward (no loop-gain class), but the
// one-poles are recursive -- sanity-check the impulse response cannot grow at
// the narrowest band / longest ringing setting. noise/failure MUST be 0 so the
// response is a decaying sequence.
// =============================================================================
static void filterStabilityTest (double Fs)
{
    Surikire e;
    e.prepare (Fs, 1);
    e.setWow01 (0.0);
    e.setFlutter01 (0.0);
    e.setGeneration01 (1.0); // narrowest gen band: HP400 / LP1000
    e.setUserHpHz (20.0);
    e.setUserLpHz (1000.0);
    e.setSaturate01 (0.0);
    e.setNoise01 (0.0);
    e.setFailure01 (0.0);
    e.setMix01 (1.0);
    e.reset();

    auto proc = [&e] (double x) -> double
    {
        float s = (float) x;
        float* ch[1] = { &s };
        e.process (ch, 1, 1);
        return (double) s;
    };
    check (fct::impulseResponseNonIncreasing (proc, Fs, 2.0, 0.25, 1.05),
           at ("surikire filter cascade IR non-increasing", Fs));
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        std::printf ("surikire dsp @ Fs=%.0f\n", Fs);
        wowFlutterTransparencyTest (Fs);
        wowFlutterBesselSidebandTest (Fs);
        wowFlutterDualModCarrierTest (Fs);
        wowFlutterMaxDepthNoClampTest (Fs);
        filterResponseTest (Fs);
        saturationTest (Fs);
        noiseFloorTest (Fs);
        dropoutScheduleTest (Fs);
        longHoldWorstCaseTest (Fs);
        nanRecoveryTest (Fs);
        resetResidueTest (Fs);
        filterStabilityTest (Fs);
    }

    if (g_failures)
    {
        std::fprintf (stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf ("all Surikire DSP tests passed\n");
    return 0;
}
