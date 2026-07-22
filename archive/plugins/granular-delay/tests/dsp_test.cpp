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
//   6. Sparse-overlap feedback stability (class A): impulse response is
//      non-increasing at max feedback in the sparse-overlap region.
//   7. Finite-guard self-recovery + long-hold boundedness (class C): the
//      feedback write self-recovers from an injected Inf, and a tens-of-seconds
//      hold at max feedback stays finite and realistically bounded.
//   8. Pitch / pitch-random / jitter / spread paths (class H): these were pinned
//      to 0 in every earlier test, so their bug paths were never exercised.
//      pitch=+12 st transposes the wet tone up an octave (independent DFT
//      oracle); pitch extremes and max jitter/pitch-random stay finite, bounded
//      and non-increasing at high feedback; spread=100 gives a stereo, equal-
//      power field while spread=0 gives an identical L/R mono field.
//   9. Worst-case delay buffer (class D): with the 6.0 s buffer the processor
//      allocates, a delay near the tempo-sync + LFO maximum is honoured, not
//      silently clamped.
//
#include "factory_core/GranularDelay.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace fct = factory_core::testing;

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

    // Single-bin DFT magnitude (independent spectral oracle). Used with integer
    // bins (frequency = k * Fs / len) so there is no leakage.
    double binMag (const std::vector<double>& x, int start, int len, double freq, double Fs)
    {
        const double w = 2.0 * kPi * freq / Fs;
        double re = 0.0, im = 0.0;
        for (int i = 0; i < len && start + i < (int) x.size(); ++i)
        {
            const double v = x[(size_t) (start + i)];
            re += v * std::cos (w * i);
            im += v * std::sin (w * i);
        }
        return std::sqrt (re * re + im * im);
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

    // Sparse-overlap feedback stability (issue #34). In the sparse region
    // (density such that grain interval > grainSize/2) an isolated grain must
    // not amplify the delayed signal, or feedback*grain-gain can exceed 1 and
    // the loop diverges. Now that the core caps grain gain at unity, drive an
    // impulse through the loop at max feedback and assert the impulse response
    // is NON-INCREASING and bounded: the delay-tap peaks must not grow tap over
    // tap, and the tail energy must decay. This is an independent bound (loop
    // gain < 1), not derived from the implementation's coefficients.
    void sparseOverlapDecayTest (double Fs)
    {
        std::printf ("Sparse-overlap feedback decay @ Fs=%.0f\n", Fs);
        // density = 20 Hz, grainSize = 40 ms -> interval = Fs/20, grain = 0.04*Fs.
        // interval (0.05*Fs) > grain/2 (0.02*Fs): genuinely sparse overlap.
        const double grainMs = 40.0;
        const int D = (int) (0.15 * Fs);

        factory_core::GranularDelay g;
        g.prepare (Fs, 2.0);
        g.setGrainSizeMs (grainMs);
        g.setDensityHz (20.0);
        g.setDelaySamples ((double) D);
        g.setFeedback (0.95); // plugin's feedback maximum
        g.setPositionJitterMs (0.0);
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (0.0);
        g.setSpread (0.0);
        g.setMix (1.0);

        const int total = (int) (3.0 * Fs);
        std::vector<double> out ((size_t) total);
        double globalPeak = 0.0;
        for (int n = 0; n < total; ++n)
        {
            double x = (n == 0) ? 1.0 : 0.0; // unit impulse
            double l = x, r = x;
            g.processStereo (l, r);
            out[(size_t) n] = l;
            globalPeak = std::max (globalPeak, std::abs (l));
        }

        // Bounded: nothing may blow up (allow modest transient headroom).
        if (! std::isfinite (globalPeak) || globalPeak > 4.0)
            fail ("sparse-overlap output not bounded: peak " + std::to_string (globalPeak));

        // Per-tap peak: measure the largest |sample| in a window around each
        // successive delay tap; require it to be NON-INCREASING (loop gain < 1).
        const int win = (int) (0.05 * Fs);
        auto tapPeak = [&] (int centre)
        {
            double p = 0.0;
            for (int i = std::max (0, centre - win); i < std::min (total, centre + win); ++i)
                p = std::max (p, std::abs (out[(size_t) i]));
            return p;
        };
        double prev = tapPeak (D);
        for (int k = 2; k <= 12; ++k)
        {
            const int centre = k * D;
            if (centre + win >= total) break;
            const double p = tapPeak (centre);
            // Allow a tiny epsilon for grain-scheduling phase, but forbid growth.
            if (p > prev + 1.0e-9)
                fail ("sparse-overlap tap " + std::to_string (k) + " grew: "
                      + std::to_string (p) + " > " + std::to_string (prev));
            prev = p;
        }

        // Late-window energy must have decayed well below the first echo.
        const double eEarly = rms (out, D, win);
        const double eLate  = rms (out, total - win, win);
        if (! (eLate < eEarly))
            fail ("sparse-overlap tail did not decay: late " + std::to_string (eLate)
                  + " >= early " + std::to_string (eEarly));
        std::printf ("  peak=%.4f  early rms=%.4e  late rms=%.4e (non-increasing)\n",
                     globalPeak, eEarly, eLate);
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

    // --- class H: pitch path (was pinned to 0 in every test) -----------------
    //
    // pitch=+12 st -> grain playback rate 2^(12/12) = 2.0, so a steady input
    // tone at f0 comes back transposed to 2*f0. Independent oracle: a single-bin
    // DFT of the wet output must have more energy at 2*f0 than at f0. f0 is tied
    // to Fs and placed on an integer bin (both f0 and 2*f0) so the DFT is exact.
    void pitchTransposeTest (double Fs)
    {
        std::printf ("Pitch +12 st octave-up @ Fs=%.0f\n", Fs);
        const int    Nblk  = 16384;
        const int    binF0 = 400;                       // integer bins -> no leakage
        const double f0    = binF0 * Fs / (double) Nblk; // 2*f0 sits at bin 800
        if (2.0 * f0 >= 0.45 * Fs) { std::printf ("  (skipped: 2*f0 out of band)\n"); return; }

        const double grainMs = 80.0;
        auto g = makeEngine (Fs, grainMs, 0.3 * Fs);
        g.setPitchSemitones (12.0); // rate 2.0

        const double A = 0.5;
        const double w = 2.0 * kPi * f0 / Fs;
        int n = 0;
        const int settle = (int) (0.5 * Fs);
        for (int i = 0; i < settle; ++i, ++n) { double l = A * std::sin (w * n), r = l; g.processStereo (l, r); }

        std::vector<double> out ((size_t) Nblk);
        for (int i = 0; i < Nblk; ++i, ++n)
        {
            double l = A * std::sin (w * n), r = l;
            g.processStereo (l, r);
            out[(size_t) i] = 0.5 * (l + r);
        }
        if (! fct::allFinite (out)) { fail ("pitch transpose produced non-finite output"); return; }

        const double magF0  = binMag (out, 0, Nblk, f0,       Fs);
        const double mag2F0 = binMag (out, 0, Nblk, 2.0 * f0, Fs);
        // Independent oracle: energy moved up an octave. Require a clear margin.
        if (! (mag2F0 > 2.0 * magF0))
            fail ("pitch +12 st did not transpose up: |X(2f0)|=" + std::to_string (mag2F0)
                  + " not >> |X(f0)|=" + std::to_string (magF0));
        std::printf ("  |X(f0)|=%.3e  |X(2f0)|=%.3e  ratio=%.2f (expect >2)\n",
                     magF0, mag2F0, mag2F0 / std::max (1e-30, magF0));
    }

    // pitch extremes (-24 / +24 st) with high feedback: the loop must stay
    // finite, bounded and non-increasing (class A with the pitch path active).
    void pitchExtremeStabilityTest (double Fs)
    {
        std::printf ("Pitch extremes stability @ Fs=%.0f\n", Fs);
        for (double st : { -24.0, 24.0 })
        {
            factory_core::GranularDelay g;
            g.prepare (Fs, 2.0);
            g.setGrainSizeMs (40.0);
            g.setDensityHz (20.0);           // sparse overlap (worst case)
            g.setDelaySamples (0.15 * Fs);
            g.setFeedback (0.95);            // plugin maximum
            g.setPositionJitterMs (0.0);
            g.setPitchSemitones (st);
            g.setPitchRandomSemis (0.0);
            g.setSpread (0.0);
            g.setMix (1.0);

            auto proc = [&g] (double x) { double l = x, r = x; g.processStereo (l, r); return 0.5 * (l + r); };
            if (! fct::impulseResponseNonIncreasing (proc, Fs, 3.0))
                fail ("pitch " + std::to_string (st) + " st: impulse response not non-increasing/finite");

            // Independent peak bound over the same path (realistic, not 1e6).
            factory_core::GranularDelay g2;
            g2.prepare (Fs, 2.0);
            g2.setGrainSizeMs (40.0); g2.setDensityHz (20.0); g2.setDelaySamples (0.15 * Fs);
            g2.setFeedback (0.95); g2.setPitchSemitones (st); g2.setSpread (0.0); g2.setMix (1.0);
            const int total = (int) (3.0 * Fs);
            std::vector<double> out ((size_t) total);
            for (int nn = 0; nn < total; ++nn)
            {
                double x = (nn == 0) ? 1.0 : 0.0, l = x, r = x;
                g2.processStereo (l, r);
                out[(size_t) nn] = l;
            }
            if (! fct::allFinite (out)) fail ("pitch " + std::to_string (st) + " st: non-finite");
            const double pk = fct::peakAbs (out);
            if (pk > 4.0) fail ("pitch " + std::to_string (st) + " st: peak " + std::to_string (pk) + " > 4.0");
        }
        std::printf ("  ok (finite, bounded, non-increasing at +/-24 st)\n");
    }

    // jitter=max (200 ms) and pitchRand=max (12 st) with high feedback: random
    // read positions and per-grain rates must not produce clamp artifacts, NaN
    // or runaway peaks.
    void jitterPitchRandStabilityTest (double Fs)
    {
        std::printf ("Jitter+PitchRand extremes stability @ Fs=%.0f\n", Fs);
        factory_core::GranularDelay g;
        g.prepare (Fs, 2.0);
        g.setGrainSizeMs (40.0);
        g.setDensityHz (20.0);
        g.setDelaySamples (0.15 * Fs);
        g.setFeedback (0.95);
        g.setPositionJitterMs (200.0);   // parameter maximum
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (12.0);    // parameter maximum
        g.setSpread (0.0);
        g.setMix (1.0);

        const int total = (int) (4.0 * Fs);
        std::vector<double> out ((size_t) total);
        for (int n = 0; n < total; ++n)
        {
            double x = (n == 0) ? 1.0 : 0.0, l = x, r = x;
            g.processStereo (l, r);
            out[(size_t) n] = l;
        }
        if (! fct::allFinite (out)) fail ("jitter/pitchrand max: non-finite output");
        const double pk = fct::peakAbs (out);
        if (pk > 4.0) fail ("jitter/pitchrand max: peak " + std::to_string (pk) + " > 4.0");
        std::printf ("  peak=%.4f (finite, bounded)\n", pk);
    }

    // --- class H: spread / stereo panning path -------------------------------
    //
    // spread=100 must produce a genuine stereo field (L != R) whose per-channel
    // energy sums back to the mono energy (equal-power law: gL^2+gR^2==1). With
    // sparse, non-overlapping grains the cross-grain terms vanish so the equality
    // is tight. spread=0 must give an identical L/R field (centre pan). The RNG
    // sequence is identical between the runs (jitter/pitch draws happen either
    // way), so only the panning differs.
    void spreadTest (double Fs)
    {
        std::printf ("Spread stereo/equal-power @ Fs=%.0f\n", Fs);
        const double grainMs = 20.0;
        const double density = 10.0; // interval = 0.1*Fs >> grain (0.02*Fs): sparse

        auto run = [&] (double spread, std::vector<double>& L, std::vector<double>& R)
        {
            factory_core::GranularDelay g;
            g.prepare (Fs, 2.0);
            g.setGrainSizeMs (grainMs);
            g.setDensityHz (density);
            g.setDelaySamples (0.2 * Fs);
            g.setFeedback (0.0);
            g.setPositionJitterMs (0.0);
            g.setPitchSemitones (0.0);
            g.setPitchRandomSemis (0.0);
            g.setSpread (spread);
            g.setMix (1.0);

            const double A = 0.5, w = 2.0 * kPi * 440.0 / Fs;
            const int settle = (int) (0.3 * Fs);
            int n = 0;
            for (int i = 0; i < settle; ++i, ++n) { double l = A * std::sin (w * n), r = l; g.processStereo (l, r); }
            const int M = (int) (0.7 * Fs);
            L.assign ((size_t) M, 0.0); R.assign ((size_t) M, 0.0);
            for (int i = 0; i < M; ++i, ++n)
            {
                double l = A * std::sin (w * n), r = l;
                g.processStereo (l, r);
                L[(size_t) i] = l; R[(size_t) i] = r;
            }
        };

        std::vector<double> L0, R0, L1, R1;
        run (0.0, L0, R0);
        run (1.0, L1, R1);

        // spread=0 -> centre pan -> L == R exactly.
        double diff0 = 0.0;
        for (size_t i = 0; i < L0.size(); ++i) diff0 = std::max (diff0, std::abs (L0[i] - R0[i]));
        if (diff0 > 1.0e-12) fail ("spread=0 not mono (L!=R): max diff " + std::to_string (diff0));

        // spread=100 -> a real stereo difference must exist.
        double diff1 = 0.0;
        for (size_t i = 0; i < L1.size(); ++i) diff1 = std::max (diff1, std::abs (L1[i] - R1[i]));
        if (! (diff1 > 1.0e-3)) fail ("spread=100 has no stereo difference: max diff " + std::to_string (diff1));

        // Equal power: total L^2+R^2 preserved vs the mono (spread=0) energy.
        auto energy2 = [] (const std::vector<double>& a, const std::vector<double>& b)
        {
            double e = 0.0;
            for (size_t i = 0; i < a.size(); ++i) e += a[i] * a[i] + b[i] * b[i];
            return e;
        };
        const double e0 = energy2 (L0, R0);
        const double e1 = energy2 (L1, R1);
        const double rel = std::abs (e1 - e0) / std::max (1e-30, e0);
        if (rel > 0.15)
            fail ("spread=100 not equal-power: |dE|/E = " + std::to_string (rel) + " > 0.15");
        std::printf ("  L!=R diff=%.4f  equal-power |dE|/E=%.4f\n", diff1, rel);
    }

    // --- class D: worst-case delay buffer ------------------------------------
    //
    // The processor allocates a 6.0 s buffer (kMaxDelaySeconds) to cover the
    // tempo-sync worst case (dotted 1/4 at 20 BPM = 4.5 s) plus the +25% LFO
    // headroom -> 5.625 s. A delay near that maximum must be honoured, not
    // silently clamped down to a shorter buffer. Independent oracle: with
    // jitter=0 and rate=1 the grain reads a fixed position d0=delaySamples, so an
    // input impulse reappears in the wet output at output time ~= delaySamples.
    void worstCaseBufferTest (double Fs)
    {
        std::printf ("Worst-case 6s buffer, delay near max @ Fs=%.0f\n", Fs);
        const double maxDelaySeconds = 6.0;             // == processor kMaxDelaySeconds
        const double requested = 5.6 * Fs;              // < 5.625 s (reachable max), > old 2 s buffer

        factory_core::GranularDelay g;
        g.prepare (Fs, maxDelaySeconds);
        g.setGrainSizeMs (80.0);
        g.setDensityHz (25.0);
        g.setDelaySamples (requested);
        g.setFeedback (0.0);
        g.setPositionJitterMs (0.0);
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (0.0);
        g.setSpread (0.0);
        g.setMix (1.0);

        const int grain = (int) (0.08 * Fs);
        const int total = (int) (5.9 * Fs);
        std::vector<double> out ((size_t) total);
        for (int n = 0; n < total; ++n)
        {
            double x = (n == 0) ? 1.0 : 0.0, l = x, r = x;
            g.processStereo (l, r);
            out[(size_t) n] = l;
        }
        if (! fct::allFinite (out)) { fail ("worst-case buffer: non-finite output"); return; }

        // Energy must appear in a window around the requested delay ...
        const int atReq = (int) requested;
        const double eReq = rms (out, std::max (0, atReq - grain), 2 * grain);
        // ... and NOT at ~2.0 s, where a too-short (old 2 s) buffer would clamp it.
        const int atClamp = (int) (2.0 * Fs);
        const double eClamp = rms (out, std::max (0, atClamp - grain), 2 * grain);

        if (! (eReq > 1.0e-4))
            fail ("worst-case buffer: no wet energy at requested delay (clamped/lost): eReq="
                  + std::to_string (eReq));
        if (! (eReq > 8.0 * eClamp))
            fail ("worst-case buffer: energy appears near 2 s clamp point, not at requested delay: eReq="
                  + std::to_string (eReq) + " eClamp=" + std::to_string (eClamp));
        std::printf ("  eReq=%.4e (@%.2fs)  eClamp=%.4e (@2.0s)\n", eReq, requested / Fs, eClamp);
    }

    // --- class C: long hold + finite-guard self-recovery ---------------------

    // Tens-of-seconds hold at max feedback in the sparse-overlap worst case must
    // stay finite and realistically bounded (not "not-NaN" 1e6 tolerance).
    void longHoldFiniteTest (double Fs)
    {
        std::printf ("Long-hold finite/bounded (30 s, fb=0.95) @ Fs=%.0f\n", Fs);
        factory_core::GranularDelay g;
        g.prepare (Fs, 2.0);
        g.setGrainSizeMs (40.0);
        g.setDensityHz (20.0);       // sparse overlap (worst case)
        g.setDelaySamples (0.15 * Fs);
        g.setFeedback (0.95);        // plugin maximum
        g.setPositionJitterMs (0.0);
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (0.0);
        g.setSpread (0.0);
        g.setMix (1.0);

        const int burst = (int) (0.05 * Fs);
        const int total = (int) (30.0 * Fs);
        const double A = 0.7, w = 2.0 * kPi * 440.0 / Fs;
        double peak = 0.0;
        bool finite = true;
        for (int n = 0; n < total; ++n)
        {
            double x = (n < burst) ? A * std::sin (w * n) : 0.0, l = x, r = x;
            g.processStereo (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) finite = false;
            peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
        }
        if (! finite) fail ("long hold went non-finite");
        if (peak > 4.0) fail ("long hold peak " + std::to_string (peak) + " > 4.0 (unrealistic)");
        std::printf ("  peak over 30 s = %.4f (finite, bounded)\n", peak);
    }

    // Inject a single non-finite input into the feedback path; the finite guard
    // must flush the loop so the output returns to finite silence. Without the
    // guard, the Inf would enter the delay buffer, be read by every grain, and
    // poison the loop permanently (output NaN forever).
    void finiteGuardRecoveryTest (double Fs)
    {
        std::printf ("Finite-guard self-recovery @ Fs=%.0f\n", Fs);
        factory_core::GranularDelay g;
        g.prepare (Fs, 2.0);
        g.setGrainSizeMs (40.0);
        g.setDensityHz (25.0);
        g.setDelaySamples (0.1 * Fs);
        g.setFeedback (0.7);
        g.setPositionJitterMs (0.0);
        g.setPitchSemitones (0.0);
        g.setPitchRandomSemis (0.0);
        g.setSpread (0.0);
        g.setMix (1.0);

        const double inf = std::numeric_limits<double>::infinity();
        const int total = (int) (0.5 * Fs); // > delay, so a grain reads the poisoned spot
        std::vector<double> out ((size_t) total);
        for (int n = 0; n < total; ++n)
        {
            double x = (n == 0) ? inf : 0.0; // single Inf into the loop, then silence
            double l = x, r = x;
            g.processStereo (l, r);
            out[(size_t) n] = 0.5 * (l + r);
        }

        // The injection sample itself carries the raw Inf through the dry path
        // (an unavoidable in==out artifact); what the finite guard buys is that
        // the FEEDBACK loop does not stay poisoned. Assert every sample AFTER the
        // injection is finite: without the guard the Inf enters the delay buffer
        // and reappears (as NaN) at output time ~= delaySamples, so this range
        // would fail. A tail band well past the delay confirms recovery.
        const std::vector<double> after (out.begin() + 1, out.end());
        if (! fct::allFinite (after)) { fail ("finite guard: loop stayed poisoned (non-finite after Inf)"); return; }
        // ... and the tail settles back toward silence.
        const int win = (int) (0.02 * Fs);
        const double tail = rms (out, total - win, win);
        if (! (tail < 1.0e-3))
            fail ("finite guard: tail did not decay to silence: rms " + std::to_string (tail));
        std::printf ("  all-finite after Inf, tail rms = %.2e (recovered)\n", tail);
    }
}

int main (int argc, char** argv)
{
    delayLineTest();
    tempoSyncTest();

    const std::vector<double> rates = fct::sampleRatesFromArgs (argc, argv);

    // Shorter invariants: full 6-rate matrix (class G hard rule).
    for (double Fs : rates)
    {
        colaTest (Fs);
        feedbackTest (Fs);
        sparseOverlapDecayTest (Fs);
        mixTest (Fs);
        pitchTransposeTest (Fs);
        pitchExtremeStabilityTest (Fs);
        jitterPitchRandStabilityTest (Fs);
        spreadTest (Fs);
        worstCaseBufferTest (Fs);
        finiteGuardRecoveryTest (Fs);
    }

    // Heavy tens-of-seconds hold (class C) at representative low/mid/high rates
    // to keep total runtime sane while still spanning the matrix. When a single
    // rate is requested (CTest per-rate case), run it for that rate too.
    std::vector<double> longRates;
    if (argc > 1) longRates = rates;
    else          longRates = { 44100.0, 96000.0, 192000.0 };
    for (double Fs : longRates)
        longHoldFiniteTest (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
