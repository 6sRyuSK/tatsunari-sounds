//
// dsp_test.cpp — headless verification of the vocal multiband compressor DSP
// core (factory_core::Crossover3 / MultibandCompressor). Gates:
//
//   1. Crossover reconstruction: low+mid+high sums to a flat magnitude.
//   2. Band isolation: each band rolls off outside its range.
//   3. Multiband transparent at ratio 1 (flat magnitude end to end).
//   4. Per-band compression: a loud tone in one band is compressed while a tone
//      in another (ratio 1) band passes unchanged.
//   5. Mix=0 is exact dry passthrough.
//
// The per-band compressor is factory_core::Compressor, already verified by the
// bus-compressor tests; here we check the crossover and the wiring.
//
#include "factory_core/Crossover3.h"
#include "factory_core/MultibandCompressor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    cd dftAt (const std::vector<double>& h, double w)
    {
        const cd step = std::exp (cd (0.0, -w));
        cd zk (1.0, 0.0), acc (0.0, 0.0);
        for (double hn : h) { acc += hn * zk; zk *= step; }
        return acc;
    }
    double magDb (const cd& h) { return 20.0 * std::log10 (std::abs (h)); }
    double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
    double linToDb (double l)  { return 20.0 * std::log10 (std::max (l, 1e-12)); }

    void crossoverTest (double Fs)
    {
        std::printf ("Crossover reconstruction + isolation @ Fs=%.0f\n", Fs);
        const int N = 1 << 16;
        const double f1 = 250.0, f2 = 4000.0;
        factory_core::Crossover3 xo;
        xo.prepare (Fs);
        xo.setFrequencies (f1, f2);

        std::vector<double> sum ((size_t) N), low ((size_t) N), mid ((size_t) N), high ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            const double x = (n == 0) ? 1.0 : 0.0;
            double l, m, h;
            xo.process (x, l, m, h);
            low[(size_t) n] = l; mid[(size_t) n] = m; high[(size_t) n] = h;
            sum[(size_t) n] = l + m + h;
        }

        // Reconstruction: |sum| flat across the spectrum.
        double maxDev = 0.0;
        const int M = 64;
        for (int k = 0; k < M; ++k)
        {
            const double f = 20.0 * std::pow ((0.45 * Fs) / 20.0, (double) k / (M - 1));
            const double w = 2.0 * kPi * f / Fs;
            maxDev = std::max (maxDev, std::abs (magDb (dftAt (sum, w))));
        }
        if (maxDev > 0.1) fail ("reconstruction not flat: " + std::to_string (maxDev) + " dB");

        // Isolation: low passes 100, rejects 1500; high passes 8000, rejects 500.
        auto at = [&] (const std::vector<double>& v, double f) { return magDb (dftAt (v, 2.0 * kPi * f / Fs)); };
        if (at (low, 100.0) < -1.0 || at (low, 1500.0) > -10.0) fail ("low band isolation");
        if (at (high, 8000.0) < -1.0 || at (high, 500.0) > -10.0) fail ("high band isolation");
        if (at (mid, 1000.0) < -1.0) fail ("mid band passband");
        std::printf ("  recon maxDev=%.4f dB  low@100=%.1f low@1500=%.1f high@8k=%.1f high@500=%.1f\n",
                     maxDev, at (low, 100.0), at (low, 1500.0), at (high, 8000.0), at (high, 500.0));
    }

    void transparentTest (double Fs)
    {
        std::printf ("Multiband transparent @ Fs=%.0f\n", Fs);
        const int N = 1 << 16;
        factory_core::MultibandCompressor mb;
        mb.prepare (Fs);
        mb.setCrossover (250.0, 4000.0);
        for (int i = 0; i < 3; ++i) { mb.band (i).setThresholdDb (0.0); mb.band (i).setRatio (1.0); }
        mb.setMix (1.0);

        std::vector<double> ir ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            double l = (n == 0) ? 1.0 : 0.0, r = l;
            mb.processStereo (l, r);
            ir[(size_t) n] = l;
        }
        double maxDev = 0.0;
        const int M = 64;
        for (int k = 0; k < M; ++k)
        {
            const double f = 20.0 * std::pow ((0.45 * Fs) / 20.0, (double) k / (M - 1));
            maxDev = std::max (maxDev, std::abs (magDb (dftAt (ir, 2.0 * kPi * f / Fs))));
        }
        if (maxDev > 0.1) fail ("not transparent at ratio 1: " + std::to_string (maxDev) + " dB");
        std::printf ("  maxDev=%.4f dB\n", maxDev);
    }

    // Settled output RMS for a steady tone through the multiband comp.
    double toneRms (factory_core::MultibandCompressor& mb, double Fs, double f, double amp, int settle, int measure)
    {
        const double w = 2.0 * kPi * f / Fs;
        for (int n = 0; n < settle; ++n) { double l = amp * std::sin (w * n), r = l; mb.processStereo (l, r); }
        double s = 0.0;
        for (int n = 0; n < measure; ++n)
        {
            double l = amp * std::sin (w * (settle + n)), r = l;
            mb.processStereo (l, r);
            s += l * l;
        }
        return std::sqrt (s / measure);
    }

    void perBandTest (double Fs)
    {
        std::printf ("Per-band compression @ Fs=%.0f\n", Fs);
        const int settle = (int) (0.3 * Fs), measure = (int) (0.1 * Fs);
        const double amp = dbToLin (-6.0);

        auto makeMb = [&] (double midRatio) {
            auto mb = std::make_unique<factory_core::MultibandCompressor>();
            mb->prepare (Fs);
            mb->setCrossover (250.0, 4000.0);
            mb->setMix (1.0);
            for (int i = 0; i < 3; ++i) { mb->band (i).setThresholdDb (0.0); mb->band (i).setRatio (1.0); }
            // Mid band compresses.
            mb->band (1).setThresholdDb (-24.0);
            mb->band (1).setRatio (midRatio);
            mb->band (1).setAttackMs (5.0);
            mb->band (1).setReleaseMs (80.0);
            mb->band (1).prepare (Fs); // apply attack/release coeffs
            return mb;
        };

        // 1 kHz lives in the mid band -> compressed when ratio>1.
        auto comp = makeMb (4.0);
        auto flat = makeMb (1.0);
        const double midComp = toneRms (*comp, Fs, 1000.0, amp, settle, measure);
        const double midFlat = toneRms (*flat, Fs, 1000.0, amp, settle, measure);
        if (midComp >= midFlat * 0.99) fail ("mid band not compressed (" + std::to_string (midComp) + " vs " + std::to_string (midFlat) + ")");

        // 100 Hz lives in the low band (ratio 1) -> unchanged.
        auto comp2 = makeMb (4.0);
        auto flat2 = makeMb (1.0);
        const double lowComp = toneRms (*comp2, Fs, 100.0, amp, settle, measure);
        const double lowFlat = toneRms (*flat2, Fs, 100.0, amp, settle, measure);
        if (std::abs (linToDb (lowComp) - linToDb (lowFlat)) > 0.5) fail ("low band changed though ratio 1");

        std::printf ("  mid: %.2f dB GR;  low unchanged (%.2f vs %.2f dB)\n",
                     linToDb (midComp) - linToDb (midFlat), linToDb (lowComp), linToDb (lowFlat));
    }

    void mixTest (double Fs)
    {
        std::printf ("Mix=0 dry passthrough @ Fs=%.0f\n", Fs);
        factory_core::MultibandCompressor mb;
        mb.prepare (Fs);
        mb.setCrossover (250.0, 4000.0);
        mb.setMix (0.0);
        for (int i = 0; i < 3; ++i) { mb.band (i).setThresholdDb (-40.0); mb.band (i).setRatio (8.0); mb.band (i).prepare (Fs); }
        double maxErr = 0.0;
        for (int n = 0; n < (int) (0.2 * Fs); ++n)
        {
            const double x = 0.4 * std::sin (2.0 * kPi * 800.0 * n / Fs);
            double l = x, r = x;
            mb.processStereo (l, r);
            maxErr = std::max (maxErr, std::abs (l - x));
        }
        if (maxErr > 1e-12) fail ("mix=0 not exact dry: " + std::to_string (maxErr));
        std::printf ("  max err = %.2e\n", maxErr);
    }
}

int main (int argc, char** argv)
{
    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 96000.0 };

    for (double Fs : rates)
    {
        crossoverTest (Fs);
        transparentTest (Fs);
        perBandTest (Fs);
        mixTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
