//
// dsp_test.cpp — headless verification of the bus-compressor DSP core
// (factory_core::Compressor). Gates:
//
//   1. Static compression curve vs an INDEPENDENT gain-computer oracle: feed a
//      constant (DC) level through the real DSP path, let the ballistics settle,
//      and compare the steady-state output level (dB) to the oracle.
//   2. Formula-independent invariants: unity below threshold, slope 1/ratio
//      above it, exact makeup shift, monotonicity.
//   3. Attack / release time constants from a level step (time to reach 63% of
//      the final gain reduction).
//   4. Stereo link: detection on max(|L|,|R|) applies one shared gain.
//
// The oracle re-implements the gain computer independently; it never calls the
// DSP class under test.
//
#include "factory_core/Compressor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
    double linToDb (double l)  { return 20.0 * std::log10 (std::max (l, 1.0e-12)); }

    // INDEPENDENT oracle: hard-knee gain computer + makeup.
    double oracleOutDb (double inDb, double thr, double ratio, double makeup)
    {
        const double out = (inDb <= thr) ? inDb : thr + (inDb - thr) / ratio;
        return out + makeup;
    }

    factory_core::Compressor makeComp (double Fs, double thr, double ratio,
                                       double makeup, double atkMs, double relMs)
    {
        factory_core::Compressor c;
        c.setThresholdDb (thr);
        c.setRatio (ratio);
        c.setMakeupDb (makeup);
        c.setAttackMs (atkMs);
        c.setReleaseMs (relMs);
        c.prepare (Fs);
        return c;
    }

    double settledGain (factory_core::Compressor& c, double levelLin, int n)
    {
        double g = 1.0;
        for (int i = 0; i < n; ++i)
            g = c.processDetector (levelLin);
        return g;
    }

    void staticCurveTest (double Fs)
    {
        std::printf ("Static curve @ Fs=%.0f\n", Fs);
        const double thr = -20.0, ratio = 4.0, makeup = 0.0;
        const int settle = 80000;

        const double levels[] = { -60, -40, -30, -20, -10, 0, 6 };
        double prevOut = -1e9;
        for (double inDb : levels)
        {
            auto c = makeComp (Fs, thr, ratio, makeup, 1.0, 50.0);
            const double g = settledGain (c, dbToLin (inDb), settle);
            const double outDb = inDb + linToDb (g);
            const double expected = oracleOutDb (inDb, thr, ratio, makeup);
            if (std::abs (outDb - expected) > 1.0e-4)
                fail ("static curve inDb=" + std::to_string (inDb) + " out=" + std::to_string (outDb)
                      + " != " + std::to_string (expected));
            if (outDb < prevOut - 1.0e-9)
                fail ("curve not monotonic at inDb=" + std::to_string (inDb));
            prevOut = outDb;
        }

        // Below threshold -> unity (just makeup).
        {
            auto c = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            const double g = settledGain (c, dbToLin (-40.0), settle);
            if (std::abs (linToDb (g)) > 1.0e-4) fail ("below-threshold gain != 0 dB");
        }

        // Slope above threshold == 1/ratio.
        {
            auto c1 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            auto c2 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            const double out1 = -10.0 + linToDb (settledGain (c1, dbToLin (-10.0), settle));
            const double out2 =   0.0 + linToDb (settledGain (c2, dbToLin (  0.0), settle));
            const double slope = (out2 - out1) / (0.0 - (-10.0));
            if (std::abs (slope - 1.0 / ratio) > 1.0e-3)
                fail ("above-threshold slope " + std::to_string (slope) + " != 1/ratio");
        }

        // Makeup shifts output by exactly makeupDb.
        {
            auto c0 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            auto c6 = makeComp (Fs, thr, ratio, 6.0, 1.0, 50.0);
            const double o0 = linToDb (settledGain (c0, dbToLin (0.0), settle));
            const double o6 = linToDb (settledGain (c6, dbToLin (0.0), settle));
            if (std::abs ((o6 - o0) - 6.0) > 1.0e-4) fail ("makeup shift != 6 dB");
        }
        std::printf ("  ok\n");
    }

    void timingTest (double Fs)
    {
        std::printf ("Attack/Release timing @ Fs=%.0f\n", Fs);
        const double thr = -20.0, ratio = 4.0;
        const double atkMs = 10.0, relMs = 200.0;
        auto c = makeComp (Fs, thr, ratio, 0.0, atkMs, relMs);

        const double loud = dbToLin (0.0);        // GR target = -15 dB
        const double finalGr = c.staticGainDb (0.0);

        // Attack: time to reach 63% of final GR from rest.
        c.reset();
        int nAtk = -1;
        for (int i = 0; i < (int) (Fs * 0.5); ++i)
        {
            c.processDetector (loud);
            if (nAtk < 0 && c.currentGainReductionDb() <= 0.63 * finalGr) nAtk = i + 1;
        }
        const double atkMeasured = 1000.0 * nAtk / Fs;
        if (std::abs (atkMeasured - atkMs) > 0.1 * atkMs)
            fail ("attack " + std::to_string (atkMeasured) + " ms != " + std::to_string (atkMs));

        // Release: from settled GR, drop to silence; time to recover to 37% of GR.
        const double quiet = dbToLin (-80.0);
        int nRel = -1;
        for (int i = 0; i < (int) (Fs * 2.0); ++i)
        {
            c.processDetector (quiet);
            if (nRel < 0 && c.currentGainReductionDb() >= 0.37 * finalGr) nRel = i + 1;
        }
        const double relMeasured = 1000.0 * nRel / Fs;
        if (std::abs (relMeasured - relMs) > 0.1 * relMs)
            fail ("release " + std::to_string (relMeasured) + " ms != " + std::to_string (relMs));

        std::printf ("  attack=%.2f ms (target %.1f)  release=%.1f ms (target %.1f)\n",
                     atkMeasured, atkMs, relMeasured, relMs);
    }

    void stereoLinkTest (double Fs)
    {
        std::printf ("Stereo link @ Fs=%.0f\n", Fs);
        auto c = makeComp (Fs, -20.0, 4.0, 0.0, 1.0, 50.0);
        const double l0 = dbToLin (0.0), r0 = dbToLin (-40.0); // L loud, R quiet
        double l = l0, r = r0;
        for (int i = 0; i < 80000; ++i) { l = l0; r = r0; c.processStereoSample (l, r); }
        const double gl = l / l0, gr = r / r0;
        if (std::abs (gl - gr) > 1.0e-9) fail ("stereo gains differ: " + std::to_string (gl) + " vs " + std::to_string (gr));
        std::printf ("  shared gain = %.4f (%.2f dB)\n", gl, linToDb (gl));
    }
}

int main (int argc, char** argv)
{
    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    for (double Fs : rates)
    {
        staticCurveTest (Fs);
        timingTest (Fs);
        stereoLinkTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
