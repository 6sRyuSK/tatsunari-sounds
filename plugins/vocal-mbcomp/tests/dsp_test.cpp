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
#include "factory_core/testing/DspInvariants.h"

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
    namespace fct = factory_core::testing;
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

    // Independent static gain-reduction oracle. For a hard-knee compressor the
    // static curve is  out = T + (in - T)/R  above threshold, so the expected
    // steady-state gain reduction (magnitude, dB) is  GR = (L - T)*(1 - 1/R)
    // for L > T, and 0 below. Derived from the SPEC only — never read from the
    // implementation under test.
    double grOracleDb (double L, double T, double R)
    {
        if (L <= T) return 0.0;
        return (L - T) * (1.0 - 1.0 / R);
    }

    void perBandGrOracleTest (double Fs)
    {
        std::printf ("Per-band gain-reduction oracle @ Fs=%.0f\n", Fs);
        // Long settle so the decoupled ballistics reach steady state; the
        // measure window is an integer-ish number of periods for a clean RMS.
        const int settle = (int) (0.5 * Fs), measure = (int) (0.2 * Fs);
        const double fTone = 1000.0; // lives in the mid band

        // Build a multiband comp with a known threshold/ratio on the mid band
        // and identity (ratio 1) elsewhere. Hard knee (kneeDb default 0),
        // no makeup — matching the oracle's static curve.
        auto makeMb = [&] (double thr, double ratio) {
            auto mb = std::make_unique<factory_core::MultibandCompressor>();
            mb->prepare (Fs);
            mb->setCrossover (250.0, 4000.0);
            mb->setMix (1.0);
            for (int i = 0; i < 3; ++i) { mb->band (i).setThresholdDb (0.0); mb->band (i).setRatio (1.0); }
            mb->band (1).setThresholdDb (thr);
            mb->band (1).setRatio (ratio);
            mb->band (1).setKneeDb (0.0);
            mb->band (1).setMakeupDb (0.0);
            // Fast attack + long release so the decoupled ballistics LATCH the
            // gain at the peak-level static reduction: the rectified-sine
            // detector re-affirms the peak every cycle while the slow release
            // barely eases it through the brief troughs, so the applied gain is
            // ~constant at staticGainDb(peak-L). That is what makes the peak
            // static-curve the correct independent oracle here (an audio-rate
            // attack/release would apply a time-varying gain whose RMS average
            // is shallower than the peak reduction, and would NOT match the
            // static curve). This calibrates the measurement to the spec; it
            // does not loosen the ±1 dB tolerance.
            mb->band (1).setAttackMs (0.5);
            mb->band (1).setReleaseMs (1500.0);
            mb->band (1).prepare (Fs);
            return mb;
        };

        // A sine of amplitude A has level 20*log10(A) dBFS at its peak; the
        // compressor detects on the (linked) peak envelope, so the input level
        // driving the static curve is the peak level in dBFS.
        struct Point { double L, T, R; };
        const Point pts[] = {
            { -6.0, -24.0, 4.0 },
            { -6.0, -18.0, 2.0 },
            { -12.0, -30.0, 3.0 },
        };

        for (const auto& p : pts)
        {
            const double amp = dbToLin (p.L);
            // Reference: same band at ratio 1 (no compression) to isolate the
            // crossover's own passband gain from the compression reduction.
            auto comp = makeMb (p.T, p.R);
            auto flat = makeMb (p.T, 1.0);
            const double rComp = toneRms (*comp, Fs, fTone, amp, settle, measure);
            const double rFlat = toneRms (*flat, Fs, fTone, amp, settle, measure);
            const double measuredGr = -(linToDb (rComp) - linToDb (rFlat)); // magnitude
            const double expectedGr = grOracleDb (p.L, p.T, p.R);
            std::printf ("  L=%.0f T=%.0f R=%.1f  expected GR=%.2f dB  measured GR=%.2f dB\n",
                         p.L, p.T, p.R, expectedGr, measuredGr);
            if (std::abs (measuredGr - expectedGr) > 1.0)
                fail ("gain-reduction oracle mismatch: expected " + std::to_string (expectedGr)
                      + " got " + std::to_string (measuredGr) + " dB (L=" + std::to_string (p.L)
                      + " T=" + std::to_string (p.T) + " R=" + std::to_string (p.R) + ")");
        }
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

    // --- Task 1: crossover EXTREMES ------------------------------------------
    // Every other test pins the splits at 250/4000. LR4 reconstruction is
    // structurally flat for ANY split pair (low + mid + high == an allpass
    // cascade), so the same 0.1 dB flatness tolerance must hold at the boundary
    // settings too: the SQUEEZED worst case (low=600 / high=1500, the closest
    // splits the parameter ranges allow — narrowest mid band, most crossover
    // overlap, worst reconstruction error) and the WIDE extremes (80 / 9000).
    // Also re-verify band isolation at the squeezed setting.
    void crossoverExtremesTest (double Fs)
    {
        std::printf ("Crossover extremes reconstruction + squeezed isolation @ Fs=%.0f\n", Fs);
        const int N = 1 << 16;

        auto reconMaxDev = [&] (double lowHz, double highHz) {
            factory_core::Crossover3 xo;
            xo.prepare (Fs);
            xo.setFrequencies (lowHz, highHz);
            std::vector<double> sum ((size_t) N);
            for (int n = 0; n < N; ++n)
            {
                double l, m, h;
                xo.process ((n == 0) ? 1.0 : 0.0, l, m, h);
                sum[(size_t) n] = l + m + h;
            }
            double maxDev = 0.0;
            const int M = 96;
            for (int k = 0; k < M; ++k)
            {
                const double f = 20.0 * std::pow ((0.45 * Fs) / 20.0, (double) k / (M - 1));
                maxDev = std::max (maxDev, std::abs (magDb (dftAt (sum, 2.0 * kPi * f / Fs))));
            }
            return maxDev;
        };

        // Same tolerance discipline as the existing flatness gate (0.1 dB).
        const double sqDev   = reconMaxDev (600.0, 1500.0); // squeezed worst case
        const double wideDev = reconMaxDev (80.0, 9000.0);  // wide extremes
        if (sqDev   > 0.1) fail ("squeezed 600/1500 reconstruction not flat: " + std::to_string (sqDev) + " dB");
        if (wideDev > 0.1) fail ("wide 80/9000 reconstruction not flat: " + std::to_string (wideDev) + " dB");

        // Band isolation still holds at the squeezed setting. The mid band is
        // only ~1.3 octaves wide there, so its passband peak sits a few dB down;
        // assert presence (> -6 dB at the geometric centre) plus real rejection
        // outside, and clean pass/reject for the low and high bands.
        factory_core::Crossover3 xo;
        xo.prepare (Fs);
        xo.setFrequencies (600.0, 1500.0);
        std::vector<double> low ((size_t) N), mid ((size_t) N), high ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            double l, m, h;
            xo.process ((n == 0) ? 1.0 : 0.0, l, m, h);
            low[(size_t) n] = l; mid[(size_t) n] = m; high[(size_t) n] = h;
        }
        auto at = [&] (const std::vector<double>& v, double f) { return magDb (dftAt (v, 2.0 * kPi * f / Fs)); };
        const double midCtr = std::sqrt (600.0 * 1500.0); // ~948 Hz
        if (at (low, 100.0) < -1.0 || at (low, 6000.0) > -10.0)   fail ("squeezed low band isolation");
        if (at (high, 6000.0) < -1.0 || at (high, 200.0) > -10.0) fail ("squeezed high band isolation");
        if (at (mid, midCtr) < -6.0 || at (mid, 100.0) > -10.0 || at (mid, 6000.0) > -10.0) fail ("squeezed mid band isolation");
        std::printf ("  squeezed recon=%.4f  wide recon=%.4f | low@100=%.1f low@6k=%.1f mid@%.0f=%.1f high@6k=%.1f high@200=%.1f\n",
                     sqDev, wideDev, at (low, 100.0), at (low, 6000.0), midCtr, at (mid, midCtr), at (high, 6000.0), at (high, 200.0));
    }

    // --- Task 2: makeup gain oracle ------------------------------------------
    // The core exposes setMakeupDb but every existing test sets it to 0. Two
    // independent oracles, both measured relative to an uncompressed reference
    // band (ratio 1, makeup 0) to cancel the crossover's own passband gain:
    //   (a) EXACT makeup-only: a tone BELOW threshold gets no compression, so
    //       the applied gain is exactly the makeup -> relative level == M dB.
    //   (b) compression + makeup ABOVE threshold: relative level ==
    //       M - (L - T)(1 - 1/R) dB  (spec static curve, ±1 dB as elsewhere).
    void makeupOracleTest (double Fs)
    {
        std::printf ("Makeup gain oracle @ Fs=%.0f\n", Fs);

        // (a) exact makeup-only gain. The tone sits below threshold in EVERY
        // band (ratio irrelevant), and the SAME makeup is applied to all three
        // bands, so the whole flat reconstruction is scaled by exactly the
        // makeup factor -> relative level == M dB, exactly. (Applying makeup to
        // one band only would leave the other bands' crossover leakage of the
        // tone unscaled and blur the result, so makeup must be uniform here.)
        {
            const int settle = (int) (0.3 * Fs), measure = (int) (0.2 * Fs);
            const double fTone = 100.0;             // in-band, but any tone works
            const double amp = dbToLin (-20.0);     // well below the 0 dB threshold
            auto makeMb = [&] (double makeup) {
                auto mb = std::make_unique<factory_core::MultibandCompressor>();
                mb->prepare (Fs); mb->setCrossover (250.0, 4000.0); mb->setMix (1.0);
                for (int i = 0; i < 3; ++i)
                {
                    mb->band (i).setThresholdDb (0.0);   // tone at -20 dB stays below
                    mb->band (i).setRatio (2.0);         // ratio must be irrelevant below thr
                    mb->band (i).setKneeDb (0.0);
                    mb->band (i).setMakeupDb (makeup);
                    mb->band (i).setAttackMs (5.0); mb->band (i).setReleaseMs (80.0);
                    mb->band (i).prepare (Fs);
                }
                return mb;
            };
            for (double M : { 3.0, 6.0, -4.0 })
            {
                auto comp = makeMb (M);
                auto flat = makeMb (0.0);
                const double rC = toneRms (*comp, Fs, fTone, amp, settle, measure);
                const double rF = toneRms (*flat, Fs, fTone, amp, settle, measure);
                const double rel = linToDb (rC) - linToDb (rF);
                if (std::abs (rel - M) > 0.02)
                    fail ("makeup exact-gain oracle mismatch: expected " + std::to_string (M) + " got " + std::to_string (rel) + " dB");
                std::printf ("  exact makeup M=%+.1f dB -> measured %+.3f dB\n", M, rel);
            }
        }

        // (b) compression + makeup above threshold. Latching ballistics (fast
        // attack + long release) pin the applied gain at the peak static
        // reduction, exactly as in perBandGrOracleTest; the ±1 dB tolerance is
        // unchanged.
        {
            const int settle = (int) (0.5 * Fs), measure = (int) (0.2 * Fs);
            const double fTone = 1000.0;            // lives in the mid band
            auto makeMb = [&] (double thr, double ratio, double makeup) {
                auto mb = std::make_unique<factory_core::MultibandCompressor>();
                mb->prepare (Fs); mb->setCrossover (250.0, 4000.0); mb->setMix (1.0);
                for (int i = 0; i < 3; ++i) { mb->band (i).setThresholdDb (0.0); mb->band (i).setRatio (1.0); mb->band (i).setMakeupDb (0.0); }
                mb->band (1).setThresholdDb (thr); mb->band (1).setRatio (ratio);
                mb->band (1).setKneeDb (0.0); mb->band (1).setMakeupDb (makeup);
                mb->band (1).setAttackMs (0.5); mb->band (1).setReleaseMs (1500.0);
                mb->band (1).prepare (Fs);
                return mb;
            };
            struct P { double L, T, R, M; };
            const P pts[] = { { -6.0, -24.0, 4.0, 6.0 }, { -6.0, -18.0, 2.0, 3.0 }, { -12.0, -30.0, 3.0, 8.0 } };
            for (const auto& p : pts)
            {
                const double amp = dbToLin (p.L);
                auto comp = makeMb (p.T, p.R, p.M);
                auto flat = makeMb (p.T, 1.0, 0.0);
                const double rC = toneRms (*comp, Fs, fTone, amp, settle, measure);
                const double rF = toneRms (*flat, Fs, fTone, amp, settle, measure);
                const double rel = linToDb (rC) - linToDb (rF);
                const double expected = p.M - grOracleDb (p.L, p.T, p.R);
                if (std::abs (rel - expected) > 1.0)
                    fail ("makeup+comp oracle mismatch: expected " + std::to_string (expected) + " got " + std::to_string (rel) + " dB");
                std::printf ("  L=%.0f T=%.0f R=%.1f M=%.1f  expected %+.2f dB  measured %+.2f dB\n",
                             p.L, p.T, p.R, p.M, expected, rel);
            }
        }
    }

    // --- Task 3: crossover retune mid-stream (class E/F) ----------------------
    // Retuning the crossover swaps LR4 biquad coeffs in place while the z-state
    // continues; a stale/exploding state (missing propagation or a coeff/state
    // mismatch) would spike or diverge. Stream a tone, retune 250/4000 ->
    // 600/1500 mid-stream, and assert (a) output stays finite and (b) the
    // post-retune sample-to-sample transient stays within a bound chosen with
    // margin over the measured worst case — a genuine blow-up fails it.
    void retuneTest (double Fs)
    {
        std::printf ("Crossover retune mid-stream (class E/F) @ Fs=%.0f\n", Fs);
        const double amp = 0.5, f = 1000.0, w = 2.0 * kPi * f / Fs;
        factory_core::MultibandCompressor mb;
        mb.prepare (Fs); mb.setCrossover (250.0, 4000.0); mb.setMix (1.0);
        for (int i = 0; i < 3; ++i)
        {
            mb.band (i).setThresholdDb (-20.0); mb.band (i).setRatio (4.0);
            mb.band (i).setAttackMs (5.0); mb.band (i).setReleaseMs (80.0); mb.band (i).prepare (Fs);
        }

        const int pre = (int) (0.5 * Fs), post = (int) (0.5 * Fs);
        std::vector<double> y ((size_t) (pre + post));
        for (int n = 0; n < pre; ++n) { double l = amp * std::sin (w * n), r = l; mb.processStereo (l, r); y[(size_t) n] = l; }

        // Steady-state per-sample slew of the settled tone (pre-retune baseline).
        double preMaxDelta = 0.0;
        for (int n = pre / 2 + 1; n < pre; ++n) preMaxDelta = std::max (preMaxDelta, std::abs (y[(size_t) n] - y[(size_t) (n - 1)]));

        mb.setCrossover (600.0, 1500.0); // retune to the squeezed splits mid-stream
        for (int n = 0; n < post; ++n) { double l = amp * std::sin (w * (pre + n)), r = l; mb.processStereo (l, r); y[(size_t) (pre + n)] = l; }

        if (! fct::allFinite (y)) fail ("retune produced non-finite output");

        double postMaxDelta = 0.0;
        const int wtail = (int) (0.05 * Fs);
        for (int n = pre; n < pre + wtail; ++n) postMaxDelta = std::max (postMaxDelta, std::abs (y[(size_t) n] - y[(size_t) (n - 1)]));
        const double peak = fct::peakAbs (y);

        // Bound: measured post-retune transient is ~0.10-0.11 at all rates (tone
        // slew ~0.07 + a small coeff-swap step); a stale/exploding z-state would
        // be orders of magnitude larger. 0.35 (= 0.7 * amp) gives ~3x margin and
        // still fails any genuine blow-up.
        const double bound = 0.35;
        if (postMaxDelta > bound) fail ("retune transient too large: " + std::to_string (postMaxDelta) + " (bound " + std::to_string (bound) + ")");
        std::printf ("  preDelta=%.4f postDelta=%.4f peak=%.4f (bound %.2f)\n", preMaxDelta, postMaxDelta, peak, bound);
    }

    // --- Task 3 (cont): prepare/reset clears state ----------------------------
    // Drive one instance into deep gain reduction + non-zero LR4 state, reset(),
    // then feed it and a FRESH instance the same burst: reset() must clear both
    // the compressor ballistics and the crossover z-state, so the outputs match
    // sample-for-sample.
    void resetTest (double Fs)
    {
        std::printf ("prepare/reset clears state @ Fs=%.0f\n", Fs);
        auto setup = [&] (factory_core::MultibandCompressor& mb) {
            mb.prepare (Fs); mb.setCrossover (250.0, 4000.0); mb.setMix (1.0);
            for (int i = 0; i < 3; ++i)
            {
                mb.band (i).setThresholdDb (-40.0); mb.band (i).setRatio (8.0);
                mb.band (i).setAttackMs (1.0); mb.band (i).setReleaseMs (120.0); mb.band (i).prepare (Fs);
            }
        };
        factory_core::MultibandCompressor driven, fresh;
        setup (driven); setup (fresh);

        const double wd = 2.0 * kPi * 900.0 / Fs; // build up deep GR + LR4 state
        for (int n = 0; n < (int) (0.5 * Fs); ++n) { double l = 0.9 * std::sin (wd * n), r = l; driven.processStereo (l, r); }
        driven.reset();

        const double wt = 2.0 * kPi * 500.0 / Fs;
        double maxDiff = 0.0;
        for (int n = 0; n < (int) (0.1 * Fs); ++n)
        {
            double la = 0.3 * std::sin (wt * n), ra = la; driven.processStereo (la, ra);
            double lb = 0.3 * std::sin (wt * n), rb = lb; fresh.processStereo (lb, rb);
            maxDiff = std::max (maxDiff, std::abs (la - lb));
        }
        if (maxDiff > 1e-9) fail ("reset did not restore fresh state: maxDiff=" + std::to_string (maxDiff));
        std::printf ("  maxDiff (reset vs fresh) = %.2e\n", maxDiff);
    }

    // --- Task 4: silence / very-low tone -> no phantom GR (class J) -----------
    // Aggressive settings in ALL bands; digital silence and a -90 dBFS tone must
    // both leave every band's gain reduction at ~0 (the level is far below the
    // threshold floor, so a healthy detector never engages).
    void silenceTest (double Fs)
    {
        std::printf ("Silence / -90 dBFS tone -> no phantom GR (class J) @ Fs=%.0f\n", Fs);
        auto makeMb = [&] () {
            auto mb = std::make_unique<factory_core::MultibandCompressor>();
            mb->prepare (Fs); mb->setCrossover (250.0, 4000.0); mb->setMix (1.0);
            for (int i = 0; i < 3; ++i)
            {
                mb->band (i).setThresholdDb (-32.0); mb->band (i).setRatio (4.0); // aggressive macro extreme
                mb->band (i).setAttackMs (1.0); mb->band (i).setReleaseMs (100.0); mb->band (i).setMakeupDb (0.0);
                mb->band (i).prepare (Fs);
            }
            return mb;
        };
        // (a) digital silence
        {
            auto mb = makeMb();
            for (int n = 0; n < (int) (0.3 * Fs); ++n) { double l = 0.0, r = 0.0; mb->processStereo (l, r); }
            for (int b = 0; b < 3; ++b)
                if (mb->bandGainReductionDb (b) < -1e-6) fail ("phantom GR on digital silence, band " + std::to_string (b));
        }
        // (b) very low tone at -90 dBFS
        {
            auto mb = makeMb();
            const double amp = dbToLin (-90.0), w = 2.0 * kPi * 1000.0 / Fs;
            for (int n = 0; n < (int) (0.3 * Fs); ++n) { double l = amp * std::sin (w * n), r = l; mb->processStereo (l, r); }
            for (int b = 0; b < 3; ++b)
                if (mb->bandGainReductionDb (b) < -1e-6) fail ("phantom GR on -90 dBFS tone, band " + std::to_string (b));
            std::printf ("  GR low=%.2e mid=%.2e high=%.2e dB\n",
                         mb->bandGainReductionDb (0), mb->bandGainReductionDb (1), mb->bandGainReductionDb (2));
        }
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        crossoverTest (Fs);
        crossoverExtremesTest (Fs);
        transparentTest (Fs);
        perBandTest (Fs);
        perBandGrOracleTest (Fs);
        makeupOracleTest (Fs);
        retuneTest (Fs);
        resetTest (Fs);
        silenceTest (Fs);
        mixTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
