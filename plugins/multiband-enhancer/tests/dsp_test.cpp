//
// dsp_test.cpp — headless verification of the Tatsumin Enhancer DSP core
// (factory_core::MultibandEnhancer and its new primitives). Links factory_core
// only; runs the full sample-rate matrix. Every quantitative oracle is an
// INDEPENDENT code path (closed-form Fourier series / fixed spec constants /
// black-box superposition), never derived from the implementation under test.
//
// v1.0.0 model (issues #81 / #71): a single Mix (constant-voltage dry/enhanced
// blend), per-band Enhance 0..150 %, per-band Mode and per-band Solo. The gates
// below are re-derived at the NEW worst case (enh 150 %, mix 100 %, per-band mode
// combinations); no tolerance/oracle is loosened.
//
// Gate map (see docs/regression-policy.md):
//   G1  phase coherence / flatness (defect #1)      G9  state reset (Class E)
//   G2  latency exact                               G10 no feedback (Class A)
//   G3  harmonic oracle, all modes x both quality   G11 resolution-follows-rate (Class G)
//   G4  aliasing                                    G12 crossover order clamp
//   G5  DC removal                                  G13 clickless per-band mode transition
//   G6  width oracle                                G14 clickless quality switch (defect #69)
//   G7  Glue levelling                              G15 mix law (constant-voltage)
//   G8  finiteness + worst-case hold (Class C)      G16 solo routing (superposition)
//                                                   G17 per-band mode independence
//
// Linear-phase crossover option (issue #72, HQ-only):
//   G18 linear split reconstruction == pure delay (flat, all rates, extreme fc)
//   G19 linear phase: tap symmetry + measured group-delay flatness
//   G20 crossover -6 dB at fc + mastering-grade stopband
//   G21 reported latency == FIR group delay + OS latency; taps follow the rate
//   G22 engine linear mode: enh-0 flatness, worst-case finiteness, redesign-on-drag
//
// Output gain / delta-listen / IIR extreme-hi coverage (audit additions):
//   G23 output gain law (dB oracle, relative to 0 dB) + clickless mid-stream change
//   G24 delta-listen routes the residual bus to the MAIN output; enh-0 -> silence
//   G25 standard-phase (IIR) flatness at the extreme-hi crossover set
//
#include "factory_core/MultibandEnhancer.h"
#include "factory_core/LinearPhaseCrossover5.h"
#include "factory_core/Oversampler.h"
#include "factory_core/HarmonicShaper.h"
#include "factory_core/FFT.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    using ME = factory_core::MultibandEnhancer;
    using Mode = factory_core::HarmonicShaper::Mode;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    bool g_verbose = false;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
    double linToDb (double l)  { return 20.0 * std::log10 (std::max (l, 1e-300)); }

    // Drive the engine block-by-block; mono input duplicated to both channels.
    void runEngine (ME& eng,
                    const std::vector<double>& inL, const std::vector<double>& inR,
                    std::vector<double>& outL, std::vector<double>& outR,
                    std::vector<double>& dL, std::vector<double>& dR, int block = 256)
    {
        const int N = (int) inL.size();
        outL.assign ((size_t) N, 0.0); outR.assign ((size_t) N, 0.0);
        dL.assign ((size_t) N, 0.0);   dR.assign ((size_t) N, 0.0);
        std::vector<float> bL ((size_t) block), bR ((size_t) block), bdL ((size_t) block), bdR ((size_t) block);
        for (int s = 0; s < N; s += block)
        {
            const int n = std::min (block, N - s);
            for (int i = 0; i < n; ++i) { bL[(size_t) i] = (float) inL[(size_t) (s + i)]; bR[(size_t) i] = (float) inR[(size_t) (s + i)]; }
            eng.processBlock (bL.data(), bR.data(), n, bdL.data(), bdR.data());
            for (int i = 0; i < n; ++i)
            {
                outL[(size_t) (s + i)] = bL[(size_t) i]; outR[(size_t) (s + i)] = bR[(size_t) i];
                dL[(size_t) (s + i)]   = bdL[(size_t) i]; dR[(size_t) (s + i)]   = bdR[(size_t) i];
            }
        }
    }

    // Amplitude of a real signal at FFT bin k over a length-2^order window
    // starting at `start` (rectangular window; use bin-aligned tones for no leak).
    double binAmp (const std::vector<double>& x, size_t start, int order, int k, factory_core::FFT& fft)
    {
        const int N = 1 << order;
        std::vector<cd> buf ((size_t) N);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = cd ((start + (size_t) i < x.size()) ? x[start + (size_t) i] : 0.0, 0.0);
        fft.forward (buf.data());
        return 2.0 * std::abs (buf[(size_t) k]) / (double) N;
    }

    // Full magnitude spectrum (dB) of an impulse response.
    void magSpectrum (const std::vector<double>& ir, int order, factory_core::FFT& fft, std::vector<double>& magDb)
    {
        const int N = 1 << order;
        std::vector<cd> buf ((size_t) N);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = cd ((size_t) i < ir.size() ? ir[(size_t) i] : 0.0, 0.0);
        fft.forward (buf.data());
        magDb.assign ((size_t) (N / 2), -300.0);
        for (int k = 0; k < N / 2; ++k) magDb[(size_t) k] = 20.0 * std::log10 (std::max (std::abs (buf[(size_t) k]), 1e-300));
    }

    int binOf (double f, double fs, int order) { return (int) std::llround (f * (double) (1 << order) / fs); }
    double freqOfBin (int k, double fs, int order) { return (double) k * fs / (double) (1 << order); }

    // ---- configuration helpers ------------------------------------------------
    void setAllMode (ME& eng, Mode m) { for (int b = 0; b < 5; ++b) eng.setMode (b, m); }

    void configFlat (ME& eng)  // neutral, enhance 0 (mix set by the caller)
    {
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 0.0); eng.setWidth (b, 100.0); }
        eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
        setAllMode (eng, Mode::Tube);
        eng.setOutputDb (0.0);
        eng.setDeltaListen (false);
    }

    // Run one 1-sample block (applies a pending quality switch / active path).
    void tick (ME& eng) { float l = 0, r = 0, dl = 0, dr = 0; eng.processBlock (&l, &r, 1, &dl, &dr); }

    // Warm the smoothers to steady state on silence.
    void warmup (ME& eng, double fs, double seconds = 0.35)
    {
        const int N = (int) (seconds * fs);
        std::vector<double> z ((size_t) N, 0.0), oL, oR, dL, dR;
        runEngine (eng, z, z, oL, oR, dL, dR);
    }

    // ==========================================================================
    // G1 — phase coherence / flatness (defect #1: comb filtering)
    // ==========================================================================
    // With enhance 0 the residual bus is silent, so the enhanced bus's linear
    // reconstruction is the pure band sum == allpass(x) == the direct bus. The
    // constant-voltage blend out = (1-mix)*direct + mix*linWet is therefore FLAT
    // at 0 dB for EVERY mix — a comb (any phase offset between direct and the
    // reconstruction, defect #1) would ripple the magnitude, worst at mix 50 %
    // where the two buses carry equal weight.
    void g1_flatness (double fs)
    {
        std::printf ("G1 flatness/phase-coherence @ %.0f\n", fs);
        const int order = 15;
        const int Nfft  = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        struct XSet { double f1, f2, f3, f4; const char* name; };
        const XSet sets[] = {
            { 130.0, 700.0, 2200.0, 7500.0, "default" },
            { 40.0,  50.4,  63.5,   80.0,   "packed-lo" },
        };

        for (auto quality : { ME::Quality::HQ, ME::Quality::ZeroLatency })
        {
            const bool zl = (quality == ME::Quality::ZeroLatency);
            const double loHz = zl ? 10.0 : 20.0;
            const double hiFrac = zl ? 0.49 : 0.42;
            const double tolA = zl ? 0.10 : 0.25;

            for (const auto& xs : sets)
            {
                for (double mixPct : { 0.0, 50.0, 100.0 })
                {
                    ME eng; eng.prepare (fs, 256);
                    configFlat (eng);
                    eng.setQuality (quality);
                    eng.setCrossovers (xs.f1, xs.f2, xs.f3, xs.f4);
                    eng.setMix (mixPct);
                    warmup (eng, fs);

                    std::vector<double> in ((size_t) Nfft, 0.0), inR, oL, oR, dL, dR;
                    in[0] = 1.0; inR = in;
                    runEngine (eng, in, inR, oL, oR, dL, dR);

                    std::vector<double> magDb;
                    magSpectrum (oL, order, fft, magDb);
                    const double target = 0.0; // constant-voltage + enh 0 => flat 0 dB at every mix
                    double maxDev = 0.0; double atWorst = 0.0;
                    for (int k = 1; k < Nfft / 2; ++k)
                    {
                        const double f = freqOfBin (k, fs, order);
                        if (f < loHz || f > hiFrac * fs) continue;
                        const double dev = std::abs (magDb[(size_t) k] - target);
                        if (dev > maxDev) { maxDev = dev; atWorst = f; }
                    }
                    if (g_verbose)
                        std::printf ("   [%s %s mix%.0f] maxDev=%.4f dB @ %.0f Hz (tol %.2f)\n",
                                     zl ? "ZL" : "HQ", xs.name, mixPct, maxDev, atWorst, tolA);
                    if (maxDev > tolA)
                        fail ("G1 " + std::string (zl ? "ZL " : "HQ ") + xs.name + " mix" + std::to_string ((int) mixPct)
                              + " dev " + std::to_string (maxDev) + " dB > " + std::to_string (tolA));
                }
            }
        }
    }

    // ==========================================================================
    // G2 — latency exact
    // ==========================================================================
    // HQ oversamples at EVERY supported rate (M = 4 below 50 kHz, else 2; #70), and
    // both ratios round-trip to exactly 51 host samples (2H/M), so HQ latency is a
    // uniform 51; only M=1 (Zero-Latency) is 0. The M->latency map is independently
    // re-verified by the impulse bracket below (sub-test ii).
    int expectedLatency (double fs) { return (ME::hqFactor (fs) == 1) ? 0 : 51; }

    void g2_latency (double fs)
    {
        std::printf ("G2 latency @ %.0f\n", fs);
        // (i) The engine reports the design constant.
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::HQ);
            tick (eng); // apply active-path selection
            const int rep = eng.latencySamples();
            const int exp = expectedLatency (fs);
            if (g_verbose) std::printf ("   HQ report=%d expect=%d\n", rep, exp);
            if (rep != exp) fail ("G2 HQ latency report " + std::to_string (rep) + " != " + std::to_string (exp));

            // Regression (#70): at 176.4/192 kHz HQ now oversamples (M=2) too, so it
            // must publish 51 there as well — NOT the old 0 (M=1). Assert against the
            // hardcoded constant (independent of hqFactor) at the high rates.
            if (fs >= 100000.0 && rep != 51)
                fail ("G2 #70 high-rate HQ latency " + std::to_string (rep) + " != 51");

            ME z; z.prepare (fs, 256); configFlat (z); z.setQuality (ME::Quality::ZeroLatency); tick (z);
            if (z.latencySamples() != 0) fail ("G2 ZL latency report != 0");
        }
        // (ii) The oversampler bracket (the element that PRODUCES the reported
        // latency) delays an impulse by exactly 2H/M. The full engine path also
        // runs the crossover allpass, whose frequency-dependent group delay is
        // an inherent (non-reportable) phase response, so we gate the bracket.
        for (int M : { 1, 2, 4 })
        {
            factory_core::Oversampler up, dn;
            up.prepare (fs, M, 256); dn.prepare (fs, M, 256);
            const int n = 256;
            std::vector<float> in ((size_t) n, 0.0f), os ((size_t) (n * M), 0.0f), out ((size_t) n, 0.0f);
            in[0] = 1.0f;
            up.processUp (in.data(), n, os.data());
            dn.processDown (os.data(), n, out.data());
            int arg = 0; double pk = 0.0;
            for (int i = 0; i < n; ++i) if (std::fabs ((double) out[(size_t) i]) > pk) { pk = std::fabs ((double) out[(size_t) i]); arg = i; }
            const int expArg = up.latencyHostSamples();
            if (g_verbose) std::printf ("   OS M=%d argmax=%d expect=%d\n", M, arg, expArg);
            if (arg != expArg) fail ("G2 OS M=" + std::to_string (M) + " argmax " + std::to_string (arg) + " != " + std::to_string (expArg));
        }
    }

    // ==========================================================================
    // G3 — harmonic oracle (all 5 modes x both quality), on the shaper unit.
    // Fourier closed form for u = a sin(theta), f(u) = u + sum c_k e u^k,
    // independently re-derived here.
    // ==========================================================================
    struct HOracle { double H1, H2, H3, H4, H5; };
    HOracle harmonicOracle (const double c[4], double e, double a)
    {
        const double a2 = a * a, a3 = a2 * a, a4 = a3 * a, a5 = a4 * a;
        HOracle o;
        o.H1 = std::abs (a + 0.75 * a3 * e * c[1] + (10.0 / 16.0) * a5 * e * c[3]);
        o.H2 = std::abs (0.5 * a2 * e * c[0] + 0.5 * a4 * e * c[2]);
        o.H3 = std::abs (0.25 * a3 * e * c[1] + (5.0 / 16.0) * a5 * e * c[3]);
        o.H4 = std::abs (0.125 * a4 * e * c[2]);
        o.H5 = std::abs ((1.0 / 16.0) * a5 * e * c[3]);
        return o;
    }

    void g3_harmonics (double fs)
    {
        std::printf ("G3 harmonic oracle @ %.0f\n", fs);
        // Independent coefficient table (NOT read from the implementation).
        struct M { Mode mode; const char* name; double c[4]; };
        const M modes[] = {
            { Mode::Tube,   "Tube",   { 0.35,  0.08, 0.00, 0.00 } },
            { Mode::Tape,   "Tape",   { 0.00, -0.30, 0.00, 0.06 } },
            { Mode::Bright, "Bright", { 0.08,  0.10, 0.10, 0.15 } },
            { Mode::Clean,  "Clean",  { 0.20, -0.10, 0.00, 0.00 } },
            { Mode::Glue,   "Glue",   { 0.00, -0.30, 0.00, 0.06 } },
        };
        const int order = 14;
        const int Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const double e = 1.0, a = 0.25;
        const int k0 = binOf (1000.0, fs, order);
        const double f0 = freqOfBin (k0, fs, order);

        for (const auto& m : modes)
        {
            for (bool adaa : { false, true })
            {
                factory_core::HarmonicShaper sh;
                sh.prepare (fs);
                sh.setMode (m.mode);
                sh.setEnhance (e);
                sh.setAdaa (adaa);
                sh.setEnvGain (1.0); // Glue at g=1 == Tape residual (oracle uses Tape coeffs)
                sh.reset();          // snap coeffs to the mode (measure the pure curve, not a mid-morph)

                const int settle = 2048;
                std::vector<double> y ((size_t) Nfft, 0.0);
                for (int i = 0; i < settle; ++i) { double x = a * std::sin (2.0 * kPi * f0 * i / fs); sh.processResidual (x); }
                for (int i = 0; i < Nfft; ++i)
                {
                    double x = a * std::sin (2.0 * kPi * f0 * (settle + i) / fs);
                    y[(size_t) i] = x + sh.processResidual (x);
                }

                const HOracle o = harmonicOracle (m.c, e, a);
                const double H[5] = { o.H1, o.H2, o.H3, o.H4, o.H5 };
                const double meas[5] = {
                    binAmp (y, 0, order, 1 * k0, fft), binAmp (y, 0, order, 2 * k0, fft),
                    binAmp (y, 0, order, 3 * k0, fft), binAmp (y, 0, order, 4 * k0, fft),
                    binAmp (y, 0, order, 5 * k0, fft)
                };
                const double tol = adaa ? 1.0 : 0.5;
                for (int h = 0; h < 5; ++h)
                {
                    if (H[h] < 1.0e-4) continue; // harmonic absent for this mode; skip dB compare
                    const double dev = std::abs (linToDb (meas[h]) - linToDb (H[h]));
                    const double htol = (h == 0) ? 0.2 : tol;
                    if (g_verbose)
                        std::printf ("   %-6s %s H%d: oracle %.2f dB meas %.2f dB dev %.3f\n",
                                     m.name, adaa ? "ADAA" : "raw ", h + 1, linToDb (H[h]), linToDb (meas[h]), dev);
                    if (dev > htol)
                        fail ("G3 " + std::string (m.name) + (adaa ? " ADAA" : " raw") + " H" + std::to_string (h + 1)
                              + " dev " + std::to_string (dev) + " dB > " + std::to_string (htol));
                }
            }
        }
    }

    // ==========================================================================
    // G4 — aliasing (worst case: enhance 150 %, mix 100 %, Bright)
    // ==========================================================================
    void g4_alias (double fs)
    {
        std::printf ("G4 aliasing @ %.0f\n", fs);
        const int order = 15;
        const int Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        // HQ oversamples at EVERY supported rate now (M = 4 below 50 kHz, else 2;
        // #70): the fundamental's ultrasonic harmonics are pushed above the host
        // Nyquist and removed by the decimation FIR before they can fold, so the
        // audible-band aliasing holds the OS grade (-78 dBFS) at ALL rates — incl.
        // 176.4/192 kHz, which previously ran M=1 ADAA-only and folded a ~-38 dBFS
        // 3rd-harmonic image near 19 kHz (this gate used to be relaxed to -45 dB
        // there). The a=1.0 abuse case stresses the oversampler at full scale and
        // now runs at every rate too.
        struct Case { ME::Quality q; double a; double ffrac; double tol; const char* name; };
        const Case cases[] = {
            { ME::Quality::HQ,          0.5, 0.30, -78.0, "HQ" },
            { ME::Quality::ZeroLatency, 0.5, 0.15, -45.0, "ZL" },
            { ME::Quality::HQ,          1.0, 0.30, -60.0, "HQ-abuse" },
        };
        for (const auto& c : cases)
        {
            const int k0 = binOf (c.ffrac * fs, fs, order);
            const double f0 = freqOfBin (k0, fs, order);
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 100.0); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            setAllMode (eng, Mode::Bright);
            eng.setQuality (c.q);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs);

            const int settle = (int) (0.25 * fs); // let the 5 Hz DC blocker + crossover settle
            std::vector<double> inL ((size_t) (settle + Nfft)), inR, oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i) inL[(size_t) i] = c.a * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);

            std::vector<double> seg (oL.begin() + settle, oL.begin() + settle + Nfft);
            std::vector<cd> buf ((size_t) Nfft);
            for (int i = 0; i < Nfft; ++i) buf[(size_t) i] = cd (seg[(size_t) i], 0.0);
            fft.forward (buf.data());
            // Aliasing = folded images of the out-of-band harmonics that land in the
            // AUDIBLE band (20 Hz .. 20 kHz). Exclude DC (< 20 Hz: the DC-blocker
            // transient, not aliasing) and the in-band harmonics k*f0 < fs/2 (those
            // are the wanted signal). Everything else in-band is an alias/noise spur.
            const double audibleHi = std::min (20000.0, 0.45 * fs);
            double maxSpur = 0.0; int spurBin = 0;
            for (int k = 1; k < Nfft / 2; ++k)
            {
                const double f = freqOfBin (k, fs, order);
                if (f < 20.0 || f > audibleHi) continue;
                bool inBandHarmonic = false;
                for (int h = 1; (double) h * f0 < 0.5 * fs; ++h)
                    if (std::abs (k - h * k0) <= 2) { inBandHarmonic = true; break; }
                if (inBandHarmonic) continue;
                const double amp = 2.0 * std::abs (buf[(size_t) k]) / (double) Nfft;
                if (amp > maxSpur) { maxSpur = amp; spurBin = k; }
            }
            const double spurDb = linToDb (maxSpur);
            std::printf ("   %-8s a=%.1f f0=%.0f  worst audible alias %.1f dBFS @ %.0f Hz (gate %.0f)\n",
                         c.name, c.a, f0, spurDb, freqOfBin (spurBin, fs, order), c.tol);
            if (spurDb > c.tol)
                fail ("G4 " + std::string (c.name) + " alias " + std::to_string (spurDb) + " dBFS > " + std::to_string (c.tol));
        }
    }

    // ==========================================================================
    // G5 — DC removal (enhance 150 %, mix 100 %)
    // ==========================================================================
    void g5_dc (double fs)
    {
        std::printf ("G5 DC removal @ %.0f\n", fs);
        const int order = 15, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        for (Mode m : { Mode::Tube, Mode::Clean, Mode::Bright })
        {
            const int k0 = binOf (1000.0, fs, order);
            const double f0 = freqOfBin (k0, fs, order);
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 100.0); }
            setAllMode (eng, m);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs);
            const int settle = (int) (0.3 * fs); // 5 Hz DC blocker needs ~0.16 s to settle
            std::vector<double> inL ((size_t) (settle + Nfft)), inR, oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i) inL[(size_t) i] = 0.5 * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            std::vector<double> seg (oL.begin() + settle, oL.begin() + settle + Nfft);
            std::vector<cd> buf ((size_t) Nfft);
            for (int i = 0; i < Nfft; ++i) buf[(size_t) i] = cd (seg[(size_t) i], 0.0);
            fft.forward (buf.data());
            const double dc = std::abs (buf[0]) / (double) Nfft; // DC magnitude (mean)
            const double dcDb = linToDb (dc);
            if (g_verbose) std::printf ("   mode=%d DC=%.1f dBFS\n", (int) m, dcDb);
            if (dcDb > -70.0) fail ("G5 DC " + std::to_string (dcDb) + " dBFS > -70");
        }
    }

    // ==========================================================================
    // G6 — width oracle (M/S), linear path isolated at enhance 0, mix 100 %
    // ==========================================================================
    void g6_width (double fs)
    {
        std::printf ("G6 width @ %.0f\n", fs);
        const int order = 14, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const int k0 = binOf (1000.0, fs, order);
        const double f0 = freqOfBin (k0, fs, order);

        // The width M/S transform is exact only when every band shares the same
        // width (a single band at f0 does not carry the full signal — LR4 bands
        // overlap). Uniform width => enhanced = w * sum(b_i) = w * S, so the S
        // ratio is exactly w and M is untouched. Enhance 0 + mix 100 % isolates the
        // linear width path (out = linWet, no residual, no direct blend).
        auto measS = [&] (double widthPct, bool sideSignal) -> double
        {
            ME eng; eng.prepare (fs, 256);
            configFlat (eng);
            for (int j = 0; j < 5; ++j) eng.setWidth (j, widthPct);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs);
            const int settle = 2048;
            std::vector<double> inL ((size_t) (settle + Nfft)), inR ((size_t) (settle + Nfft)), oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i)
            {
                const double s = 0.5 * std::sin (2.0 * kPi * f0 * i / fs);
                inL[(size_t) i] = s;
                inR[(size_t) i] = sideSignal ? -s : s;
            }
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            std::vector<double> comp ((size_t) Nfft);
            for (int i = 0; i < Nfft; ++i)
                comp[(size_t) i] = sideSignal ? 0.5 * (oL[(size_t) (settle + i)] - oR[(size_t) (settle + i)])
                                              : 0.5 * (oL[(size_t) (settle + i)] + oR[(size_t) (settle + i)]);
            return binAmp (comp, 0, order, k0, fft);
        };

        // pure S input, uniform width 100 -> 200 == +6.02 dB, 0 == muted.
        const double s100 = measS (100.0, true);
        const double s200 = measS (200.0, true);
        const double s0   = measS (0.0,   true);
        const double dS   = linToDb (s200) - linToDb (s100);
        if (g_verbose) std::printf ("   uniform S: w200-w100=%.3f dB, w0 rel=%.1f dB\n", dS, linToDb (s0) - linToDb (s100));
        if (std::abs (dS - 6.0206) > 0.2) fail ("G6 uniform width200 S " + std::to_string (dS) + " != +6.02");
        if (linToDb (s0) - linToDb (s100) > -60.0) fail ("G6 width0 S not muted");

        // pure M input -> width invariant (mono compatibility).
        const double m100 = measS (100.0, false);
        const double m200 = measS (200.0, false);
        if (g_verbose) std::printf ("   uniform M: w200-w100=%.4f dB\n", linToDb (m200) - linToDb (m100));
        if (std::abs (linToDb (m200) - linToDb (m100)) > 0.05) fail ("G6 M changed with width");

        // per-band wiring: soloing each band's width to 200 (others 100) must
        // audibly raise the S energy near that band's centre (knob is routed).
        const double centres[5] = { 80.0, 400.0, 1300.0, 4200.0, 11000.0 };
        for (int b = 0; b < 5; ++b)
        {
            const int kb = binOf (centres[b], fs, order);
            const double fb = freqOfBin (kb, fs, order);
            auto sAt = [&] (int solo) -> double
            {
                ME eng; eng.prepare (fs, 256);
                configFlat (eng);
                for (int j = 0; j < 5; ++j) eng.setWidth (j, 100.0);
                if (solo >= 0) eng.setWidth (solo, 200.0);
                eng.setMix (100.0); eng.setOutputDb (0.0);
                warmup (eng, fs);
                const int settle = 2048;
                std::vector<double> inL ((size_t) (settle + Nfft)), inR ((size_t) (settle + Nfft)), oL, oR, dL, dR;
                for (int i = 0; i < settle + Nfft; ++i) { const double s = 0.5 * std::sin (2.0 * kPi * fb * i / fs); inL[(size_t) i] = s; inR[(size_t) i] = -s; }
                runEngine (eng, inL, inR, oL, oR, dL, dR);
                std::vector<double> sc ((size_t) Nfft);
                for (int i = 0; i < Nfft; ++i) sc[(size_t) i] = 0.5 * (oL[(size_t) (settle + i)] - oR[(size_t) (settle + i)]);
                return binAmp (sc, 0, order, kb, fft);
            };
            const double base = sAt (-1);
            const double solo = sAt (b);
            const double up = linToDb (solo) - linToDb (base);
            if (g_verbose) std::printf ("   band %d width-solo raise=%.2f dB\n", b, up);
            if (up < 1.0) fail ("G6 band " + std::to_string (b) + " width knob not routed (" + std::to_string (up) + " dB)");
        }
    }

    // ==========================================================================
    // G7 — Glue levelling (mix 100 %, all bands Glue)
    // ==========================================================================
    void g7_glue (double fs)
    {
        std::printf ("G7 Glue levelling @ %.0f\n", fs);
        const int order = 14, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const int k0 = binOf (1000.0, fs, order);
        const double f0 = freqOfBin (k0, fs, order);

        auto ratioDb = [&] (Mode mode, double aDb) -> double
        {
            const double a = dbToLin (aDb);
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            setAllMode (eng, mode);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            eng.setQuality (ME::Quality::HQ);
            warmup (eng, fs, 0.5);
            const int settle = (int) (0.4 * fs);
            std::vector<double> inL ((size_t) (settle + Nfft)), inR, oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i) inL[(size_t) i] = a * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            const double H1 = binAmp (oL, (size_t) settle, order, k0, fft);
            const double H3 = binAmp (oL, (size_t) settle, order, 3 * k0, fft);
            return linToDb (H3) - linToDb (H1);
        };

        // (a) level-sensitivity of H3/H1 over a 12 dB input change (-24 -> -12 dBFS)
        const double tapeLo = ratioDb (Mode::Tape, -24.0), tapeHi = ratioDb (Mode::Tape, -12.0);
        const double glueLo = ratioDb (Mode::Glue, -24.0), glueHi = ratioDb (Mode::Glue, -12.0);
        const double tapeSens = tapeHi - tapeLo; // ~ +24 dB (proportional to a^2)
        const double glueSens = glueHi - glueLo; // ~ +12 dB (proportional to a, alpha=0.5)
        std::printf ("   Tape H3/H1 sens=%.1f dB (expect ~24), Glue sens=%.1f dB (expect ~12)\n", tapeSens, glueSens);
        if (std::abs (tapeSens - 24.0) > 3.0) fail ("G7 Tape sensitivity " + std::to_string (tapeSens) + " != ~24");
        if (std::abs (glueSens - 12.0) > 4.0) fail ("G7 Glue sensitivity " + std::to_string (glueSens) + " != ~12");

        // (b) -80 dBFS sine -> delta output <= -100 dBFS (absolute floor / Class J)
        {
            const double a = dbToLin (-80.0);
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) eng.setEnhance (b, 100.0);
            setAllMode (eng, Mode::Glue); eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs);
            const int N = (int) (0.5 * fs);
            std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
            for (int i = 0; i < N; ++i) inL[(size_t) i] = a * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            double pk = 0.0; for (int i = N / 2; i < N; ++i) pk = std::max (pk, std::abs (dL[(size_t) i]));
            if (g_verbose) std::printf ("   -80dBFS -> delta peak %.1f dBFS\n", linToDb (pk));
            if (linToDb (pk) > -100.0) fail ("G7 floor: delta " + std::to_string (linToDb (pk)) + " dBFS > -100");
        }
        // (c) silence -> zero delta
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) eng.setEnhance (b, 100.0);
            setAllMode (eng, Mode::Glue); eng.setMix (100.0);
            const int N = (int) (2.0 * fs);
            std::vector<double> z ((size_t) N, 0.0), oL, oR, dL, dR;
            runEngine (eng, z, z, oL, oR, dL, dR);
            double e = 0.0; for (double v : dL) e += v * v;
            if (e != 0.0) fail ("G7 silence produced nonzero delta energy " + std::to_string (e));
        }
    }

    // ==========================================================================
    // G8 — finiteness + worst-case hold + NaN self-heal (Class C)
    // Worst case: enhance 150 %, width 200 %, Glue, mix 100 %, 0 dBFS square.
    // ==========================================================================
    void g8_finite (double fs)
    {
        std::printf ("G8 finiteness / NaN self-heal @ %.0f\n", fs);
        auto worst = [&] (bool injectNaN, std::vector<double>& out)
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 200.0); }
            setAllMode (eng, Mode::Glue);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            const int N = (int) (10.0 * fs);
            const int period = std::max (2, (int) (fs / 55.0));
            std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
            for (int i = 0; i < N; ++i) inL[(size_t) i] = ((i % period) < period / 2) ? 1.0 : -1.0; // 0 dBFS square, 55 Hz
            if (injectNaN) inL[(size_t) (N / 4)] = std::nan ("");
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            out = oL;
        };
        std::vector<double> clean, withNaN;
        worst (false, clean);
        worst (true,  withNaN);
        if (! factory_core::testing::allFinite (clean)) fail ("G8 clean not finite");
        if (! factory_core::testing::allFinite (withNaN)) fail ("G8 NaN run not finite (no self-heal)");
        const double pk = factory_core::testing::peakAbs (clean);
        if (pk > dbToLin (12.0)) fail ("G8 peak " + std::to_string (linToDb (pk)) + " dBFS > +12");
        // self-heal: 0.5 s after the NaN (injected at N/4), RMS diff vs clean is tiny.
        const size_t start = (size_t) (10.0 * fs / 4.0 + 0.5 * fs);
        double e = 0.0; size_t cnt = 0;
        for (size_t i = start; i < clean.size(); ++i) { const double d = clean[i] - withNaN[i]; e += d * d; ++cnt; }
        const double rms = std::sqrt (e / std::max<size_t> (1, cnt));
        if (g_verbose) std::printf ("   peak=%.2f dBFS  self-heal RMS diff=%.2e\n", linToDb (pk), rms);
        if (rms > 1e-4) fail ("G8 self-heal RMS diff " + std::to_string (rms) + " > 1e-4");
    }

    // ==========================================================================
    // G9 — state reset (Class E)
    // ==========================================================================
    void g9_reset (double fs)
    {
        std::printf ("G9 state reset @ %.0f\n", fs);
        for (auto q : { ME::Quality::HQ, ME::Quality::ZeroLatency })
        {
            auto impulseAfter = [&] (bool dirty) -> std::vector<double>
            {
                ME eng; eng.prepare (fs, 256);
                for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 80.0); eng.setWidth (b, 120.0); }
                setAllMode (eng, Mode::Tape); eng.setQuality (q);
                eng.setMix (100.0);
                eng.reset();                    // canonical state (both runs start identical)
                if (dirty)
                {
                    const int N = (int) fs;
                    std::vector<double> nz ((size_t) N), nr, oL, oR, dL, dR;
                    unsigned s = 22222;
                    for (int i = 0; i < N; ++i) { s = s * 1664525u + 1013904223u; nz[(size_t) i] = ((double) (s >> 9) / 8388608.0 - 1.0) * 0.5; }
                    nr = nz;
                    runEngine (eng, nz, nr, oL, oR, dL, dR);
                    eng.reset();                // reset from a dirtied state -> must match canonical
                }
                const int M = 2048;
                std::vector<double> in ((size_t) M, 0.0), inR, oL, oR, dL, dR;
                in[0] = 1.0; inR = in;
                runEngine (eng, in, inR, oL, oR, dL, dR);
                return oL;
            };
            const auto a = impulseAfter (false);
            const auto b = impulseAfter (true);
            double maxDiff = 0.0;
            for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max (maxDiff, std::abs (a[i] - b[i]));
            if (g_verbose) std::printf ("   %s max reset diff=%.2e\n", q == ME::Quality::HQ ? "HQ" : "ZL", maxDiff);
            if (maxDiff > 1e-9) fail ("G9 reset diff " + std::to_string (maxDiff) + " > 1e-9");
        }
    }

    // ==========================================================================
    // G10 — no feedback path (Class A): impulse response energy non-increasing.
    // Worst case: enhance 150 %, width 200 %, Glue, mix 100 %.
    // ==========================================================================
    void g10_feedback (double fs)
    {
        std::printf ("G10 feedforward-only @ %.0f\n", fs);
        ME eng; eng.prepare (fs, 256);
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 200.0); }
        setAllMode (eng, Mode::Glue); eng.setMix (100.0); eng.setOutputDb (0.0);
        warmup (eng, fs);
        auto proc = [&] (double x) -> double
        {
            float l = (float) x, r = (float) x, dl = 0.0f, dr = 0.0f;
            eng.processBlock (&l, &r, 1, &dl, &dr);
            return (double) l;
        };
        if (! factory_core::testing::impulseResponseNonIncreasing (proc, fs))
            fail ("G10 impulse response energy increasing (feedback?)");
    }

    // ==========================================================================
    // G11 — resolution follows sample rate (Class G)
    // ==========================================================================
    void g11_resolution (double fs)
    {
        std::printf ("G11 resolution follows rate @ %.0f\n", fs);
        const int order = factory_core::fftOrderForSampleRate (fs, 13, 48000.0, 15);
        const double bin = factory_core::testing::binWidthHz (fs, order);
        const double win = factory_core::testing::windowLengthSec (fs, order);
        if (g_verbose) std::printf ("   order=%d bin=%.2f Hz window=%.3f s\n", order, bin, win);
        if (bin > 8.0) fail ("G11 bin width " + std::to_string (bin) + " Hz > 8");
        if (win < 0.12 || win > 0.25) fail ("G11 window " + std::to_string (win) + " s out of [0.12,0.25]");
    }

    // ==========================================================================
    // G12 — crossover order clamp
    // ==========================================================================
    void g12_clamp (double fs)
    {
        std::printf ("G12 crossover clamp @ %.0f\n", fs);
        ME eng; eng.prepare (fs, 256); configFlat (eng);
        eng.setCrossovers (3000.0, 200.0, 5000.0, 100.0); // reversed / bunched
        tick (eng);
        // pump enough blocks for the log-smoother to settle onto the (clamped) values
        warmup (eng, fs, 0.2);
        double f[4]; for (int i = 0; i < 4; ++i) f[i] = eng.effectiveCrossoverHz (i);
        if (g_verbose) std::printf ("   effective xover=[%.1f %.1f %.1f %.1f]\n", f[0], f[1], f[2], f[3]);
        for (int i = 1; i < 4; ++i)
        {
            if (f[i] < f[i - 1] * 1.259) fail ("G12 xover not ascending/1.26-spaced at " + std::to_string (i));
        }
    }

    // ==========================================================================
    // G13 — clickless per-band mode transition
    // ==========================================================================
    // Switch every band's mode mid-signal; the shaper's ~30 ms coefficient
    // crossfade must keep the output step-free. Enhance 100 % (the shaper coeff
    // morph is enhance-linear; 100 % keeps the empirical 0.08 step bound meaningful).
    void g13_click (double fs)
    {
        std::printf ("G13 clickless per-band mode switch @ %.0f\n", fs);
        const int k0order = 14;
        const int k0 = binOf (1000.0, fs, k0order);
        const double f0 = freqOfBin (k0, fs, k0order);
        ME eng; eng.prepare (fs, 256);
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
        setAllMode (eng, Mode::Tube); eng.setMix (100.0); eng.setOutputDb (0.0);
        warmup (eng, fs);

        const int N = (int) (0.5 * fs);
        std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
        for (int i = 0; i < N; ++i) inL[(size_t) i] = 0.25 * std::sin (2.0 * kPi * f0 * i / fs);
        inR = inL;
        // steady baseline max step
        std::vector<float> bL (256), bR (256), bdL (256), bdR (256);
        double baseStep = 0.0, switchStep = 0.0; bool switched = false; double prev = 0.0; bool have = false;
        for (int s = 0; s < N; s += 256)
        {
            const int n = std::min (256, N - s);
            for (int i = 0; i < n; ++i) { bL[(size_t) i] = (float) inL[(size_t) (s + i)]; bR[(size_t) i] = (float) inR[(size_t) (s + i)]; }
            if (! switched && s > N / 3) { setAllMode (eng, Mode::Bright); switched = true; }
            eng.processBlock (bL.data(), bR.data(), n, bdL.data(), bdR.data());
            for (int i = 0; i < n; ++i)
            {
                const double v = bL[(size_t) i];
                if (have)
                {
                    const double step = std::abs (v - prev);
                    if (! std::isfinite (v)) fail ("G13 non-finite output");
                    if (s <= N / 3) baseStep = std::max (baseStep, step);
                    else switchStep = std::max (switchStep, step);
                }
                prev = v; have = true;
            }
        }
        if (g_verbose) std::printf ("   base step=%.4f, during switch=%.4f\n", baseStep, switchStep);
        if (switchStep > 0.08) fail ("G13 switch step " + std::to_string (switchStep) + " > 0.08");
    }

    // ==========================================================================
    // G14 — clickless quality switch (defect #69: HQ<->ZeroLatency crossfade)
    // ==========================================================================
    // Drive a steady in-band sinusoid at mix 100 % and toggle Quality back and
    // forth, spaced wider than the ~20 ms fade so fades never overlap. A hard path
    // swap (the old bug) reset the incoming path to silence and dropped the
    // outgoing one, producing a one-sample step ~ the output peak. The equal-power
    // crossfade keeps the output continuous.
    //
    // Oracle is INDEPENDENT of the engine: the worst sample-to-sample step of a
    // click-free output is bounded by the max slew of its spectral content, using
    // amplitudes from the constant-voltage gain SPEC + the closed-form
    // HarmonicShaper series (never measured from the engine). At mix 100 % the
    // total linear gain on the fundamental is 1.0 (not 2.0):
    //   * fundamental amplitude <= 1.0 * A * 1.1   (unity linear reconstruction,
    //     +10% for the H1 self-boost),
    //   * harmonics 2..5 amplitude <= 5 bands * (0.5 A^2|c0| + 0.25 A^3|c1|) for the
    //     driven mode (Tube), each slewing at 2 sin(pi h f0 / fs).
    void g14_quality_click (double fs)
    {
        std::printf ("G14 clickless quality switch @ %.0f\n", fs);
        const double f0 = 300.0;    // in band 1 (130..700 Hz), low enough that even
        const double A  = 0.25;     // the 5th harmonic (1.5 kHz) slews slowly

        // Independent slew bound (spec + closed-form oracle, never the engine).
        const int    Hmax = 5;
        const double c0 = 0.35, c1 = 0.08;                // Tube coeffs (G3's independent table)
        const double a1 = 1.0 * A * 1.1;                  // fundamental amplitude bound (mix 100 %: gain 1.0)
        const double aH = 5.0 * (0.5 * A * A * c0 + 0.25 * A * A * A * c1); // harmonic (2..5) bound at mix 100 %
        const double slew1 = a1 * 2.0 * std::sin (kPi * f0 / fs);
        const double slewH = aH * 2.0 * std::sin (kPi * (double) Hmax * f0 / fs);
        const double kSafety = 2.0;
        const double bound = kSafety * (slew1 + slewH);

        ME eng; eng.prepare (fs, 256);
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
        eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
        setAllMode (eng, Mode::Tube);
        eng.setMix (100.0); eng.setOutputDb (0.0);
        eng.setQuality (ME::Quality::HQ);
        warmup (eng, fs, 0.4);      // settle path + smoothers + DC blocker

        const int N = (int) (2.0 * fs);
        const int toggleEvery = std::max (1, (int) std::lround (0.029 * fs)); // > 20 ms fade
        std::vector<float> bL (256), bR (256), bdL (256), bdR (256);
        double worstStep = 0.0, prev = 0.0;
        bool have = false, finite = true, zl = false;
        int nextToggle = toggleEvery, nSwitch = 0;
        for (int s = 0; s < N; s += 256)
        {
            const int n = std::min (256, N - s);
            for (int i = 0; i < n; ++i)
            {
                const double v = A * std::sin (2.0 * kPi * f0 * (double) (s + i) / fs);
                bL[(size_t) i] = (float) v; bR[(size_t) i] = (float) v;
            }
            if (s >= nextToggle)
            {
                zl = ! zl;
                eng.setQuality (zl ? ME::Quality::ZeroLatency : ME::Quality::HQ);
                nextToggle += toggleEvery; ++nSwitch;
            }
            eng.processBlock (bL.data(), bR.data(), n, bdL.data(), bdR.data());
            for (int i = 0; i < n; ++i)
            {
                const double y = bL[(size_t) i];
                if (! std::isfinite (y)) finite = false;
                if (have) worstStep = std::max (worstStep, std::abs (y - prev));
                prev = y; have = true;
            }
        }
        if (g_verbose)
            std::printf ("   %d switches, worst step=%.4f (bound %.4f, click~%.2f)\n",
                         nSwitch, worstStep, bound, A);
        if (! finite) fail ("G14 non-finite output during quality switch");
        if (worstStep > bound)
            fail ("G14 quality-switch step " + std::to_string (worstStep) + " > bound " + std::to_string (bound));
    }

    // ==========================================================================
    // G15 — mix law (constant-voltage / linear complementary)
    // ==========================================================================
    // (a) mix 0 % is the pure direct (dry) path: flat magnitude even at enhance
    //     150 % and any mode — the residual bus is scaled by mix, so at 0 % no
    //     harmonics reach the output.
    // (b) The intermediate blend follows the SPEC's linear law:
    //     out(50%) == 0.5*(out(0%) + out(100%)) sample-for-sample. This is the
    //     independent oracle (the linear formula applied to the measured 0 %/100 %
    //     endpoints) and distinguishes constant-voltage from an equal-power law
    //     (which would give 0.707*(out0+out100) at the midpoint).
    void g15_mix_law (double fs)
    {
        std::printf ("G15 mix law @ %.0f\n", fs);
        const int order = 15, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        // (a) mix 0 % -> pure dry, flat even at max enhance / any mode.
        for (Mode m : { Mode::Tube, Mode::Bright })
        {
            for (auto q : { ME::Quality::HQ, ME::Quality::ZeroLatency })
            {
                const bool zl = (q == ME::Quality::ZeroLatency);
                const double loHz = zl ? 10.0 : 20.0, hiFrac = zl ? 0.49 : 0.42, tolA = zl ? 0.10 : 0.25;
                ME eng; eng.prepare (fs, 256);
                configFlat (eng);
                for (int b = 0; b < 5; ++b) eng.setEnhance (b, 150.0);
                setAllMode (eng, m);
                eng.setQuality (q);
                eng.setMix (0.0);
                warmup (eng, fs);
                std::vector<double> in ((size_t) Nfft, 0.0), inR, oL, oR, dL, dR;
                in[0] = 1.0; inR = in;
                runEngine (eng, in, inR, oL, oR, dL, dR);
                std::vector<double> magDb; magSpectrum (oL, order, fft, magDb);
                double maxDev = 0.0;
                for (int k = 1; k < Nfft / 2; ++k)
                {
                    const double f = freqOfBin (k, fs, order);
                    if (f < loHz || f > hiFrac * fs) continue;
                    maxDev = std::max (maxDev, std::abs (magDb[(size_t) k]));
                }
                if (g_verbose) std::printf ("   mix0 dry flatness mode=%d %s maxDev=%.4f dB (tol %.2f)\n",
                                            (int) m, zl ? "ZL" : "HQ", maxDev, tolA);
                if (maxDev > tolA) fail ("G15 mix0 not flat (mode " + std::to_string ((int) m) + ") dev "
                                         + std::to_string (maxDev) + " > " + std::to_string (tolA));
            }
        }

        // (b) linear complementary law on a real tone (enhance on so dry != wet).
        {
            const int torder = 14, Tfft = 1 << torder;
            const int k0 = binOf (1000.0, fs, torder);
            const double f0 = freqOfBin (k0, fs, torder);
            const int settle = 2048, N = settle + Tfft;
            auto atMix = [&] (double mixPct, std::vector<double>& oL) -> void
            {
                ME eng; eng.prepare (fs, 256);
                for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
                eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
                setAllMode (eng, Mode::Tube);
                eng.setMix (mixPct); eng.setOutputDb (0.0);
                eng.setQuality (ME::Quality::HQ);
                warmup (eng, fs, 0.4);
                std::vector<double> inL ((size_t) N), inR, oR, dL, dR;
                for (int i = 0; i < N; ++i) inL[(size_t) i] = 0.25 * std::sin (2.0 * kPi * f0 * i / fs);
                inR = inL;
                runEngine (eng, inL, inR, oL, oR, dL, dR);
            };
            std::vector<double> o0, o50, o100;
            atMix (0.0, o0); atMix (50.0, o50); atMix (100.0, o100);
            double maxDev = 0.0, maxAbs = 0.0;
            for (int i = settle; i < N; ++i)
            {
                const double oracle = 0.5 * (o0[(size_t) i] + o100[(size_t) i]);
                maxDev = std::max (maxDev, std::abs (o50[(size_t) i] - oracle));
                maxAbs = std::max (maxAbs, std::abs (o50[(size_t) i]));
            }
            if (g_verbose) std::printf ("   linear-law: max|o50 - 0.5(o0+o100)|=%.2e (signal peak %.3f)\n", maxDev, maxAbs);
            if (maxDev > 1e-5) fail ("G15 mix 50%% not the linear midpoint (dev " + std::to_string (maxDev) + ")");
        }
    }

    // ==========================================================================
    // G16 — solo routing (superposition oracle)
    // ==========================================================================
    // Bands are parallel and the residual DC-blocker is LTI, so the soloed-band
    // output is exactly the sum of the single-band solos (settled segment), and
    // soloing ALL bands is bit-identical to no solo. Same for the delta bus. This
    // is a black-box superposition property, independent of the implementation.
    void g16_solo (double fs)
    {
        std::printf ("G16 solo routing @ %.0f\n", fs);
        const int N = (int) (0.6 * fs);
        const int seg0 = (int) (0.3 * fs); // compare after the states have settled

        // Broadband stereo input spanning several bands, with side content on the
        // 1.5 kHz component so the width path participates.
        std::vector<double> inL ((size_t) N), inR ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const double t = (double) i / fs;
            const double mid  = 0.30 * std::sin (2.0 * kPi * 180.0 * t) + 0.12 * std::sin (2.0 * kPi * 9000.0 * t);
            const double side = 0.20 * std::sin (2.0 * kPi * 1500.0 * t);
            inL[(size_t) i] = mid + side;
            inR[(size_t) i] = mid - side;
        }

        auto run = [&] (const std::array<bool, 5>& solo, std::vector<double>& oL, std::vector<double>& dL)
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0 + 20.0 * b); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            const Mode modes[5] = { Mode::Tube, Mode::Tape, Mode::Bright, Mode::Clean, Mode::Glue };
            for (int b = 0; b < 5; ++b) eng.setMode (b, modes[b]);
            eng.setMix (70.0); eng.setOutputDb (0.0);
            for (int b = 0; b < 5; ++b) eng.setSolo (b, solo[(size_t) b]);
            warmup (eng, fs, 0.4);
            std::vector<double> oR, dR;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
        };

        std::array<bool, 5> s1 { false, true, false, false, false };
        std::array<bool, 5> s3 { false, false, false, true, false };
        std::array<bool, 5> s13 { false, true, false, true, false };
        std::array<bool, 5> sAll { true, true, true, true, true };
        std::array<bool, 5> sNone { false, false, false, false, false };

        std::vector<double> o1, d1, o3, d3, o13, d13, oAll, dAll, oNone, dNone;
        run (s1, o1, d1); run (s3, o3, d3); run (s13, o13, d13);
        run (sAll, oAll, dAll); run (sNone, oNone, dNone);

        double outSup = 0.0, deltaSup = 0.0, allVsNone = 0.0, soloEnergy = 0.0;
        for (int i = seg0; i < N; ++i)
        {
            outSup    = std::max (outSup,    std::abs (o13[(size_t) i] - (o1[(size_t) i] + o3[(size_t) i])));
            deltaSup  = std::max (deltaSup,  std::abs (d13[(size_t) i] - (d1[(size_t) i] + d3[(size_t) i])));
            allVsNone = std::max (allVsNone, std::abs (oAll[(size_t) i] - oNone[(size_t) i]));
            soloEnergy += o1[(size_t) i] * o1[(size_t) i];
        }
        if (g_verbose)
            std::printf ("   out superpos=%.2e, delta superpos=%.2e, all==none=%.2e\n", outSup, deltaSup, allVsNone);
        if (outSup > 1e-5)    fail ("G16 output solo superposition broken (" + std::to_string (outSup) + ")");
        if (deltaSup > 1e-5)  fail ("G16 delta solo superposition broken (" + std::to_string (deltaSup) + ")");
        if (allVsNone > 1e-9) fail ("G16 solo-all != no-solo (" + std::to_string (allVsNone) + ")");
        if (soloEnergy <= 0.0) fail ("G16 soloed band produced silence");
    }

    // ==========================================================================
    // G17 — per-band mode independence
    // ==========================================================================
    // A 1 kHz tone sits in band 2 (700..2200); its 2nd harmonic (2 kHz) is
    // generated by band 2's shaper and lives in band 2. Changing a FAR band's mode
    // (band 4, HI > 7500) must leave that H2 unchanged (bands are independent),
    // while changing band 2's OWN mode must move it (routing is live).
    void g17_band_mode_independence (double fs)
    {
        std::printf ("G17 per-band mode independence @ %.0f\n", fs);
        const int order = 14, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const int k0 = binOf (1000.0, fs, order);   // band 2 (700..2200)

        auto measureH2 = [&] (const std::array<Mode, 5>& modes) -> double
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            for (int b = 0; b < 5; ++b) eng.setMode (b, modes[(size_t) b]);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            eng.setQuality (ME::Quality::HQ);
            warmup (eng, fs, 0.4);
            const int settle = 2048;
            const double f0 = freqOfBin (k0, fs, order);
            std::vector<double> inL ((size_t) (settle + Nfft)), inR, oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i) inL[(size_t) i] = 0.25 * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            return binAmp (oL, (size_t) settle, order, 2 * k0, fft);
        };

        const std::array<Mode, 5> allTube   { Mode::Tube, Mode::Tube, Mode::Tube,  Mode::Tube, Mode::Tube };
        const std::array<Mode, 5> farBright  { Mode::Tube, Mode::Tube, Mode::Tube,  Mode::Tube, Mode::Bright }; // band 4 changed
        const std::array<Mode, 5> sigBright  { Mode::Tube, Mode::Tube, Mode::Bright, Mode::Tube, Mode::Tube };  // band 2 changed

        const double base   = measureH2 (allTube);
        const double farChg = measureH2 (farBright);
        const double sigChg = measureH2 (sigBright);
        const double farDev = std::abs (linToDb (farChg) - linToDb (base));
        const double sigDev = std::abs (linToDb (sigChg) - linToDb (base));
        if (g_verbose)
            std::printf ("   base H2=%.2f dB, far-band change=%.4f dB, own-band change=%.2f dB\n",
                         linToDb (base), farDev, sigDev);
        if (farDev > 0.05) fail ("G17 far-band mode change moved band-2 H2 by " + std::to_string (farDev) + " dB");
        if (sigDev < 3.0)  fail ("G17 own-band mode change left band-2 H2 unmoved (" + std::to_string (sigDev) + " dB)");
    }

    // ==========================================================================
    // Linear-phase crossover helpers (issue #72)
    // ==========================================================================
    using LPX = factory_core::LinearPhaseCrossover5;

    // FFT order large enough to hold the full length-N impulse response plus margin.
    int firFftOrder (int taps, int maxBlock)
    {
        int order = 1;
        while ((1 << order) < taps + 2 * maxBlock + 16) ++order;
        return order;
    }

    // Push a unit impulse (at global sample 0) through the primitive on channel 0 and
    // collect the five band signals, each `L` samples long (block-driven, so the
    // stateful cross-block reconstruction is exercised).
    void firImpulseBands (LPX& xo, int L, int maxBlock, std::vector<std::vector<double>>& bands)
    {
        bands.assign (5, std::vector<double> ((size_t) L, 0.0));
        std::vector<float> in ((size_t) maxBlock);
        std::vector<float> b[5];
        for (auto& v : b) v.assign ((size_t) maxBlock, 0.0f);
        for (int s = 0; s < L; s += maxBlock)
        {
            const int n = std::min (maxBlock, L - s);
            for (int i = 0; i < n; ++i) in[(size_t) i] = (s + i == 0) ? 1.0f : 0.0f;
            xo.process (0, in.data(), n, b[0].data(), b[1].data(), b[2].data(), b[3].data(), b[4].data());
            for (int band = 0; band < 5; ++band)
                for (int i = 0; i < n; ++i) bands[(size_t) band][(size_t) (s + i)] = (double) b[(size_t) band][(size_t) i];
        }
    }

    // Rate-scaled FIR geometry the engine uses (host-rate primitive test path).
    struct FirGeom { int taps; int delay; };
    FirGeom firGeom (double fs)
    {
        const int D = ME::firDelayHostSamples (fs);
        return { 2 * D + 1, D };
    }

    // ==========================================================================
    // G18 — linear split reconstruction == pure delay (complementary by construction)
    // ==========================================================================
    // The five bands are differences of adjacent linear-phase low-passes plus a
    // D-sample delayed input, so their sum telescopes to a UNIT impulse at D — i.e.
    // sum(bands) is a pure delay, EXACTLY, regardless of the crossover frequencies.
    // Impulse -> sum -> FFT must be flat 0 dB at every bin (independent oracle: the
    // ideal magnitude of a pure delay is 1 at all frequencies), at every rate and at
    // extreme crossover settings (40 Hz .. 18 kHz — the redesign-under-drag worst case).
    void g18_linear_reconstruction (double fs)
    {
        std::printf ("G18 linear split reconstruction @ %.0f\n", fs);
        const FirGeom g = firGeom (fs);
        const int maxBlock = 192;                 // deliberately not a divisor of taps
        const int order = firFftOrder (g.taps, maxBlock);
        const int L = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        struct XSet { double f1, f2, f3, f4; const char* name; };
        const XSet sets[] = {
            { 130.0, 700.0, 2200.0, 7500.0, "default"    },
            { 40.0,  60.0,  90.0,   130.0,  "extreme-lo" }, // 40 Hz split, packed low
            { 2000.0, 5000.0, 10000.0, 18000.0, "extreme-hi" },
        };

        for (const auto& xs : sets)
        {
            LPX xo; xo.prepare (fs, maxBlock, g.taps, g.delay);
            xo.design (xs.f1, xs.f2, xs.f3, xs.f4);

            std::vector<std::vector<double>> bands;
            firImpulseBands (xo, L, maxBlock, bands);

            std::vector<double> sum ((size_t) L, 0.0);
            for (int i = 0; i < L; ++i)
                for (int b = 0; b < 5; ++b) sum[(size_t) i] += bands[(size_t) b][(size_t) i];

            // (a) the sum is a single unit spike at the group delay D.
            int peakIdx = 0; double peak = 0.0, off = 0.0;
            for (int i = 0; i < L; ++i)
            {
                const double a = std::abs (sum[(size_t) i]);
                if (a > peak) { peak = a; peakIdx = i; }
            }
            for (int i = 0; i < L; ++i) if (std::abs (i - g.delay) > 1) off = std::max (off, std::abs (sum[(size_t) i]));
            if (peakIdx != g.delay) fail ("G18 " + std::string (xs.name) + " peak at " + std::to_string (peakIdx) + " != D " + std::to_string (g.delay));
            if (std::abs (peak - 1.0) > 0.02) fail ("G18 " + std::string (xs.name) + " peak " + std::to_string (peak) + " != 1");
            if (off > 2.0e-3) fail ("G18 " + std::string (xs.name) + " off-delay leakage " + std::to_string (off) + " > 2e-3");

            // (b) magnitude spectrum of the sum is flat 0 dB (pure delay).
            std::vector<double> magDb; magSpectrum (sum, order, fft, magDb);
            double maxDev = 0.0;
            for (int k = 1; k < L / 2; ++k) maxDev = std::max (maxDev, std::abs (magDb[(size_t) k]));
            if (g_verbose) std::printf ("   [%s] taps=%d D=%d peak=%.6f@%d flat dev=%.4f dB\n",
                                        xs.name, g.taps, g.delay, peak, peakIdx, maxDev);
            if (maxDev > 0.02) fail ("G18 " + std::string (xs.name) + " band-sum not flat (dev " + std::to_string (maxDev) + " dB > 0.02)");
        }
    }

    // ==========================================================================
    // G19 — linear phase: tap symmetry AND measured group-delay flatness
    // ==========================================================================
    // Every band is symmetric about D (the low-passes are symmetric windowed sincs,
    // the delta band is a symmetric impulse minus a symmetric low-pass), so its
    // impulse response h[i] == h[N-1-i] (Type-I linear phase). Equivalently the band
    // response H(w) = A(w) e^{-jDw} with A real, so H(w) e^{+jDw} has ZERO imaginary
    // part — a direct, unwrap-free group-delay-flatness check (constant delay D).
    void g19_linear_phase (double fs)
    {
        std::printf ("G19 linear phase / group delay @ %.0f\n", fs);
        const FirGeom g = firGeom (fs);
        const int maxBlock = 192;
        const int order = firFftOrder (g.taps, maxBlock);
        const int L = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        LPX xo; xo.prepare (fs, maxBlock, g.taps, g.delay);
        xo.design (130.0, 700.0, 2200.0, 7500.0);
        std::vector<std::vector<double>> bands;
        firImpulseBands (xo, L, maxBlock, bands);

        // (a) tap symmetry of the LO low-pass (band 0) and HI band (band 4).
        for (int band : { 0, 4 })
        {
            double sym = 0.0;
            for (int i = 0; i < g.taps; ++i)
                sym = std::max (sym, std::abs (bands[(size_t) band][(size_t) i] - bands[(size_t) band][(size_t) (g.taps - 1 - i)]));
            if (g_verbose) std::printf ("   band %d tap asymmetry=%.2e\n", band, sym);
            if (sym > 1.0e-5) fail ("G19 band " + std::to_string (band) + " taps not symmetric (" + std::to_string (sym) + ")");
        }

        // (b) group-delay flatness: rotate H(k) by e^{+j 2pi k D / L}; imaginary part
        // must vanish (constant delay D across the whole band).
        std::vector<cd> H ((size_t) L);
        for (int i = 0; i < L; ++i) H[(size_t) i] = cd (bands[0][(size_t) i], 0.0);
        fft.forward (H.data());
        double maxImag = 0.0;
        for (int k = 0; k < L / 2; ++k)
        {
            const cd rot = H[(size_t) k] * std::polar (1.0, 2.0 * kPi * (double) k * (double) g.delay / (double) L);
            maxImag = std::max (maxImag, std::abs (rot.imag()));
        }
        if (g_verbose) std::printf ("   band 0 max |Im(H e^{jDw})|=%.2e (peak |H|=1)\n", maxImag);
        if (maxImag > 1.0e-3) fail ("G19 band 0 non-linear phase (residual imag " + std::to_string (maxImag) + " > 1e-3)");
    }

    // ==========================================================================
    // G20 — crossover -6 dB at fc + mastering-grade stopband
    // ==========================================================================
    // A DC-normalised windowed-sinc low-pass at cutoff fc has |H(fc)| ~ 0.5 (-6 dB),
    // so adjacent bands cross complementary. Independent oracle: the ideal half-band
    // crossover point is -6.02 dB. Also assert a deep stopband well above fc.
    void g20_crossover_points (double fs)
    {
        std::printf ("G20 crossover -6 dB / stopband @ %.0f\n", fs);
        const FirGeom g = firGeom (fs);
        const int maxBlock = 192;
        const int order = firFftOrder (g.taps, maxBlock);
        const int L = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        const double f1 = 130.0, f2 = 700.0, f3 = 2200.0, f4 = 7500.0;
        LPX xo; xo.prepare (fs, maxBlock, g.taps, g.delay);
        xo.design (f1, f2, f3, f4);
        std::vector<std::vector<double>> bands;
        firImpulseBands (xo, L, maxBlock, bands);
        (void) fft;

        // Evaluate the band's magnitude response at EXACTLY f (direct DTFT of the
        // impulse response) — the crossover edge is steep, so snapping to the nearest
        // FFT bin adds several dB of quantisation error where f does not divide the
        // bin grid (e.g. 7.5 kHz at 88.2 kHz). This is a more accurate measurement of
        // the same -6 dB spec, not a looser one.
        auto magAt = [&] (int band, double f) -> double
        {
            const double w = 2.0 * kPi * f / fs;
            double re = 0.0, im = 0.0;
            for (int n = 0; n < L; ++n)
            {
                const double h = bands[(size_t) band][(size_t) n];
                re += h * std::cos (w * (double) n);
                im -= h * std::sin (w * (double) n);
            }
            return std::sqrt (re * re + im * im);
        };

        // LO low-pass (band 0) is -6 dB at f1, HI band (band 4) is -6 dB at f4.
        const double lo6  = linToDb (magAt (0, f1));
        const double hi6  = linToDb (magAt (4, f4));
        if (g_verbose) std::printf ("   |LO(f1)|=%.2f dB  |HI(f4)|=%.2f dB (target -6.02)\n", lo6, hi6);
        if (std::abs (lo6 - (-6.02)) > 2.0) fail ("G20 LO band not -6 dB at f1 (" + std::to_string (lo6) + ")");
        if (std::abs (hi6 - (-6.02)) > 2.0) fail ("G20 HI band not -6 dB at f4 (" + std::to_string (hi6) + ")");

        // Stopband: the LO low-pass is deep well above f1 (5x), the HI band deep well
        // below f4 (f4/5) — mastering-grade rejection past the transition.
        const double loStop = linToDb (magAt (0, 5.0 * f1));
        const double hiStop = linToDb (magAt (4, f4 / 5.0));
        if (g_verbose) std::printf ("   LO stop@5f1=%.1f dB  HI stop@f4/5=%.1f dB\n", loStop, hiStop);
        if (loStop > -55.0) fail ("G20 LO stopband weak (" + std::to_string (loStop) + " dB > -55)");
        if (hiStop > -55.0) fail ("G20 HI stopband weak (" + std::to_string (hiStop) + " dB > -55)");
    }

    // ==========================================================================
    // G21 — reported latency == FIR group delay + OS latency; taps follow the rate
    // ==========================================================================
    void g21_linear_latency (double fs)
    {
        std::printf ("G21 linear-phase latency @ %.0f\n", fs);
        const int Dhost = ME::firDelayHostSamples (fs);

        // (i) HQ + Linear reports OS latency (51) + the FIR group delay.
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Linear);
            tick (eng);
            const int rep = eng.latencySamples();
            const int exp = expectedLatency (fs) + Dhost;   // 51 + D at every supported rate
            if (g_verbose) std::printf ("   HQ+Linear latency=%d expect=%d (D=%d)\n", rep, exp, Dhost);
            if (rep != exp) fail ("G21 HQ+Linear latency " + std::to_string (rep) + " != " + std::to_string (exp));
        }
        // (ii) Standard phase is unchanged (no FIR delay).
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Standard);
            tick (eng);
            if (eng.latencySamples() != expectedLatency (fs))
                fail ("G21 HQ+Standard latency changed (" + std::to_string (eng.latencySamples()) + ")");
        }
        // (iii) Zero-Latency ignores the phase option (no oversampling bracket).
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::ZeroLatency); eng.setPhaseMode (ME::Phase::Linear);
            tick (eng);
            if (eng.latencySamples() != 0) fail ("G21 ZL+Linear latency != 0 (" + std::to_string (eng.latencySamples()) + ")");
        }
        // (iv) Resolution follows the sample rate: the FIR group delay is a fixed
        // ~43 ms of TIME at every rate (so the tap count scales with fs, never fixed).
        const double sec = (double) Dhost / fs;
        if (g_verbose) std::printf ("   FIR group delay=%.2f ms (taps=%d)\n", 1000.0 * sec, 2 * Dhost + 1);
        if (sec < 0.040 || sec > 0.045) fail ("G21 FIR delay " + std::to_string (1000.0 * sec) + " ms out of [40,45]");
    }

    // ==========================================================================
    // G22 — engine linear mode: flatness, worst-case finiteness, redesign-on-drag
    // ==========================================================================
    void g22_linear_engine (double fs)
    {
        std::printf ("G22 engine linear mode @ %.0f\n", fs);
        const int order = 15, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const int Dhost = ME::firDelayHostSamples (fs);

        // (a) enh 0, every mix -> the enhanced bus's linear reconstruction is the pure
        // (delayed) band sum == the direct bus, so the constant-voltage blend is FLAT
        // 0 dB at every mix (the linear-mode analogue of G1). Measure through the full
        // HQ oversampling bracket (band ~20 Hz .. 0.42 fs, matching G1's HQ window).
        for (double mixPct : { 0.0, 50.0, 100.0 })
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Linear);
            eng.setMix (mixPct);
            warmup (eng, fs, 0.4);
            std::vector<double> in ((size_t) (Nfft + Dhost), 0.0), inR, oL, oR, dL, dR;
            in[0] = 1.0; inR = in;
            runEngine (eng, in, inR, oL, oR, dL, dR);
            // Align out to the reported latency so the impulse sits at the window start.
            std::vector<double> seg (oL.begin() + Dhost, oL.begin() + Dhost + Nfft);
            std::vector<double> magDb; magSpectrum (seg, order, fft, magDb);
            double maxDev = 0.0;
            for (int k = 1; k < Nfft / 2; ++k)
            {
                const double f = freqOfBin (k, fs, order);
                if (f < 20.0 || f > 0.42 * fs) continue;
                maxDev = std::max (maxDev, std::abs (magDb[(size_t) k]));
            }
            if (g_verbose) std::printf ("   enh0 mix%.0f linear flatness dev=%.4f dB\n", mixPct, maxDev);
            if (maxDev > 0.25) fail ("G22 linear enh0 mix" + std::to_string ((int) mixPct) + " not flat (" + std::to_string (maxDev) + " dB)");
        }

        // (b) worst case (enh 150 %, width 200 %, Glue, mix 100 %) in linear mode:
        // output stays finite with a realistic peak, and the impulse response energy
        // is non-increasing (the FIR + shaper path is strictly feed-forward).
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 200.0); }
            setAllMode (eng, Mode::Glue);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Linear);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs, 0.4);
            const int N = (int) (2.0 * fs);
            const int period = std::max (2, (int) (fs / 55.0));
            std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
            for (int i = 0; i < N; ++i) inL[(size_t) i] = ((i % period) < period / 2) ? 1.0 : -1.0;
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            if (! factory_core::testing::allFinite (oL)) fail ("G22 worst-case output not finite");
            const double pk = factory_core::testing::peakAbs (oL);
            if (g_verbose) std::printf ("   worst-case peak=%.2f dBFS\n", linToDb (pk));
            if (pk > dbToLin (12.0)) fail ("G22 worst-case peak " + std::to_string (linToDb (pk)) + " dBFS > +12");
        }
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 150.0); eng.setWidth (b, 200.0); }
            setAllMode (eng, Mode::Glue);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Linear);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            warmup (eng, fs);
            auto proc = [&] (double x) -> double
            {
                float l = (float) x, r = (float) x, dl = 0.0f, dr = 0.0f;
                eng.processBlock (&l, &r, 1, &dl, &dr);
                return (double) l;
            };
            if (! factory_core::testing::impulseResponseNonIncreasing (proc, fs))
                fail ("G22 linear impulse response energy increasing (feedback?)");
        }

        // (c) redesign-on-drag to extreme crossover settings stays finite + flat at
        // enh 0 (the reconstruction is a pure delay for ANY crossover frequencies).
        for (auto xs : { std::array<double,4>{ 40.0, 60.0, 90.0, 130.0 },
                         std::array<double,4>{ 2000.0, 5000.0, 10000.0, 18000.0 } })
        {
            ME eng; eng.prepare (fs, 256); configFlat (eng);
            eng.setQuality (ME::Quality::HQ); eng.setPhaseMode (ME::Phase::Linear);
            eng.setMix (100.0);
            eng.setCrossovers (xs[0], xs[1], xs[2], xs[3]);
            eng.redesignFir (xs[0], xs[1], xs[2], xs[3]); // stands in for the message-thread redesign
            warmup (eng, fs, 0.4);
            std::vector<double> in ((size_t) (Nfft + Dhost), 0.0), inR, oL, oR, dL, dR;
            in[0] = 1.0; inR = in;
            runEngine (eng, in, inR, oL, oR, dL, dR);
            if (! factory_core::testing::allFinite (oL)) fail ("G22 redesign extreme not finite");
            std::vector<double> seg (oL.begin() + Dhost, oL.begin() + Dhost + Nfft);
            std::vector<double> magDb; magSpectrum (seg, order, fft, magDb);
            double maxDev = 0.0;
            for (int k = 1; k < Nfft / 2; ++k)
            {
                const double f = freqOfBin (k, fs, order);
                if (f < 20.0 || f > 0.42 * fs) continue;
                maxDev = std::max (maxDev, std::abs (magDb[(size_t) k]));
            }
            if (g_verbose) std::printf ("   redesign [%.0f..%.0f] flat dev=%.4f dB\n", xs[0], xs[3], maxDev);
            if (maxDev > 0.25) fail ("G22 redesign extreme not flat (" + std::to_string (maxDev) + " dB)");
        }
    }

    // ==========================================================================
    // G23 — output gain law (dB oracle) + clickless mid-stream gain change
    // ==========================================================================
    // setOutputDb() drives gOutputSm (host-rate one-pole, 30 ms -- the DOCUMENTED
    // Smoother::setRate rate MultibandEnhancer.h uses for gOutputSm) which
    // multiplies the FINAL output once, post-crossfade/post-delta-listen (line
    // "Output gain + delta-listen (once, post-crossfade)"). At enh 0 / width 100
    // the direct bus equals the wet bus (residual is silent, M/S transform is a
    // no-op at width 1), so ANY mix reduces to out = direct * gOutput -- the ONLY
    // effect a dB change can have is the multiplicative gain step. Comparing two
    // outputDb settings RELATIVE to a 0 dB baseline (rather than to an assumed-
    // unity direct-bus gain) isolates that step from the crossover/allpass's own
    // (small, G1-gated) passband ripple -- an oracle that never reads the
    // implementation's gain value, only the dB-to-linear spec (10^(db/20)).
    void g23_output_gain (double fs)
    {
        std::printf ("G23 output gain @ %.0f\n", fs);
        const int order = 14, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);
        const int k0 = binOf (1000.0, fs, order);
        const double f0 = freqOfBin (k0, fs, order);
        const double A = 0.25;

        // (a) dB oracle: dB(measured) - dB(measured @ 0 dB) == the requested dB,
        // exactly, at -6 / +6 dB.
        auto measure = [&] (double outDb) -> double
        {
            ME eng; eng.prepare (fs, 256);
            configFlat (eng);            // enh 0, width 100 -> direct == wet
            eng.setMix (60.0);           // any mix works at enh 0; prove it (not 0/50/100)
            eng.setOutputDb (outDb);
            warmup (eng, fs, 0.4);       // >> 30 ms gOutput smoother settle
            const int settle = 2048;
            std::vector<double> inL ((size_t) (settle + Nfft)), inR, oL, oR, dL, dR;
            for (int i = 0; i < settle + Nfft; ++i) inL[(size_t) i] = A * std::sin (2.0 * kPi * f0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);
            return binAmp (oL, (size_t) settle, order, k0, fft);
        };
        const double m0 = measure (0.0);
        for (double db : { -6.0, 6.0 })
        {
            const double m = measure (db);
            const double dev = (linToDb (m) - linToDb (m0)) - db;
            if (g_verbose) std::printf ("   outputDb=%.0f rel=%.4f dB expect=%.1f dev=%.4f\n",
                                        db, linToDb (m) - linToDb (m0), db, dev);
            if (std::abs (dev) > 0.05) fail ("G23 outputDb " + std::to_string (db) + " dev " + std::to_string (dev) + " dB > 0.05");
        }

        // (b) mid-stream gain change: finite, and the click stays within an
        // independent slew bound built from the DOCUMENTED 30 ms one-pole rate
        // plus the steady-state per-sample step at the larger of the two gains --
        // never derived by reading the engine's actual output.
        {
            const double ft = 300.0;      // low tone: even the steady-state step is small
            const double fromDb = -6.0, toDb = 6.0;
            ME eng; eng.prepare (fs, 256);
            configFlat (eng);
            eng.setMix (100.0);
            eng.setOutputDb (fromDb);
            warmup (eng, fs, 0.4);

            const double coeff = 1.0 - std::exp (-1.0 / std::max (1.0, 30.0 * 0.001 * fs)); // documented rate
            const double gFrom = dbToLin (fromDb), gTo = dbToLin (toDb);
            const double maxGain = std::max (gFrom, gTo);
            const double steadyStep    = maxGain * A * 2.0 * std::sin (kPi * ft / fs);
            const double gainSlewStep  = A * coeff * std::abs (gTo - gFrom);
            const double kSafety = 2.0;
            const double bound = kSafety * (steadyStep + gainSlewStep);

            const int N = (int) (1.0 * fs);
            const int switchAt = N / 2;
            std::vector<float> bL (256), bR (256), bdL (256), bdR (256);
            double worstStep = 0.0, prev = 0.0; bool have = false, finite = true, switched = false;
            for (int s = 0; s < N; s += 256)
            {
                const int n = std::min (256, N - s);
                for (int i = 0; i < n; ++i)
                {
                    const double v = A * std::sin (2.0 * kPi * ft * (double) (s + i) / fs);
                    bL[(size_t) i] = (float) v; bR[(size_t) i] = (float) v;
                }
                if (! switched && s >= switchAt) { eng.setOutputDb (toDb); switched = true; }
                eng.processBlock (bL.data(), bR.data(), n, bdL.data(), bdR.data());
                for (int i = 0; i < n; ++i)
                {
                    const double y = bL[(size_t) i];
                    if (! std::isfinite (y)) finite = false;
                    if (have) worstStep = std::max (worstStep, std::abs (y - prev));
                    prev = y; have = true;
                }
            }
            if (g_verbose) std::printf ("   mid-stream gain change worst step=%.5f (bound %.5f)\n", worstStep, bound);
            if (! finite) fail ("G23 non-finite output during gain change");
            if (worstStep > bound) fail ("G23 gain-change step " + std::to_string (worstStep) + " > bound " + std::to_string (bound));
        }
    }

    // ==========================================================================
    // G24 — delta-listen routes the residual bus to the MAIN output
    // ==========================================================================
    // MultibandEnhancer.h: "out = (deltaListen ? deltaHost : out) * gOutput" in the
    // main processBlock's final loop. With gOutput == 1 (0 dB) the main L/R output
    // must equal the delta bus (deltaL/deltaR) bit-for-bit; with enhance 0 (residual
    // identically zero for |x| <= 1 -- HarmonicShaper::f(u) == fCore(u) == u in the
    // linear region at enh 0) the main output under delta-listen must be silence.
    void g24_delta_listen_routing (double fs)
    {
        std::printf ("G24 delta-listen routing @ %.0f\n", fs);

        // (a) main == delta bus (post gOutput, here 0 dB) under deltaListen.
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            setAllMode (eng, Mode::Bright);
            eng.setMix (100.0); eng.setOutputDb (0.0);
            eng.setDeltaListen (true);
            warmup (eng, fs, 0.4);

            const int N = (int) (0.5 * fs);
            std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
            for (int i = 0; i < N; ++i)
                inL[(size_t) i] = 0.30 * std::sin (2.0 * kPi * 1000.0 * i / fs) + 0.15 * std::sin (2.0 * kPi * 3000.0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);

            double maxDiffL = 0.0, maxDiffR = 0.0;
            const size_t settle = (size_t) (0.2 * fs);
            for (size_t i = settle; i < oL.size(); ++i)
            {
                maxDiffL = std::max (maxDiffL, std::abs (oL[i] - dL[i]));
                maxDiffR = std::max (maxDiffR, std::abs (oR[i] - dR[i]));
            }
            if (g_verbose) std::printf ("   deltaListen main-vs-delta maxDiff L=%.2e R=%.2e\n", maxDiffL, maxDiffR);
            if (maxDiffL > 1.0e-6) fail ("G24 deltaListen main L != delta bus (" + std::to_string (maxDiffL) + ")");
            if (maxDiffR > 1.0e-6) fail ("G24 deltaListen main R != delta bus (" + std::to_string (maxDiffR) + ")");
        }

        // (b) enhance 0 + deltaListen -> main output is silence (no residual to
        // listen to; the linear reconstruction carries no harmonics).
        {
            ME eng; eng.prepare (fs, 256);
            configFlat (eng);           // enh 0, width 100
            eng.setMix (100.0);
            eng.setDeltaListen (true);
            warmup (eng, fs, 0.4);

            const int N = (int) (0.5 * fs);
            std::vector<double> inL ((size_t) N), inR, oL, oR, dL, dR;
            for (int i = 0; i < N; ++i) inL[(size_t) i] = 0.30 * std::sin (2.0 * kPi * 1000.0 * i / fs);
            inR = inL;
            runEngine (eng, inL, inR, oL, oR, dL, dR);

            if (! factory_core::testing::allFinite (oL)) fail ("G24 enh0 deltaListen output not finite");
            const double pk = factory_core::testing::peakAbs (oL);
            const double pkDb = linToDb (pk);
            if (g_verbose) std::printf ("   enh0 deltaListen main peak=%.1f dBFS\n", pkDb);
            if (pkDb > -90.0) fail ("G24 enh0 deltaListen main not silent (" + std::to_string (pkDb) + " dBFS)");
        }
    }

    // ==========================================================================
    // G25 — standard-phase (IIR) flatness at the extreme-hi crossover set
    // ==========================================================================
    // G1 only exercises the IIR crossover at 'default' and 'packed-lo'; the
    // extreme-hi set (2k/5k/10k/18k -- the same redesign-under-drag worst case
    // G18/G22 use for the LINEAR-phase FIR path) was never gated on the STANDARD
    // (IIR/allpass-compensated) crossover. Same construction and tolerances as G1
    // (constant-voltage + enh 0 -> flat 0 dB at every mix): do NOT loosen if it fails.
    void g25_standard_extreme_hi (double fs)
    {
        std::printf ("G25 standard-phase extreme-hi flatness @ %.0f\n", fs);
        const int order = 15, Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        const double f1 = 2000.0, f2 = 5000.0, f3 = 10000.0, f4 = 18000.0;

        for (auto quality : { ME::Quality::HQ, ME::Quality::ZeroLatency })
        {
            const bool zl = (quality == ME::Quality::ZeroLatency);
            const double loHz = zl ? 10.0 : 20.0;
            const double hiFrac = zl ? 0.49 : 0.42;
            const double tolA = zl ? 0.10 : 0.25;

            for (double mixPct : { 0.0, 50.0, 100.0 })
            {
                ME eng; eng.prepare (fs, 256);
                configFlat (eng);
                eng.setQuality (quality);
                eng.setPhaseMode (ME::Phase::Standard);   // IIR crossover explicitly (also the default)
                eng.setCrossovers (f1, f2, f3, f4);
                eng.setMix (mixPct);
                warmup (eng, fs);

                std::vector<double> in ((size_t) Nfft, 0.0), inR, oL, oR, dL, dR;
                in[0] = 1.0; inR = in;
                runEngine (eng, in, inR, oL, oR, dL, dR);

                std::vector<double> magDb;
                magSpectrum (oL, order, fft, magDb);
                const double target = 0.0; // constant-voltage + enh 0 => flat 0 dB at every mix
                double maxDev = 0.0; double atWorst = 0.0;
                for (int k = 1; k < Nfft / 2; ++k)
                {
                    const double f = freqOfBin (k, fs, order);
                    if (f < loHz || f > hiFrac * fs) continue;
                    const double dev = std::abs (magDb[(size_t) k] - target);
                    if (dev > maxDev) { maxDev = dev; atWorst = f; }
                }
                if (g_verbose)
                    std::printf ("   [%s extreme-hi mix%.0f] maxDev=%.4f dB @ %.0f Hz (tol %.2f)\n",
                                 zl ? "ZL" : "HQ", mixPct, maxDev, atWorst, tolA);
                if (maxDev > tolA)
                    fail ("G25 " + std::string (zl ? "ZL " : "HQ ") + "extreme-hi mix" + std::to_string ((int) mixPct)
                          + " dev " + std::to_string (maxDev) + " dB > " + std::to_string (tolA));
            }
        }
    }
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); // unbuffered so a crash still shows progress
    for (int i = 1; i < argc; ++i) if (std::string (argv[i]) == "-v") g_verbose = true;

    const auto rates = factory_core::testing::sampleRatesFromArgs (argc, argv);
    for (double fs : rates)
    {
        std::printf ("===== Fs = %.1f =====\n", fs);
        g1_flatness (fs);
        g2_latency (fs);
        g3_harmonics (fs);
        g4_alias (fs);
        g5_dc (fs);
        g6_width (fs);
        g7_glue (fs);
        g8_finite (fs);
        g9_reset (fs);
        g10_feedback (fs);
        g11_resolution (fs);
        g12_clamp (fs);
        g13_click (fs);
        g14_quality_click (fs);
        g15_mix_law (fs);
        g16_solo (fs);
        g17_band_mode_independence (fs);
        g18_linear_reconstruction (fs);
        g19_linear_phase (fs);
        g20_crossover_points (fs);
        g21_linear_latency (fs);
        g22_linear_engine (fs);
        g23_output_gain (fs);
        g24_delta_listen_routing (fs);
        g25_standard_extreme_hi (fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
