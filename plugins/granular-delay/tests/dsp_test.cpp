//
// dsp_test.cpp — headless verification of the granular delay DSP core
// (factory_core::DelayLine + GranularDelay). The granular *character* is a
// sonic judgement; what is tested here is the deterministic machinery:
//
//   1. DelayLine fractional reads (a ramp reads back exactly as N-1-delay).
//   2. tempoSyncSeconds() note math.
//   3. Hann constant-overlap-add: with no jitter/pitch/spread and 50% overlap,
//      the grains reconstruct the delayed input (equal-power centre pan gives a
//      known 1/sqrt(2) per-channel gain).
//   4. Feedback decay: successive echoes fall by exactly the feedback gain.
//   5. Mix: at mix=0 the output equals the dry input sample-for-sample.
//
#include "factory_core/GranularDelay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    double rms (const std::vector<double>& v, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len && i < (int) v.size(); ++i) s += v[(size_t) i] * v[(size_t) i];
        return std::sqrt (s / len);
    }

    void delayLineTest()
    {
        std::printf ("DelayLine interpolation\n");
        factory_core::DelayLine dl;
        dl.prepare (2000);
        const int N = 500;
        for (int n = 0; n < N; ++n) dl.write ((double) n); // ramp

        for (double d : { 0.0, 1.0, 5.0, 5.5, 10.25, 100.0, 123.75 })
        {
            const double got = dl.readInterpolated (d);
            const double expected = (double) (N - 1) - d;
            if (std::abs (got - expected) > 1.0e-9)
                fail ("delay " + std::to_string (d) + ": " + std::to_string (got) + " != " + std::to_string (expected));
        }
        std::printf ("  ok\n");
    }

    void tempoSyncTest()
    {
        std::printf ("Tempo sync\n");
        using factory_core::tempoSyncSeconds;
        auto near = [] (double a, double b) { return std::abs (a - b) < 1e-12; };
        if (! near (tempoSyncSeconds (120.0, 1.0), 0.5))   fail ("1 beat @120 != 0.5s");
        if (! near (tempoSyncSeconds (120.0, 0.5), 0.25))  fail ("1/8 @120 != 0.25s");
        if (! near (tempoSyncSeconds (120.0, 0.75), 0.375))fail ("dotted 1/8 @120 != 0.375s");
        if (! near (tempoSyncSeconds (60.0, 1.0), 1.0))    fail ("1 beat @60 != 1s");
        std::printf ("  ok\n");
    }

    factory_core::GranularDelay makeEngine (double Fs, double grainMs, double delaySamp)
    {
        factory_core::GranularDelay g;
        g.prepare (Fs, 2.0);
        g.setGrainSizeMs (grainMs);
        g.setDensityHz (2000.0 / grainMs); // interval = grain/2 -> 50% overlap
        g.setDelaySamples (delaySamp);
        g.setFeedback (0.0);
        g.setPositionJitterMs (0.0);
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (0.0);
        g.setSpread (0.0);
        g.setMix (1.0);
        return g;
    }

    void colaTest (double Fs)
    {
        std::printf ("COLA reconstruction @ Fs=%.0f\n", Fs);
        const double grainMs = 100.0;
        const double delaySamp = 0.2 * Fs;
        auto g = makeEngine (Fs, grainMs, delaySamp);

        const double A = 0.5;
        const double w = 2.0 * kPi * 440.0 / Fs;
        int n = 0;
        const int settle = (int) (0.7 * Fs);
        for (int i = 0; i < settle; ++i, ++n) { double l = A * std::sin (w * n), r = l; g.processStereo (l, r); }

        const int M = (int) (0.2 * Fs);
        double so = 0.0;
        for (int i = 0; i < M; ++i, ++n)
        {
            double l = A * std::sin (w * n), r = l;
            g.processStereo (l, r);
            so += l * l;
        }
        const double outRms = std::sqrt (so / M);
        const double inRms = A / std::sqrt (2.0);
        const double ratio = outRms / inRms;
        if (std::abs (ratio - 1.0 / std::sqrt (2.0)) > 0.05)
            fail ("COLA gain " + std::to_string (ratio) + " != 0.707");
        std::printf ("  reconstructed gain = %.4f (expect 0.707)\n", ratio);
    }

    void feedbackTest (double Fs)
    {
        std::printf ("Feedback decay @ Fs=%.0f\n", Fs);
        const double grainMs = 30.0;
        const int D = (int) (0.2 * Fs);
        auto g = makeEngine (Fs, grainMs, (double) D);
        const double fb = 0.6;
        g.setFeedback (fb);

        const double A = 0.6;
        const double w = 2.0 * kPi * 440.0 / Fs;
        const int burst = (int) (0.04 * Fs);
        const int total = (int) (0.9 * Fs);

        std::vector<double> out ((size_t) total);
        for (int n = 0; n < total; ++n)
        {
            double x = (n < burst) ? A * std::sin (w * n) : 0.0;
            double l = x, r = x;
            g.processStereo (l, r);
            out[(size_t) n] = l;
        }

        // Echo energy in a window after each delay tap.
        const int win = (int) (0.06 * Fs);
        const double e1 = rms (out, D, win);
        const double e2 = rms (out, 2 * D, win);
        const double e3 = rms (out, 3 * D, win);
        const double r12 = e2 / e1, r23 = e3 / e2;
        if (std::abs (r12 - fb) > 0.12) fail ("echo ratio 2/1 " + std::to_string (r12) + " != fb");
        if (std::abs (r23 - fb) > 0.12) fail ("echo ratio 3/2 " + std::to_string (r23) + " != fb");
        std::printf ("  echo ratios = %.3f, %.3f (expect %.2f)\n", r12, r23, fb);
    }

    void mixTest (double Fs)
    {
        std::printf ("Mix=0 dry passthrough @ Fs=%.0f\n", Fs);
        auto g = makeEngine (Fs, 80.0, 0.1 * Fs);
        g.setFeedback (0.5);
        g.setMix (0.0);

        const double w = 2.0 * kPi * 330.0 / Fs;
        double maxErr = 0.0;
        for (int n = 0; n < (int) (0.2 * Fs); ++n)
        {
            const double x = 0.4 * std::sin (w * n);
            double l = x, r = x;
            g.processStereo (l, r);
            maxErr = std::max (maxErr, std::abs (l - x));
        }
        if (maxErr > 1.0e-12) fail ("mix=0 not exact dry: err " + std::to_string (maxErr));
        std::printf ("  max err = %.2e\n", maxErr);
    }
}

int main (int argc, char** argv)
{
    delayLineTest();
    tempoSyncTest();

    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 96000.0 };

    for (double Fs : rates)
    {
        colaTest (Fs);
        feedbackTest (Fs);
        mixTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
