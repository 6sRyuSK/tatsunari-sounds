//
// dsp_test.cpp — headless verification of the resonance suppressor DSP core
// (factory_core::FFT + ResonanceSuppressor). This is a non-linear / adaptive
// processor, so the gates are invariants rather than a single closed-form oracle:
//
//   1. FFT round-trips to identity and matches a direct DFT.
//   2. STFT perfect reconstruction: depth=0 => output == input delayed by the
//      reported latency N (proves windowing / overlap-add / latency).
//   3. Delta: wet (mix=1) + removed (delta) == dry.
//   4. Suppression + selectivity: a strong steady tone over noise is reduced,
//      while a resonance-free control band is left alone.
//   5. Reduction profile: zeroing the profile around a resonance disables
//      suppression there (the "EQ-like" curve scales suppression locally).
//   6. Stereo link: identical L==R input yields identical L==R output.
//
#include "factory_core/FFT.h"
#include "factory_core/ResonanceSuppressor.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    // Magnitude of a real signal at frequency f (single-bin DFT over [a,b)).
    double magAt (const std::vector<double>& x, int a, int b, double f, double Fs)
    {
        cd acc {};
        const double w = 2.0 * kPi * f / Fs;
        for (int n = a; n < b; ++n) acc += x[(size_t) n] * std::exp (cd (0.0, -w * (n - a)));
        return std::abs (acc) / (b - a);
    }

    void fftTest()
    {
        std::printf ("FFT\n");
        std::mt19937 rng (1);
        std::uniform_real_distribution<double> u (-1.0, 1.0);

        for (int order : { 6, 8, 10 })
        {
            factory_core::FFT f; f.prepare (order);
            const int n = f.size();
            std::vector<cd> a ((size_t) n), b;
            for (auto& v : a) v = cd (u (rng), u (rng));
            b = a;
            f.forward (b.data());
            f.inverse (b.data());
            double e = 0.0;
            for (int i = 0; i < n; ++i) e = std::max (e, std::abs (b[(size_t) i] - a[(size_t) i]));
            if (e > 1.0e-9) fail ("FFT round-trip order " + std::to_string (order) + " err " + std::to_string (e));
        }

        // Forward vs direct DFT.
        factory_core::FFT f; f.prepare (6);
        const int n = f.size();
        std::vector<cd> a ((size_t) n), A;
        for (auto& v : a) v = cd (u (rng), u (rng));
        A = a; f.forward (A.data());
        double e = 0.0;
        for (int k = 0; k < n; ++k)
        {
            cd s {};
            for (int m = 0; m < n; ++m) s += a[(size_t) m] * std::exp (cd (0.0, -2.0 * kPi * k * m / n));
            e = std::max (e, std::abs (A[(size_t) k] - s));
        }
        if (e > 1.0e-9) fail ("FFT vs DFT err " + std::to_string (e));
        std::printf ("  ok\n");
    }

    // Render a stereo signal through a configured suppressor; return mono output.
    template <typename Cfg>
    std::vector<double> render (double Fs, const std::vector<double>& xl, const std::vector<double>& xr, Cfg cfg)
    {
        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, 11);
        cfg (s);
        std::vector<double> out (xl.size());
        for (size_t n = 0; n < xl.size(); ++n)
        {
            double l = xl[n], r = xr[n];
            s.process (l, r);
            out[n] = 0.5 * (l + r);
        }
        return out;
    }

    void reconstructionTest (double Fs)
    {
        std::printf ("STFT reconstruction + latency @ Fs=%.0f\n", Fs);
        const int N = 2048;     // window = latency for order 11
        const int M = 1 << 14;
        std::mt19937 rng (7);
        std::uniform_real_distribution<double> u (-0.5, 0.5);
        std::vector<double> x ((size_t) M);
        for (auto& v : x) v = u (rng);

        const auto y = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (0.0); s.setMix (1.0);
        });

        factory_core::ResonanceSuppressor probe; probe.prepare (Fs, 11);
        if (probe.latencySamples() != N) fail ("latency != N");

        double e = 0.0;
        for (int n = 2 * N; n < M; ++n) e = std::max (e, std::abs (y[(size_t) n] - x[(size_t) (n - N)]));
        if (e > 1.0e-6) fail ("reconstruction (depth=0) err " + std::to_string (e));
        std::printf ("  maxErr=%.2e\n", e);
    }

    void deltaTest (double Fs)
    {
        std::printf ("Delta (wet + removed == dry) @ Fs=%.0f\n", Fs);
        const int N = 2048, M = 1 << 14;
        std::mt19937 rng (11);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 1500.0 * n / Fs);

        const auto wet = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.0); s.setMix (1.0);
        });
        const auto rem = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.0); s.setDelta (true);
        });
        double e = 0.0;
        for (int n = 2 * N; n < M; ++n) e = std::max (e, std::abs (wet[(size_t) n] + rem[(size_t) n] - x[(size_t) (n - N)]));
        if (e > 1.0e-6) fail ("wet+removed != dry err " + std::to_string (e));
        std::printf ("  maxErr=%.2e\n", e);
    }

    void suppressionTest (double Fs)
    {
        std::printf ("Suppression + selectivity @ Fs=%.0f\n", Fs);
        const int M = 1 << 15;
        const double f0 = 2000.0, fc = 7000.0;
        std::mt19937 rng (5);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        const auto wet = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.2); s.setSharpness (0.5);
        });

        const int a = M / 2, b = M; // steady-state window
        const double dryF0 = magAt (dry, a, b, f0, Fs), wetF0 = magAt (wet, a, b, f0, Fs);
        const double dryFc = magAt (dry, a, b, fc, Fs), wetFc = magAt (wet, a, b, fc, Fs);

        if (wetF0 > dryF0 * 0.6) fail ("resonance at f0 not suppressed (>=4.4 dB) "
                                       + std::to_string (20.0 * std::log10 (wetF0 / dryF0)) + " dB");
        if (wetFc < dryFc * 0.7) fail ("control band over-attenuated (not selective)");
        std::printf ("  f0 %.1f dB   control %.1f dB\n",
                     20.0 * std::log10 (wetF0 / dryF0), 20.0 * std::log10 (wetFc / (dryFc + 1e-12)));
    }

    void profileTest (double Fs)
    {
        std::printf ("Reduction profile (local scaling) @ Fs=%.0f\n", Fs);
        const int M = 1 << 15;
        const double f0 = 2000.0;
        std::mt19937 rng (9);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        // Profile zero around f0 -> no suppression there despite the resonance.
        const auto masked = render (Fs, x, x, [Fs, f0] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.2);
            std::vector<double> prof ((size_t) s.numBins(), 1.0);
            const int kf = (int) std::round (f0 * 2048.0 / Fs);
            for (int k = kf - 12; k <= kf + 12; ++k)
                if (k >= 0 && k < s.numBins()) prof[(size_t) k] = 0.0;
            s.setProfile (prof.data(), s.numBins());
        });

        const int a = M / 2, b = M;
        const double dryF0 = magAt (dry, a, b, f0, Fs), mF0 = magAt (masked, a, b, f0, Fs);
        if (mF0 < dryF0 * 0.85) fail ("profile=0 region still suppressed "
                                      + std::to_string (20.0 * std::log10 (mF0 / dryF0)) + " dB");
        std::printf ("  masked f0 %.2f dB (expect ~0)\n", 20.0 * std::log10 (mF0 / dryF0));
    }

    void stereoLinkTest (double Fs)
    {
        std::printf ("Stereo link (L==R) @ Fs=%.0f\n", Fs);
        const int M = 1 << 14;
        std::mt19937 rng (3);
        std::normal_distribution<double> g (0.0, 0.2);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 3000.0 * n / Fs);

        factory_core::ResonanceSuppressor s; s.prepare (Fs, 11);
        s.setDepth (1.2); s.setStereoLink (true);
        double e = 0.0;
        for (int n = 0; n < M; ++n)
        {
            double l = x[(size_t) n], r = x[(size_t) n];
            s.process (l, r);
            e = std::max (e, std::abs (l - r));
        }
        if (e > 1.0e-9) fail ("linked L/R diverged err " + std::to_string (e));
        std::printf ("  maxLRdiff=%.2e\n", e);
    }
}

int main (int argc, char** argv)
{
    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 96000.0 };

    fftTest();
    for (double Fs : rates)
    {
        reconstructionTest (Fs);
        deltaTest (Fs);
        suppressionTest (Fs);
        profileTest (Fs);
        stereoLinkTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
