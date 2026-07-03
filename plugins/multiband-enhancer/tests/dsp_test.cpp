//
// dsp_test.cpp — headless verification of the Tatsumin Enhancer DSP core
// (factory_core::MultibandEnhancer and its new primitives). Links factory_core
// only; runs the full sample-rate matrix. Every quantitative oracle is an
// INDEPENDENT code path (closed-form Fourier series / fixed spec constants),
// never derived from the implementation under test.
//
// Gate map (see plugins/multiband-enhancer/plan §5, docs/regression-policy.md):
//   G1  phase coherence / flatness (defect #1)      G8  finiteness + worst-case hold (Class C)
//   G2  latency exact                               G9  state reset (Class E)
//   G3  harmonic oracle, all modes x both quality   G10 no feedback (Class A)
//   G4  aliasing                                    G11 resolution-follows-rate (Class G)
//   G5  DC removal                                  G12 crossover order clamp
//   G6  width oracle                                G13 clickless mode transition
//   G7  Glue levelling
//
#include "factory_core/MultibandEnhancer.h"
#include "factory_core/Oversampler.h"
#include "factory_core/HarmonicShaper.h"
#include "factory_core/FFT.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
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

    // ---- configuration helper -------------------------------------------------
    void configFlat (ME& eng)  // neutral, enhance 0
    {
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 0.0); eng.setWidth (b, 100.0); }
        eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
        eng.setMode (Mode::Tube);
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
                // (a) direct 0 dB, wet muted -> flat 0 dB
                // (b) direct 0 dB, wet 0 dB   -> flat +6.02 dB (comb shrink would fail)
                for (int mode = 0; mode < 2; ++mode)
                {
                    ME eng; eng.prepare (fs, 256);
                    configFlat (eng);
                    eng.setQuality (quality);
                    eng.setCrossovers (xs.f1, xs.f2, xs.f3, xs.f4);
                    eng.setDirectDb (0.0);
                    eng.setWetDb (mode == 0 ? -60.0 : 0.0);
                    warmup (eng, fs);

                    std::vector<double> in ((size_t) Nfft, 0.0), inR, oL, oR, dL, dR;
                    in[0] = 1.0; inR = in;
                    runEngine (eng, in, inR, oL, oR, dL, dR);

                    std::vector<double> magDb;
                    magSpectrum (oL, order, fft, magDb);
                    const double target = (mode == 0) ? 0.0 : 6.0205999; // 20log10(2)
                    double maxDev = 0.0; double atWorst = 0.0;
                    for (int k = 1; k < Nfft / 2; ++k)
                    {
                        const double f = freqOfBin (k, fs, order);
                        if (f < loHz || f > hiFrac * fs) continue;
                        const double dev = std::abs (magDb[(size_t) k] - target);
                        if (dev > maxDev) { maxDev = dev; atWorst = f; }
                    }
                    if (g_verbose)
                        std::printf ("   [%s %s %s] maxDev=%.4f dB @ %.0f Hz (tol %.2f)\n",
                                     zl ? "ZL" : "HQ", xs.name, mode == 0 ? "directOnly" : "direct+wet",
                                     maxDev, atWorst, tolA);
                    if (maxDev > tolA)
                        fail ("G1 " + std::string (zl ? "ZL " : "HQ ") + xs.name + (mode == 0 ? " directOnly" : " direct+wet")
                              + " dev " + std::to_string (maxDev) + " dB > " + std::to_string (tolA));
                }
            }
        }
    }

    // ==========================================================================
    // G2 — latency exact
    // ==========================================================================
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
    // independently re-derived here (see plan §5).
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
    // G4 — aliasing
    // ==========================================================================
    void g4_alias (double fs)
    {
        std::printf ("G4 aliasing @ %.0f\n", fs);
        const int order = 15;
        const int Nfft = 1 << order;
        factory_core::FFT fft; fft.prepare (order);

        // At >=176.4 kHz HQ runs at native rate (M=1) with ADAA only — i.e. it IS
        // the Zero-Latency path — so its oversampling-regime gates (-78 / -60) do
        // not apply; gate it at the ADAA level (-45) and skip the a=1.0 abuse case,
        // which exists to stress the oversampler (absent at M=1).
        const int M = ME::hqFactor (fs);
        const bool osRegime = (M >= 2);
        struct Case { ME::Quality q; double a; double ffrac; double tol; const char* name; bool run; };
        const Case cases[] = {
            { ME::Quality::HQ,          0.5, 0.30, osRegime ? -78.0 : -45.0, "HQ",       true },
            { ME::Quality::ZeroLatency, 0.5, 0.15, -45.0,                     "ZL",       true },
            { ME::Quality::HQ,          1.0, 0.30, -60.0,                     "HQ-abuse", osRegime },
        };
        for (const auto& c : cases)
        {
            if (! c.run) continue;
            const int k0 = binOf (c.ffrac * fs, fs, order);
            const double f0 = freqOfBin (k0, fs, order);
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
            eng.setCrossovers (130.0, 700.0, 2200.0, 7500.0);
            eng.setMode (Mode::Bright);
            eng.setQuality (c.q);
            eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
    // G5 — DC removal
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
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
            eng.setMode (m);
            eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
    // G6 — width oracle (M/S)
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
        // overlap). Uniform width => wet = w * sum(b_i) = w * S, so the S ratio is
        // exactly w and M is untouched. Enhance 0, direct muted, wet 0 dB isolates
        // the linear width path.
        auto measS = [&] (double widthPct, bool sideSignal, int soloBand = -1) -> double
        {
            ME eng; eng.prepare (fs, 256);
            configFlat (eng);
            for (int j = 0; j < 5; ++j) eng.setWidth (j, soloBand < 0 ? widthPct : 100.0);
            if (soloBand >= 0) eng.setWidth (soloBand, widthPct);
            eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
                eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
    // G7 — Glue levelling
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
            eng.setMode (mode);
            eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
            eng.setMode (Mode::Glue); eng.setDirectDb (-60.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
            eng.setMode (Mode::Glue); eng.setDirectDb (-60.0); eng.setWetDb (0.0);
            const int N = (int) (2.0 * fs);
            std::vector<double> z ((size_t) N, 0.0), oL, oR, dL, dR;
            runEngine (eng, z, z, oL, oR, dL, dR);
            double e = 0.0; for (double v : dL) e += v * v;
            if (e != 0.0) fail ("G7 silence produced nonzero delta energy " + std::to_string (e));
        }
    }

    // ==========================================================================
    // G8 — finiteness + worst-case hold + NaN self-heal (Class C)
    // ==========================================================================
    void g8_finite (double fs)
    {
        std::printf ("G8 finiteness / NaN self-heal @ %.0f\n", fs);
        auto worst = [&] (bool injectNaN, std::vector<double>& out)
        {
            ME eng; eng.prepare (fs, 256);
            for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 200.0); }
            eng.setMode (Mode::Glue);
            eng.setDirectDb (0.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
                eng.setMode (Mode::Tape); eng.setQuality (q);
                eng.setDirectDb (0.0); eng.setWetDb (0.0);
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
    // ==========================================================================
    void g10_feedback (double fs)
    {
        std::printf ("G10 feedforward-only @ %.0f\n", fs);
        // Worst case: max enhance + width, Glue, both buses hot. One-sample-in ->
        // one-sample-out closure over the engine (mono).
        ME eng; eng.prepare (fs, 256);
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 200.0); }
        eng.setMode (Mode::Glue); eng.setDirectDb (0.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
    // G13 — clickless mode transition
    // ==========================================================================
    void g13_click (double fs)
    {
        std::printf ("G13 clickless mode switch @ %.0f\n", fs);
        const int k0order = 14;
        const int k0 = binOf (1000.0, fs, k0order);
        const double f0 = freqOfBin (k0, fs, k0order);
        ME eng; eng.prepare (fs, 256);
        for (int b = 0; b < 5; ++b) { eng.setEnhance (b, 100.0); eng.setWidth (b, 100.0); }
        eng.setMode (Mode::Tube); eng.setDirectDb (0.0); eng.setWetDb (0.0); eng.setOutputDb (0.0);
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
            if (! switched && s > N / 3) { eng.setMode (Mode::Bright); switched = true; }
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
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
