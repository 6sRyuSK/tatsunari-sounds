//
// dsp_test.cpp — headless verification of the bus-compressor DSP core
// (factory_core::Compressor). Gates:
//
//   1. Static compression curve vs an INDEPENDENT gain-computer oracle: feed a
//      constant (DC) level through the real DSP path, let the ballistics settle,
//      and compare the steady-state output level (dB) to the oracle. Exercised
//      at every ratio the plugin exposes (2:1 / 4:1 / 10:1).
//   2. Formula-independent invariants: unity below threshold, slope 1/ratio
//      above it, exact makeup shift, monotonicity.
//   3. Attack / release time constants from a level step (time to reach 63% of
//      the final gain reduction), including the plugin's fastest-attack
//      extreme (0.1 ms).
//   4. Stereo link: detection on max(|L|,|R|) applies one shared gain.
//   5. Class J: digital silence / near-floor input under an aggressive setting
//      produces no phantom gain reduction.
//   6. Class E: reset()/prepare() clear ballistics state immediately — the very
//      next samples must match a freshly-constructed instance exactly.
//
// The oracle re-implements the gain computer independently; it never calls the
// DSP class under test.
//
#include "factory_core/Compressor.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fct = factory_core::testing;

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

    double settledGain (factory_core::Compressor& c, double levelLin, std::size_t n)
    {
        double g = 1.0;
        for (std::size_t i = 0; i < n; ++i)
            g = c.processDetector (levelLin);
        return g;
    }

    // Class H: settle length derives from Fs (settleSeconds * Fs), not a fixed
    // sample count, so the rate loop genuinely exercises every Fs. 2.0 s is
    // ~40 time constants at the 50 ms release used below (>= the ~1.81 s /
    // 80000-sample settle this replaces at 44.1 kHz) -- not a looser gate.
    constexpr double kSettleSeconds = 2.0;

    void staticCurveTest (double Fs, double ratio)
    {
        std::printf ("Static curve @ Fs=%.0f ratio=%.0f:1\n", Fs, ratio);
        const double thr = -20.0, makeup = 0.0;
        const std::size_t settle = (std::size_t) (kSettleSeconds * Fs);

        const double levels[] = { -60, -40, -30, -20, -10, 0, 6 };
        double prevOut = -1e9;
        for (double inDb : levels)
        {
            auto c = makeComp (Fs, thr, ratio, makeup, 1.0, 50.0);
            const double g = settledGain (c, dbToLin (inDb), settle);
            const double outDb = inDb + linToDb (g);
            const double expected = oracleOutDb (inDb, thr, ratio, makeup);
            if (std::abs (outDb - expected) > 1.0e-4)
                fail ("static curve ratio=" + std::to_string (ratio) + " inDb=" + std::to_string (inDb)
                      + " out=" + std::to_string (outDb) + " != " + std::to_string (expected));
            if (outDb < prevOut - 1.0e-9)
                fail ("curve not monotonic at ratio=" + std::to_string (ratio) + " inDb=" + std::to_string (inDb));
            prevOut = outDb;
        }

        // Below threshold -> unity (just makeup).
        {
            auto c = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            const double g = settledGain (c, dbToLin (-40.0), settle);
            if (std::abs (linToDb (g)) > 1.0e-4) fail ("below-threshold gain != 0 dB (ratio=" + std::to_string (ratio) + ")");
        }

        // Slope above threshold == 1/ratio.
        {
            auto c1 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            auto c2 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            const double out1 = -10.0 + linToDb (settledGain (c1, dbToLin (-10.0), settle));
            const double out2 =   0.0 + linToDb (settledGain (c2, dbToLin (  0.0), settle));
            const double slope = (out2 - out1) / (0.0 - (-10.0));
            if (std::abs (slope - 1.0 / ratio) > 1.0e-3)
                fail ("above-threshold slope " + std::to_string (slope) + " != 1/ratio (ratio=" + std::to_string (ratio) + ")");
        }

        // Makeup shifts output by exactly makeupDb.
        {
            auto c0 = makeComp (Fs, thr, ratio, 0.0, 1.0, 50.0);
            auto c6 = makeComp (Fs, thr, ratio, 6.0, 1.0, 50.0);
            const double o0 = linToDb (settledGain (c0, dbToLin (0.0), settle));
            const double o6 = linToDb (settledGain (c6, dbToLin (0.0), settle));
            if (std::abs ((o6 - o0) - 6.0) > 1.0e-4) fail ("makeup shift != 6 dB (ratio=" + std::to_string (ratio) + ")");
        }
        std::printf ("  ok\n");
    }

    // Attack/release ballistics vs. the same time-constant oracle: the analog
    // decoupled-ballistics IIR reaches 1-1/e (~63%) of the final gain reduction
    // at t == the configured time constant. `atkTolMs` is supplied by the
    // caller so the fastest-attack extreme (0.1 ms) can account for sample-
    // period quantization without loosening the existing 10 ms/200 ms gate's
    // 10% tolerance.
    void timingTest (double Fs, double atkMs, double relMs, double atkTolMs)
    {
        std::printf ("Attack/Release timing @ Fs=%.0f (attack=%.3f ms, release=%.1f ms)\n", Fs, atkMs, relMs);
        const double thr = -20.0, ratio = 4.0;
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
        if (std::abs (atkMeasured - atkMs) > atkTolMs)
            fail ("attack " + std::to_string (atkMeasured) + " ms != " + std::to_string (atkMs)
                  + " ms (tol " + std::to_string (atkTolMs) + ")");

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

        std::printf ("  attack=%.3f ms (target %.3f)  release=%.1f ms (target %.1f)\n",
                     atkMeasured, atkMs, relMeasured, relMs);
    }

    void stereoLinkTest (double Fs)
    {
        std::printf ("Stereo link @ Fs=%.0f\n", Fs);
        auto c = makeComp (Fs, -20.0, 4.0, 0.0, 1.0, 50.0);
        const double l0 = dbToLin (0.0), r0 = dbToLin (-40.0); // L loud, R quiet
        double l = l0, r = r0;
        const std::size_t n = (std::size_t) (kSettleSeconds * Fs);
        for (std::size_t i = 0; i < n; ++i) { l = l0; r = r0; c.processStereoSample (l, r); }
        const double gl = l / l0, gr = r / r0;
        if (std::abs (gl - gr) > 1.0e-9) fail ("stereo gains differ: " + std::to_string (gl) + " vs " + std::to_string (gr));
        std::printf ("  shared gain = %.4f (%.2f dB)\n", gl, linToDb (gl));
    }

    // Class J: absolute floor -- an aggressive setting (lowest threshold, highest
    // ratio the plugin exposes) must not reduce gain for digital silence or a
    // very low-level (-90 dBFS) tone that sits well below the threshold. No
    // relative-only detector logic should ever produce reduction here.
    void silenceFloorTest (double Fs)
    {
        std::printf ("Silence / absolute floor @ Fs=%.0f\n", Fs);
        const double thr = -40.0, ratio = 10.0; // aggressive: lowest thr, highest ratio in range
        const std::size_t n = (std::size_t) (0.2 * Fs);

        // Digital silence.
        {
            auto c = makeComp (Fs, thr, ratio, 0.0, 0.1, 10.0);
            double maxDev = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double g = c.processDetector (0.0);
                const double out = 0.0 * g;
                maxDev = std::max (maxDev, std::abs (g - 1.0));
                if (std::abs (out - 0.0) > 1.0e-12)
                    fail ("silence: output != input at sample " + std::to_string (i));
            }
            if (maxDev > 1.0e-9)
                fail ("silence produced phantom gain reduction, |g-1|=" + std::to_string (maxDev));
        }

        // -90 dBFS tone, well below the -40 dB threshold.
        {
            auto c = makeComp (Fs, thr, ratio, 0.0, 0.1, 10.0);
            const double quiet = dbToLin (-90.0);
            double maxDevDb = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double g = c.processDetector (quiet);
                const double out = quiet * g;
                if (std::abs (out - quiet) > 1.0e-9 * std::max (quiet, 1.0))
                    fail ("-90 dBFS tone: output != input at sample " + std::to_string (i));
                maxDevDb = std::max (maxDevDb, std::abs (linToDb (g)));
            }
            if (maxDevDb > 1.0e-6)
                fail ("-90 dBFS tone got phantom gain reduction: " + std::to_string (maxDevDb) + " dB");
        }
        std::printf ("  ok\n");
    }

    // Class E: reset() (and prepare(), which calls reset() internally) must
    // clear ballistics state immediately -- the very next samples after either
    // must match a freshly-constructed instance bit-for-bit (same params, same
    // zero initial state), not merely "eventually converge".
    void resetStateTest (double Fs)
    {
        std::printf ("Reset / re-prepare clears ballistics @ Fs=%.0f\n", Fs);
        const double thr = -20.0, ratio = 10.0, makeup = 0.0, atkMs = 0.5, relMs = 300.0;
        const std::size_t driveN = (std::size_t) (0.5 * Fs);
        const double loud = dbToLin (0.0); // well above threshold -> deep GR

        // Drive into deep gain reduction, then reset().
        auto driven = makeComp (Fs, thr, ratio, makeup, atkMs, relMs);
        for (std::size_t i = 0; i < driveN; ++i)
            driven.processDetector (loud);
        if (driven.currentGainReductionDb() > -1.0)
            fail ("setup did not reach deep GR before reset() check");
        driven.reset();

        // Drive a second instance into deep GR, then re-prepare() (which resets
        // internally) instead of calling reset() directly.
        auto reprepped = makeComp (Fs, thr, ratio, makeup, atkMs, relMs);
        for (std::size_t i = 0; i < driveN; ++i)
            reprepped.processDetector (loud);
        if (reprepped.currentGainReductionDb() > -1.0)
            fail ("setup did not reach deep GR before prepare() check");
        reprepped.prepare (Fs);

        // Reference: a fresh instance with identical params, never touched.
        auto fresh = makeComp (Fs, thr, ratio, makeup, atkMs, relMs);

        const std::size_t postN = (std::size_t) (0.05 * Fs);
        const double postSignal = dbToLin (-30.0); // arbitrary post-reset signal
        for (std::size_t i = 0; i < postN; ++i)
        {
            const double gDriven = driven.processDetector (postSignal);
            const double gRepr   = reprepped.processDetector (postSignal);
            const double gFresh  = fresh.processDetector (postSignal);

            if (std::abs (gDriven - gFresh) > 1.0e-12)
                fail ("reset(): residual ballistics vs fresh instance at sample " + std::to_string (i));
            if (std::abs (gRepr - gFresh) > 1.0e-12)
                fail ("prepare(): residual ballistics vs fresh instance at sample " + std::to_string (i));
        }
        std::printf ("  ok\n");
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        for (double ratio : { 2.0, 4.0, 10.0 })
            staticCurveTest (Fs, ratio);

        timingTest (Fs, 10.0, 200.0, 0.1 * 10.0);              // existing case, unchanged tolerance
        timingTest (Fs, 0.1, 200.0, std::max (0.1 * 0.1, 1.5 * 1000.0 / Fs)); // fastest attack in range (PluginProcessor.cpp: 0.1 ms min)

        stereoLinkTest (Fs);
        silenceFloorTest (Fs);
        resetStateTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
