//
// dsp_test.cpp — headless verification of the Fuzznari DSP core
// (factory_core::FuzzEngine), run through the full production path including
// the RateBracket<PolyphaseResampler> oversampling. The gates:
//
//   1.  Static transfer invariants against the analytic form
//       f(x) = tanh(drive·x + b) − tanh(b): f(0)=0, monotonicity, the
//       1+tanh(|b|) bound, odd symmetry iff b=0, slope@0 = drive·sech²(b).
//   2.  Harmonic structure through the streaming engine: bias=0 → even
//       harmonics ~0 and THD monotone in drive; bias≠0 → the 2nd harmonic is
//       PRESENT and matches a separate-code-path oracle (shapeStatic applied
//       to an ideal sine).
//   3.  Gate: quantitative small-signal attenuation against the analytic
//       sech²(b_gate) prediction, and digital silence in → silence out at
//       every gate/drive setting (no phantom output — regression class J).
//   4.  Sputter: a decaying note at worst-case gate terminates below the
//       silence floor without ever going non-finite.
//   5.  Self-oscillation (osc = ON, the user-approved feedback exception):
//       zero input at stab=1 stays finite and inside the tanh amplitude bound
//       over a long hold, actually oscillates, sits at a plausible pitch, the
//       pitch tracks stab (delay length), and gate quenches the squeal.
//   6.  Non-oscillating stability: with osc OFF the standard
//       impulseResponseNonIncreasing invariant holds over the worst-case
//       stab/gate/bias grid (loop gain < 1 unconditionally); with osc ON it
//       still holds below onset.
//   7.  reset() fully clears state: a squeal cannot survive reset, and two
//       runs separated by reset() are bit-identical (class E).
//   8.  Alias gate: a high tone at full drive produces non-harmonic spectral
//       content below a fixed ceiling — this is the oversampler's regression
//       lock (measured, not eyeballed; loosening it is "Ask a human").
//   9.  DC safety: biased output carries no DC after the blocker, and a full
//       bias step during silence produces no thump.
//   10. Latency truth: the reported latencySamples() matches the measured
//       impulse arrival at every rate.
//   11. Bypass (regression class E): engaging bypass while the worst-case
//       squeal grid is self-oscillating fades the output to the silence
//       floor within the documented kBypassFadeSec time constants, the edge
//       is click-free (bounded sample-to-sample delta vs the squeal's own
//       steady-state delta), and un-bypassing restarts the loop clean
//       (clearedWhileBypassed) instead of snapping back to the established
//       squeal amplitude — checked against a genuinely fresh engine too.
//   12. Tone tilt: a two-tone (low/high, both far from the 800 Hz corner)
//       integer-bin measurement shows the hi/lo energy ratio moves in the
//       documented direction (tone>0 cuts low-shelf/boosts high-shelf) with
//       a meaningful (not just nonzero) swing between the extremes.
//   13. Level: an exact linear-gain oracle — level is applied purely as a
//       post-nonlinearity output multiply (never fed back into the loop), so
//       the whole waveform must scale by the level ratio, pointwise.
//   14. Mix: mix=0.5 equals 0.5*dry + 0.5*wet sample-for-sample. dry and wet
//       are combined at the same model-rate index inside processChannel
//       before the shared RateBracket downsampling, so they are inherently
//       latency-aligned — no manual delay compensation is needed.
//
// Sample-rate dependence: the tones are fixed absolute frequencies and the
// analysis grids are derived from Fs, the engine's internal model rate is
// derived from Fs (4x/2x/1x), and the alias/latency gates change value with
// Fs — so the rate loop asserts genuinely rate-dependent behaviour.
//
#include "factory_core/FFT.h"
#include "factory_core/FuzzEngine.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    using factory_core::FuzzEngine;
    namespace fct = factory_core::testing;

    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    double sech2 (double x) { const double c = std::cosh (x); return 1.0 / (c * c); }
    double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1e-300)); }

    // Direct DFT bin via phase recurrence: sum x[n] e^{-j w n}.
    cd dftAt (const std::vector<double>& x, double w)
    {
        const cd step = std::exp (cd (0.0, -w));
        cd zk (1.0, 0.0);
        cd acc (0.0, 0.0);
        for (const double xn : x) { acc += xn * zk; zk *= step; }
        return acc;
    }

    struct Params
    {
        double driveDb = 24.0, bias = 0.0, gate = 0.0, stab = 0.0;
        double tone = 0.0, levelDb = 0.0, mix = 1.0;
        bool   osc = false;
    };

    void applyParams (FuzzEngine& e, const Params& p)
    {
        e.setDriveDb (p.driveDb);
        e.setBias (p.bias);
        e.setGate (p.gate);
        e.setOscEnabled (p.osc);
        e.setStab (p.stab);
        e.setTone (p.tone);
        e.setLevelDb (p.levelDb);
        e.setMix (p.mix);
    }

    // Run a mono signal through a prepared engine (fed to both channels, left
    // channel returned), in fixed-size blocks like a host would.
    std::vector<double> processThrough (FuzzEngine& e, const std::vector<double>& input,
                                        int blockSize = 256)
    {
        std::vector<double> out (input.size());
        std::vector<float> inF ((size_t) blockSize), outL ((size_t) blockSize), outR ((size_t) blockSize);

        size_t pos = 0;
        while (pos < input.size())
        {
            const int n = (int) std::min ((size_t) blockSize, input.size() - pos);
            for (int i = 0; i < n; ++i)
                inF[(size_t) i] = (float) input[pos + (size_t) i];
            e.process (inF.data(), inF.data(), outL.data(), outR.data(), n);
            for (int i = 0; i < n; ++i)
                out[pos + (size_t) i] = (double) outL[(size_t) i];
            pos += (size_t) n;
        }
        return out;
    }

    std::vector<double> runEngine (const Params& p, double Fs, const std::vector<double>& input,
                                   int blockSize = 256)
    {
        FuzzEngine e;
        applyParams (e, p);
        e.prepare (Fs, blockSize);
        return processThrough (e, input, blockSize);
    }

    std::vector<double> sineSignal (double Fs, double freqHz, double amp, int numSamples)
    {
        std::vector<double> x ((size_t) numSamples);
        const double w = 2.0 * kPi * freqHz / Fs;
        for (int n = 0; n < numSamples; ++n)
            x[(size_t) n] = amp * std::sin (w * n);
        return x;
    }

    double rmsOf (const std::vector<double>& x, size_t start, size_t len)
    {
        return std::sqrt (fct::windowEnergy (x, start, len) / (double) len);
    }

    std::vector<double> tail (const std::vector<double>& x, size_t len)
    {
        return { x.end() - (long) std::min (len, x.size()), x.end() };
    }

    // Dominant frequency (Hz) of the last `1 << order` samples, via the shared FFT.
    double dominantFrequency (const std::vector<double>& x, double Fs, int order = 15)
    {
        factory_core::FFT fft;
        fft.prepare (order);
        const int N = fft.size();
        if ((int) x.size() < N)
            return 0.0;

        std::vector<cd> buf ((size_t) N);
        const size_t off = x.size() - (size_t) N;
        for (int i = 0; i < N; ++i)
        {
            // Hann window: the oscillation is not bin-aligned.
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / (N - 1.0));
            buf[(size_t) i] = cd (x[off + (size_t) i] * w, 0.0);
        }
        fft.forward (buf.data());

        int    bestBin = 1;
        double bestMag = 0.0;
        for (int j = 1; j < N / 2; ++j)
        {
            const double mag = std::abs (buf[(size_t) j]);
            if (mag > bestMag) { bestMag = mag; bestBin = j; }
        }
        return bestBin * Fs / N;
    }

    // ---- 1. Static transfer invariants --------------------------------------
    void staticTransferInvariants()
    {
        std::printf ("Static transfer invariants\n");
        const double drives[] = { 1.0, 4.0, 15.8489, 251.189 };      // 0/12/24/48 dB
        const double biases[] = { -1.2, -0.6, 0.0, 0.6, 1.2 };       // b_eff range

        for (double d : drives)
            for (double b : biases)
            {
                const std::string tag = "d=" + std::to_string (d) + " b=" + std::to_string (b);

                if (std::abs (FuzzEngine::shapeStatic (0.0, d, b)) > 1e-14)
                    fail (tag + ": f(0) != 0");

                const double bound = 1.0 + std::tanh (std::abs (b)) + 1e-12;
                double prev = FuzzEngine::shapeStatic (-4.0, d, b);
                double maxAsym = 0.0;
                for (int i = -80; i <= 80; ++i)
                {
                    const double x  = i * 0.05;
                    const double fp = FuzzEngine::shapeStatic (x, d, b);
                    const double fn = FuzzEngine::shapeStatic (-x, d, b);
                    if (i > -80 && fp < prev - 1e-15)
                        fail (tag + ": not monotonic at x=" + std::to_string (x));
                    if (std::abs (fp) > bound)
                        fail (tag + ": exceeds 1+tanh(|b|) bound at x=" + std::to_string (x));
                    if (b == 0.0 && std::abs (fp + fn) > 1e-12)
                        fail (tag + ": b=0 not odd-symmetric at x=" + std::to_string (x));
                    maxAsym = std::max (maxAsym, std::abs (fp + fn));
                    prev = fp;
                }
                if (b != 0.0 && d >= 4.0 && maxAsym < 0.01)
                    fail (tag + ": bias produced no asymmetry");

                // Slope at the origin: analytic oracle drive·sech²(b).
                const double eps = 1e-6;
                const double slope = (FuzzEngine::shapeStatic (eps, d, b)
                                      - FuzzEngine::shapeStatic (-eps, d, b)) / (2.0 * eps);
                const double expected = d * sech2 (b);
                if (std::abs (slope - expected) > 1e-4 * std::max (1.0, expected))
                    fail (tag + ": slope@0 " + std::to_string (slope) + " != " + std::to_string (expected));

                // Asymmetry direction: the biased side flattens first.
                if (b != 0.0 && d >= 1.0)
                {
                    const double s = FuzzEngine::shapeStatic (1.0, d, b) + FuzzEngine::shapeStatic (-1.0, d, b);
                    if (s * b >= 0.0)
                        fail (tag + ": asymmetry sign wrong (f(1)+f(-1)=" + std::to_string (s) + ")");
                }
            }
        std::printf ("  done (%d combos)\n", (int) (4 * 5));
    }

    // ---- 2. Harmonic structure through the full engine ----------------------
    void harmonicTests (double Fs)
    {
        std::printf ("Harmonic structure @ Fs=%.0f\n", Fs);

        const double f0Hz     = 600.0;
        const double duration = 16384.0 / 48000.0;                // analysis window (s)
        const int    N        = (int) std::llround (duration * Fs);
        const int    cycles   = (int) std::llround (f0Hz * N / Fs); // integer bin
        const double f0       = cycles * Fs / N;
        const double w0       = 2.0 * kPi * cycles / N;
        const int    warmup   = (int) std::llround (0.3 * Fs);    // smoothers + resampler + envelope settle
        const double amp      = 0.8;

        auto engineTail = [&] (const Params& p)
        {
            const auto in  = sineSignal (Fs, f0, amp, warmup + N);
            const auto out = runEngine (p, Fs, in);
            return tail (out, (size_t) N);
        };

        // (a) bias = 0: odd-symmetric — even harmonics stay at the floor, THD
        // grows with drive. Harmonics measured only in the resampler passband
        // (< 0.9 Nyquist) so the downsampler's transition band cannot skew them.
        double prevThd = -1.0;
        for (double driveDb : { 12.0, 24.0, 36.0 })
        {
            Params p;
            p.driveDb = driveDb;
            const auto y    = engineTail (p);
            const double fund = std::abs (dftAt (y, w0));

            for (int k = 2; k * f0 < 0.9 * 0.5 * Fs && k <= 12; k += 2)
            {
                const double mag = std::abs (dftAt (y, w0 * k));
                if (db (mag / fund) > -60.0)
                    fail ("Fs=" + std::to_string (Fs) + " drive=" + std::to_string (driveDb)
                          + ": even harmonic k=" + std::to_string (k) + " at "
                          + std::to_string (db (mag / fund)) + " dB (bias=0)");
            }

            double sumSq = 0.0;
            for (int k = 3; k * f0 < 0.9 * 0.5 * Fs; k += 2)
            {
                const double mag = std::abs (dftAt (y, w0 * k));
                sumSq += mag * mag;
            }
            const double thd = std::sqrt (sumSq) / fund;
            if (prevThd >= 0.0 && thd <= prevThd)
                fail ("Fs=" + std::to_string (Fs) + " drive=" + std::to_string (driveDb)
                      + ": THD not increasing with drive");
            std::printf ("  drive=%.0f dB  THD=%.4f\n", driveDb, thd);
            prevThd = thd;
        }

        // (b) bias = 0.5: even harmonics PRESENT and matching the independent
        // oracle — shapeStatic applied to an ideal sine, a separate code path
        // from the streaming engine (no resampler, no filters, no envelope).
        {
            Params p;
            p.driveDb = 24.0;
            p.bias    = 0.5;
            const auto y      = engineTail (p);
            const double fund = std::abs (dftAt (y, w0));
            const double h2   = std::abs (dftAt (y, w0 * 2.0));
            const double h2Db = db (h2 / fund);

            std::vector<double> ref ((size_t) N);
            const double driveLin = std::pow (10.0, p.driveDb / 20.0);
            const double bEff     = FuzzEngine::kBiasScale * p.bias;
            for (int n = 0; n < N; ++n)
                ref[(size_t) n] = FuzzEngine::shapeStatic (amp * std::sin (w0 * n), driveLin, bEff);
            const double refDb = db (std::abs (dftAt (ref, w0 * 2.0)) / std::abs (dftAt (ref, w0)));

            if (h2Db < -35.0)
                fail ("Fs=" + std::to_string (Fs) + ": bias=0.5 2nd harmonic missing (" + std::to_string (h2Db) + " dB)");
            if (std::abs (h2Db - refDb) > 3.0)
                fail ("Fs=" + std::to_string (Fs) + ": 2nd harmonic " + std::to_string (h2Db)
                      + " dB off oracle " + std::to_string (refDb) + " dB");
            std::printf ("  bias=0.5  H2=%.1f dB (oracle %.1f dB)\n", h2Db, refDb);
        }
    }

    // ---- 3. Gate: quantitative attenuation + no phantom output --------------
    void gateTests (double Fs)
    {
        std::printf ("Gate @ Fs=%.0f\n", Fs);

        // Small-signal attenuation against the analytic sech² oracle. The tone
        // sits far below the knee so the envelope stays ≈ amp and the operating
        // point is b_gate = gate·kGateDepth·kKnee/(amp + kKnee).
        {
            const double amp = 0.003;
            const int    len = (int) std::llround (1.0 * Fs);
            const auto   in  = sineSignal (Fs, 200.0, amp, len);

            Params p;
            p.driveDb = 24.0;
            const auto quiet  = runEngine (p, Fs, in);
            p.gate = 0.8;
            const auto gated  = runEngine (p, Fs, in);

            const size_t win = (size_t) std::llround (0.3 * Fs);
            const double ratio = rmsOf (gated, gated.size() - win, win)
                               / rmsOf (quiet, quiet.size() - win, win);
            const double bPred = p.gate * FuzzEngine::kGateDepth * FuzzEngine::kGateKnee
                               / (amp + FuzzEngine::kGateKnee);
            const double predicted = sech2 (bPred); // vs sech2(0) = 1 at gate=0
            if (std::abs (db (ratio) - db (predicted)) > 6.0)
                fail ("Fs=" + std::to_string (Fs) + ": gate attenuation " + std::to_string (db (ratio))
                      + " dB off analytic " + std::to_string (db (predicted)) + " dB");
            std::printf ("  gate=0.8 attenuation %.1f dB (analytic %.1f dB)\n", db (ratio), db (predicted));
        }

        // Digital silence in → silence out, at every gate/drive corner
        // (regression class J: detectors must have an absolute floor).
        {
            const std::vector<double> silence ((size_t) std::llround (1.0 * Fs), 0.0);
            for (double gate : { 0.0, 0.5, 1.0 })
                for (double driveDb : { 24.0, 48.0 })
                {
                    Params p;
                    p.driveDb = driveDb;
                    p.gate    = gate;
                    const auto out = runEngine (p, Fs, silence);
                    if (fct::peakAbs (out) > 1e-5)
                        fail ("Fs=" + std::to_string (Fs) + " gate=" + std::to_string (gate)
                              + " drive=" + std::to_string (driveDb) + ": phantom output "
                              + std::to_string (fct::peakAbs (out)));
                }
        }
    }

    // ---- 4. Sputter path: a decaying note terminates cleanly ----------------
    void sputterTest (double Fs)
    {
        std::printf ("Sputter @ Fs=%.0f\n", Fs);

        const int decayLen   = (int) std::llround (2.0 * Fs);
        const int silenceLen = (int) std::llround (0.5 * Fs);
        std::vector<double> in ((size_t) (decayLen + silenceLen), 0.0);
        const double w = 2.0 * kPi * 200.0 / Fs;
        for (int n = 0; n < decayLen; ++n)
        {
            const double env = std::pow (10.0, -80.0 / 20.0 * n / (double) decayLen); // 0 → −80 dB
            in[(size_t) n] = env * std::sin (w * n);
        }

        Params p;
        p.driveDb = 36.0;
        p.gate    = 0.9;
        const auto out = runEngine (p, Fs, in);

        if (! fct::allFinite (out))
            fail ("Fs=" + std::to_string (Fs) + ": sputter output not finite");

        const size_t attackWin = (size_t) std::llround (0.5 * Fs);
        if (fct::peakAbs (std::vector<double> (out.begin(), out.begin() + (long) attackWin)) < 0.1)
            fail ("Fs=" + std::to_string (Fs) + ": attack did not pass the gate");

        const auto end = tail (out, (size_t) std::llround (0.25 * Fs));
        if (fct::peakAbs (end) > 1e-5)
            fail ("Fs=" + std::to_string (Fs) + ": note did not terminate (tail peak "
                  + std::to_string (fct::peakAbs (end)) + ")");
    }

    // ---- 5. Self-oscillation (osc = ON): bounded, present, tuned ------------
    void oscillationTests (double Fs)
    {
        std::printf ("Self-oscillation @ Fs=%.0f\n", Fs);

        auto runSilence = [&] (double stab, double gate, double seconds)
        {
            Params p;
            p.driveDb = 36.0;
            p.stab    = stab;
            p.gate    = gate;
            p.osc     = true;
            const std::vector<double> silence ((size_t) std::llround (seconds * Fs), 0.0);
            return runEngine (p, Fs, silence);
        };

        // Long hold at the worst case: finite, inside the tanh bound, and
        // actually oscillating (the feature is tested, not just survived).
        const auto hold = runSilence (1.0, 0.0, 5.0);
        if (! fct::allFinite (hold))
            fail ("Fs=" + std::to_string (Fs) + ": oscillation went non-finite");
        if (fct::peakAbs (hold) > 2.05)
            fail ("Fs=" + std::to_string (Fs) + ": oscillation exceeded tanh bound ("
                  + std::to_string (fct::peakAbs (hold)) + ")");
        const size_t lastSec = (size_t) std::llround (1.0 * Fs);
        if (rmsOf (hold, hold.size() - lastSec, lastSec) < 0.05)
            fail ("Fs=" + std::to_string (Fs) + ": no self-oscillation at stab=1");

        const double fMax = dominantFrequency (hold, Fs);
        if (fMax < 80.0 || fMax > 10000.0)
            fail ("Fs=" + std::to_string (Fs) + ": squeal pitch " + std::to_string (fMax)
                  + " Hz outside [80, 10000]");

        // Pitch tracks stab through the feedback delay length. The loop picks
        // whichever mode has the highest gain, so the pitch-vs-delay relation
        // is not monotone (mode hops) — the invariant is that the pitch moves
        // audibly (≥5%) when stab moves, not its direction.
        const auto shorter = runSilence (0.7, 0.0, 2.5);
        const double fShort = dominantFrequency (shorter, Fs);
        if (std::abs (fShort - fMax) < 0.05 * fMax)
            fail ("Fs=" + std::to_string (Fs) + ": pitch does not track stab (stab=0.7 → "
                  + std::to_string (fShort) + " Hz vs stab=1 → " + std::to_string (fMax) + " Hz)");

        // Gate interaction, both directions: a small gate re-tunes the loop LP
        // (pitch moves down); a large gate starves the loop gain via sech²
        // (squeal quenched entirely) — the Fuzz Factory interplay.
        const auto detuned = runSilence (1.0, 0.15, 2.5);
        const double fDetuned = dominantFrequency (detuned, Fs);
        if (rmsOf (detuned, detuned.size() - lastSec, lastSec) < 0.05)
            fail ("Fs=" + std::to_string (Fs) + ": gate=0.15 should still oscillate");
        else if (fDetuned >= fMax)
            fail ("Fs=" + std::to_string (Fs) + ": gate=0.15 did not lower the squeal pitch ("
                  + std::to_string (fDetuned) + " Hz vs " + std::to_string (fMax) + " Hz)");

        const auto quenched = runSilence (1.0, 0.6, 2.0);
        if (rmsOf (quenched, quenched.size() - lastSec, lastSec) > 1e-4)
            fail ("Fs=" + std::to_string (Fs) + ": gate=0.6 failed to quench the squeal");

        std::printf ("  stab=1: %.0f Hz  stab=0.7: %.0f Hz  gate=0.15: %.0f Hz  peak=%.3f\n",
                     fMax, fShort, fDetuned, fct::peakAbs (hold));
    }

    // ---- 6. Non-oscillating stability: the repo feedback invariant ----------
    void stabilityTests (double Fs)
    {
        std::printf ("Stability (impulse response non-increasing) @ Fs=%.0f\n", Fs);

        auto checkStable = [&] (const Params& p, const std::string& tag)
        {
            FuzzEngine e;
            applyParams (e, p);
            e.prepare (Fs, 64);

            auto proc = [&e] (double x)
            {
                float in = (float) x, l = 0.0f, r = 0.0f;
                e.process (&in, &in, &l, &r, 1);
                return (double) l;
            };
            if (! fct::impulseResponseNonIncreasing (proc, Fs, 2.0, 0.25, 1.05))
                fail ("Fs=" + std::to_string (Fs) + " " + tag + ": impulse response increasing");
        };

        // osc OFF must be unconditionally stable — worst-case corners.
        for (double stab : { 0.0, 1.0 })
            for (double gate : { 0.0, 1.0 })
                for (double bias : { -1.0, 0.0, 1.0 })
                {
                    Params p;
                    p.driveDb = 48.0;
                    p.stab = stab; p.gate = gate; p.bias = bias;
                    checkStable (p, "osc=off stab=" + std::to_string (stab) + " gate="
                                 + std::to_string (gate) + " bias=" + std::to_string (bias));
                }

        // osc ON below onset is still non-oscillating.
        {
            Params p;
            p.driveDb = 48.0;
            p.stab = 0.3;
            p.osc  = true;
            checkStable (p, "osc=on stab=0.3");
        }

        // The toggle's guarantee itself: osc OFF, worst-case stab, zero input,
        // long hold → below the silence floor.
        {
            Params p;
            p.driveDb = 48.0;
            p.stab = 1.0; p.gate = 0.0;
            const std::vector<double> silence ((size_t) std::llround (3.0 * Fs), 0.0);
            const auto out = runEngine (p, Fs, silence);
            if (fct::peakAbs (out) > 1e-5)
                fail ("Fs=" + std::to_string (Fs) + ": osc=off self-oscillated (peak "
                      + std::to_string (fct::peakAbs (out)) + ")");
        }
    }

    // ---- 7. reset() completeness ---------------------------------------------
    void resetTests (double Fs)
    {
        std::printf ("Reset @ Fs=%.0f\n", Fs);

        // A squeal cannot survive reset + stab=0.
        {
            FuzzEngine e;
            Params p;
            p.driveDb = 36.0; p.stab = 1.0; p.osc = true;
            applyParams (e, p);
            e.prepare (Fs, 256);

            const std::vector<double> silence ((size_t) std::llround (1.0 * Fs), 0.0);
            const auto squeal = processThrough (e, silence);
            if (fct::peakAbs (squeal) < 0.05)
                fail ("Fs=" + std::to_string (Fs) + ": setup squeal did not start");

            e.setStab (0.0);
            e.reset();
            const auto after = processThrough (e, std::vector<double> ((size_t) std::llround (0.3 * Fs), 0.0));
            if (fct::peakAbs (after) > 1e-5)
                fail ("Fs=" + std::to_string (Fs) + ": squeal survived reset (peak "
                      + std::to_string (fct::peakAbs (after)) + ")");
        }

        // Determinism: two runs separated by reset() are bit-identical.
        {
            FuzzEngine e;
            Params p;
            p.driveDb = 36.0; p.stab = 0.9; p.gate = 0.3; p.bias = 0.4; p.osc = true;
            applyParams (e, p);
            e.prepare (Fs, 256);

            const int len = (int) std::llround (0.5 * Fs);
            std::vector<double> in ((size_t) len);
            for (int n = 0; n < len; ++n)
                in[(size_t) n] = 0.5 * std::sin (2.0 * kPi * 220.0 * n / Fs)
                               + 0.2 * std::sin (2.0 * kPi * 887.0 * n / Fs);

            const auto runA = processThrough (e, in);
            e.reset();
            const auto runB = processThrough (e, in);
            for (size_t i = 0; i < runA.size(); ++i)
                if (runA[i] != runB[i])
                {
                    fail ("Fs=" + std::to_string (Fs) + ": reset() not deterministic at sample "
                          + std::to_string (i));
                    break;
                }
        }
    }

    // ---- 8. Alias gate: the oversampler's regression lock -------------------
    //
    // Calibrated against the built engine (2026-07, and re-verified with the
    // bracket disabled — "test-the-test"): with the ≥176.4 kHz internal rate
    // the worst audible-band (< 20 kHz) non-harmonic bin measures −70 dB at
    // drive 24 dB and −36 dB at drive 48 dB, at EVERY host rate (the point of
    // an absolute internal rate). With oversampling broken (model == host at
    // 44.1 k) the same measurements collapse to −21 dB / −19 dB. Hence two
    // tiers, both with real margin:
    //   - drive 24 dB (hot but typical): ≤ −60 dB — trips by ~40 dB if the
    //     bracket dies, the regression lock proper.
    //   - drive 48 dB (worst case, essentially a square wave): ≤ −30 dB — the
    //     residue is the model rate's own folding of the 1/k harmonic series,
    //     physics no 4x oversampler removes (an ADAA slot is marked in the
    //     engine if this ever needs tightening). Loosening either tier is
    //     "Ask a human".
    void aliasTests (double Fs)
    {
        std::printf ("Alias gate @ Fs=%.0f\n", Fs);

        factory_core::FFT fft;
        fft.prepare (15);
        const int M = fft.size();

        // Force an ODD cycle count: an even one can make f0 divide Fs exactly
        // (3000 Hz at 48 kHz), which parks every folded alias precisely on a
        // harmonic bin and blinds the scan. Odd cycles are coprime with the
        // 2^15 grid, so aliases land on distinctly non-harmonic bins.
        const int    cycles = ((int) std::llround (3000.0 * M / Fs)) | 1;
        const double f0     = cycles * Fs / M;
        const int    warmup = (int) std::llround (0.3 * Fs);

        auto worstAudibleAliasDb = [&] (double driveDb, double& worstHz)
        {
            Params p;
            p.driveDb = driveDb;
            p.bias    = 0.3;
            const auto in  = sineSignal (Fs, f0, 0.8, warmup + M);
            const auto out = runEngine (p, Fs, in);

            std::vector<cd> buf ((size_t) M);
            const size_t off = out.size() - (size_t) M;
            for (int i = 0; i < M; ++i)
                buf[(size_t) i] = cd (out[off + (size_t) i], 0.0); // integer bin: no window needed
            fft.forward (buf.data());

            const double fund   = std::abs (buf[(size_t) cycles]);
            const int    minBin = (int) std::ceil (100.0 * M / Fs); // skip DC-blocker/envelope LF residue
            const int    maxBin = std::min (M / 2 - 1, (int) std::floor (20000.0 * M / Fs));

            double worst = 0.0;
            int    worstBin = 0;
            for (int j = minBin; j <= maxBin; ++j)
            {
                // Skip bins within ±1 of a true harmonic k·f0.
                const int r = j % cycles;
                if (r <= 1 || r >= cycles - 1)
                    continue;
                const double mag = std::abs (buf[(size_t) j]);
                if (mag > worst) { worst = mag; worstBin = j; }
            }
            worstHz = worstBin * Fs / M;
            return db (worst / fund);
        };

        double hzTypical = 0.0, hzWorst = 0.0;
        const double typicalDb = worstAudibleAliasDb (24.0, hzTypical);
        const double worstDb   = worstAudibleAliasDb (48.0, hzWorst);

        if (typicalDb > -60.0)
            fail ("Fs=" + std::to_string (Fs) + ": audible alias floor " + std::to_string (typicalDb)
                  + " dB at drive 24 dB (" + std::to_string (hzTypical) + " Hz, gate: -60 dB)");
        if (worstDb > -30.0)
            fail ("Fs=" + std::to_string (Fs) + ": audible alias floor " + std::to_string (worstDb)
                  + " dB at drive 48 dB (" + std::to_string (hzWorst) + " Hz, gate: -30 dB)");

        std::printf ("  audible alias: drive 24 dB → %.1f dB @ %.0f Hz   drive 48 dB → %.1f dB @ %.0f Hz\n",
                     typicalDb, hzTypical, worstDb, hzWorst);
    }

    // ---- 9. DC safety ---------------------------------------------------------
    void dcTests (double Fs)
    {
        std::printf ("DC safety @ Fs=%.0f\n", Fs);

        // (a) A hard-biased steady tone carries no DC after the blocker.
        {
            Params p;
            p.driveDb = 24.0;
            p.bias    = 0.8;
            const int len = (int) std::llround (1.0 * Fs);
            const auto out = runEngine (p, Fs, sineSignal (Fs, 600.0, 0.8, len));

            const size_t win = (size_t) std::llround (0.5 * Fs);
            double mean = 0.0;
            for (size_t i = out.size() - win; i < out.size(); ++i)
                mean += out[i];
            mean /= (double) win;
            if (std::abs (mean) > 1e-3)
                fail ("Fs=" + std::to_string (Fs) + ": DC offset " + std::to_string (mean)
                      + " after blocker");
        }

        // (b) A full bias step during silence produces no thump: the smoother
        // plus the −tanh(b_eff) subtraction plus the DC blocker absorb it.
        {
            FuzzEngine e;
            Params p;
            p.driveDb = 48.0;
            applyParams (e, p);
            e.prepare (Fs, 256);

            processThrough (e, std::vector<double> ((size_t) std::llround (0.2 * Fs), 0.0));
            e.setBias (1.0);
            const auto after = processThrough (e, std::vector<double> ((size_t) std::llround (0.3 * Fs), 0.0));
            if (fct::peakAbs (after) > 0.02)
                fail ("Fs=" + std::to_string (Fs) + ": bias step thump " + std::to_string (fct::peakAbs (after)));
        }
    }

    // ---- 10. Latency truth ----------------------------------------------------
    void latencyTests (double Fs)
    {
        std::printf ("Latency @ Fs=%.0f\n", Fs);

        FuzzEngine e;
        Params p;
        p.mix = 0.0; // pure (bracket-resampled) dry: a clean impulse
        applyParams (e, p);
        e.prepare (Fs, 256);
        const int reported = e.latencySamples();

        std::vector<double> in ((size_t) (reported + 2000), 0.0);
        in[0] = 1.0;
        const auto out = processThrough (e, in);

        size_t peakIdx = 0;
        double peak = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs (out[i]) > peak) { peak = std::abs (out[i]); peakIdx = i; }

        if (std::llabs ((long long) peakIdx - (long long) reported) > 1)
            fail ("Fs=" + std::to_string (Fs) + ": impulse arrived at " + std::to_string (peakIdx)
                  + ", reported latency " + std::to_string (reported));
        std::printf ("  reported=%d  measured=%d\n", reported, (int) peakIdx);
    }

    // Max sample-to-sample delta over x[start .. start+len).
    double maxDelta (const std::vector<double>& x, size_t start, size_t len)
    {
        double m = 0.0;
        for (size_t i = std::max (start, (size_t) 1); i < start + len && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    // ---- 11. Bypass (regression class E): fade to dry, click-free, restarts
    //          clean -------------------------------------------------------
    void bypassTests (double Fs)
    {
        std::printf ("Bypass @ Fs=%.0f\n", Fs);

        // Worst-case squeal grid, reused verbatim from oscillationTests/
        // resetTests: osc ON, stab=1, gate=0, drive=36 dB self-sustains from
        // digital silence and never decays on its own.
        Params p;
        p.driveDb = 36.0; p.stab = 1.0; p.gate = 0.0; p.osc = true;

        FuzzEngine e;
        applyParams (e, p);
        e.prepare (Fs, 256);

        // 1. Let the squeal establish.
        const int buildLen = (int) std::llround (2.5 * Fs);
        const auto established = processThrough (e, std::vector<double> ((size_t) buildLen, 0.0));
        const size_t lastSec = (size_t) std::llround (1.0 * Fs);
        if (rmsOf (established, established.size() - lastSec, lastSec) < 0.05)
            fail ("Fs=" + std::to_string (Fs) + ": bypass setup squeal did not establish");
        const double squealPeak = fct::peakAbs (tail (established, lastSec));

        // Baseline per-sample delta of the *steady, un-bypassed* squeal, for
        // the click-free comparison below (the oscillation itself moves
        // sample-to-sample; the bar is "the bypass edge adds nothing on top
        // of that", not "nothing moves at all").
        const double steadyMaxDelta = maxDelta (established, established.size() - lastSec, lastSec);

        // 2. Engage bypass: within a handful of the documented fade time
        // constants the output (silence in, so dry == 0) must reach the
        // silence floor even though the loop is self-sustaining.
        e.setBypassed (true);
        const int fadeSettleLen = (int) std::llround (10.0 * FuzzEngine::kBypassFadeSec * Fs);
        const int fadeWatchLen  = (int) std::llround (20.0 * FuzzEngine::kBypassFadeSec * Fs);
        const auto fadeOut = processThrough (e, std::vector<double> ((size_t) fadeWatchLen, 0.0));

        if (fct::peakAbs (tail (fadeOut, (size_t) (fadeWatchLen - fadeSettleLen))) > 1e-4)
            fail ("Fs=" + std::to_string (Fs) + ": bypass fade did not reach silence within "
                  + std::to_string (10.0 * FuzzEngine::kBypassFadeSec) + " s ("
                  + std::to_string (fct::peakAbs (tail (fadeOut, (size_t) (fadeWatchLen - fadeSettleLen)))) + ")");

        // Click-free bypass edge: a hard cut to dry would jump by
        // ~squealPeak in a single sample; a smooth one-pole fade cannot add
        // more than a modest margin over the oscillation's own steady delta.
        const double bypassEdgeDelta = maxDelta (fadeOut, 0, (size_t) fadeSettleLen);
        if (bypassEdgeDelta > 2.0 * steadyMaxDelta + 1e-6)
            fail ("Fs=" + std::to_string (Fs) + ": bypass edge click (" + std::to_string (bypassEdgeDelta)
                  + " vs steady " + std::to_string (steadyMaxDelta) + ")");

        // 3. Un-bypass: the loop must restart clean (clearedWhileBypassed),
        // i.e. re-grow from the noise floor like a fresh engine rather than
        // snap back to the established squeal amplitude. The regrowth from
        // the 1e-11 noise seed is exponential and, once it takes off, VERY
        // fast (measured on this engine: ~3e-8 peak at 20 ms, full ~1.0
        // swing by ~100 ms) -- so a *correctly cleared* loop and a loop whose
        // state *survived* bypass look identical if you wait 300 ms; they
        // only differ in the first few ms, where a survived squeal would
        // already show a large fraction of full amplitude (scaled by the
        // wetFade ramp itself) while a genuinely cleared loop is still stuck
        // at the noise floor. Use that short, timing-insensitive window.
        e.setBypassed (false);
        const int immediateLen = (int) std::llround (0.015 * Fs);
        const auto regrow = processThrough (e, std::vector<double> ((size_t) immediateLen, 0.0));

        FuzzEngine fresh;
        applyParams (fresh, p);
        fresh.prepare (Fs, 256);
        const auto freshRegrow = processThrough (fresh, std::vector<double> ((size_t) immediateLen, 0.0));

        const double regrowPeak = fct::peakAbs (regrow);
        const double freshPeak  = fct::peakAbs (freshRegrow);
        const double residualBound = 1e-3; // << squealPeak (~1), >> the measured noise-floor growth at 15 ms
        if (regrowPeak > residualBound)
            fail ("Fs=" + std::to_string (Fs) + ": squeal survived bypass (regrow peak " + std::to_string (regrowPeak)
                  + " within " + std::to_string (immediateLen) + " samples of un-bypass; established squeal was "
                  + std::to_string (squealPeak) + ")");
        if (regrowPeak > 1000.0 * freshPeak + 1e-6)
            fail ("Fs=" + std::to_string (Fs) + ": post-unbypass regrowth (" + std::to_string (regrowPeak)
                  + ") not comparable to a fresh engine's (" + std::to_string (freshPeak) + ")");

        // Click-free un-bypass edge too (regrowth is negligible here, so this
        // should be far below anything squeal-scale).
        const double unbypassEdgeDelta = maxDelta (regrow, 0, (size_t) immediateLen);
        if (unbypassEdgeDelta > 0.01)
            fail ("Fs=" + std::to_string (Fs) + ": un-bypass edge click (" + std::to_string (unbypassEdgeDelta) + ")");

        std::printf ("  squeal peak=%.3f  bypass-edge delta=%.5f (steady %.5f)  regrow(15ms) peak=%.2e (fresh %.2e)\n",
                     squealPeak, bypassEdgeDelta, steadyMaxDelta, regrowPeak, freshPeak);
    }

    // ---- 12. Tone tilt direction + meaningful magnitude ----------------------
    void toneTests (double Fs)
    {
        std::printf ("Tone tilt @ Fs=%.0f\n", Fs);

        const double loHz = 150.0, hiHz = 4000.0; // both well off the 800 Hz shelf corner
        const double duration = 16384.0 / 48000.0;
        const int    N = (int) std::llround (duration * Fs);
        const int    loCycles = (int) std::llround (loHz * N / Fs);
        const int    hiCycles = (int) std::llround (hiHz * N / Fs);
        const double loW = 2.0 * kPi * loCycles / N;
        const double hiW = 2.0 * kPi * hiCycles / N;
        const int    warmup = (int) std::llround (0.3 * Fs);
        const double amp = 0.01; // small signal: shaper stays ~linear so the shelf tilt dominates

        auto ratioDbFor = [&] (double toneVal)
        {
            std::vector<double> in ((size_t) (warmup + N));
            for (int n = 0; n < warmup + N; ++n)
                in[(size_t) n] = amp * std::sin (loW * n) + amp * std::sin (hiW * n);

            Params p;
            p.driveDb = 0.0;
            p.tone    = toneVal;
            const auto y = tail (runEngine (p, Fs, in), (size_t) N);

            const double loMag = std::abs (dftAt (y, loW));
            const double hiMag = std::abs (dftAt (y, hiW));
            return db (hiMag / loMag);
        };

        const double ratioNeg  = ratioDbFor (-1.0);
        const double ratioFlat = ratioDbFor (0.0);
        const double ratioPos  = ratioDbFor (1.0);

        // Documented direction: tone>0 cuts the low shelf / boosts the high
        // shelf (brighter); tone<0 is the mirror image (darker).
        if (! (ratioPos > ratioFlat && ratioFlat > ratioNeg))
            fail ("Fs=" + std::to_string (Fs) + ": tone tilt direction wrong (tone=-1:"
                  + std::to_string (ratioNeg) + " 0:" + std::to_string (ratioFlat)
                  + " +1:" + std::to_string (ratioPos) + " dB hi/lo)");

        const double swing = ratioPos - ratioNeg;
        if (swing < 12.0) // meaningful: comfortably below the ~2*2*kToneRangeDb=36dB asymptote
            fail ("Fs=" + std::to_string (Fs) + ": tone tilt swing too small (" + std::to_string (swing) + " dB)");

        std::printf ("  hi/lo ratio: tone=-1 %.1f dB  tone=0 %.1f dB  tone=+1 %.1f dB (swing %.1f dB)\n",
                     ratioNeg, ratioFlat, ratioPos, swing);
    }

    // ---- 13. Level: exact linear-gain oracle ---------------------------------
    void levelTests (double Fs)
    {
        std::printf ("Level gain oracle @ Fs=%.0f\n", Fs);

        const int len = (int) std::llround (0.5 * Fs);
        const auto in = sineSignal (Fs, 600.0, 0.3, len);

        Params base;
        base.driveDb = 12.0; base.bias = 0.2; base.mix = 1.0; base.levelDb = 0.0;
        Params boosted = base;
        boosted.levelDb = 12.0;

        const auto outBase  = runEngine (base, Fs, in);
        const auto outBoost = runEngine (boosted, Fs, in);

        const double levelBase     = std::pow (10.0, base.levelDb / 20.0);
        const double levelBoost    = std::pow (10.0, boosted.levelDb / 20.0);
        const double expectedRatio = levelBoost / levelBase;

        // level multiplies the output *after* the nonlinear shaper and is
        // never fed back into the loop (processChannel writes `y` into
        // fbDelay before applying level) — with mix=1 the entire waveform
        // must scale by the level ratio exactly (up to float rounding), an
        // oracle from the parameter's own definition rather than a
        // re-derivation of the implementation.
        const size_t settle = (size_t) std::llround (0.1 * Fs);
        double num = 0.0, den = 0.0;
        for (size_t n = settle; n < outBase.size(); ++n)
        {
            num += outBoost[n] * outBoost[n];
            den += outBase[n] * outBase[n];
        }
        const double measuredRatio = std::sqrt (num / den);
        if (std::abs (measuredRatio - expectedRatio) > 0.02 * expectedRatio)
            fail ("Fs=" + std::to_string (Fs) + ": level gain " + std::to_string (measuredRatio)
                  + "x != expected " + std::to_string (expectedRatio) + "x");

        // Pointwise too: the scaling must hold sample-by-sample, not just in
        // aggregate energy.
        double worstRel = 0.0;
        for (size_t n = settle; n < outBase.size(); ++n)
        {
            const double b = outBase[n];
            if (std::abs (b) < 0.02) continue; // skip near-zero crossings (relative error blows up)
            worstRel = std::max (worstRel, std::abs (outBoost[n] / b - expectedRatio) / expectedRatio);
        }
        if (worstRel > 0.05)
            fail ("Fs=" + std::to_string (Fs) + ": level scaling not pointwise-linear (worst rel err "
                  + std::to_string (worstRel) + ")");

        std::printf ("  measured ratio=%.4fx expected=%.4fx (worst pointwise rel err %.4f)\n",
                     measuredRatio, expectedRatio, worstRel);
    }

    // ---- 14. Mix intermediate blend ------------------------------------------
    void mixTests (double Fs)
    {
        std::printf ("Mix intermediate blend @ Fs=%.0f\n", Fs);

        const int len = (int) std::llround (0.5 * Fs);
        const auto in = sineSignal (Fs, 600.0, 0.3, len);

        Params p;
        p.driveDb = 18.0; p.bias = 0.3; p.levelDb = 0.0;
        Params dryP = p;  dryP.mix  = 0.0;
        Params wetP = p;  wetP.mix  = 1.0;
        Params halfP = p; halfP.mix = 0.5;

        const auto outDry  = runEngine (dryP, Fs, in);
        const auto outWet  = runEngine (wetP, Fs, in);
        const auto outHalf = runEngine (halfP, Fs, in);

        // processChannel (~line 298) combines dry and level*y at the *same*
        // model-rate sample index, before the shared RateBracket
        // downsampling -- dry and wet are inherently latency-aligned, so
        // mix=0.5 must equal 0.5*dry + 0.5*wet sample-for-sample with no
        // manual delay compensation.
        const size_t settle = (size_t) std::llround (0.05 * Fs);
        double worstAbs = 0.0, refPeak = 1e-12;
        for (size_t n = settle; n < outHalf.size(); ++n)
        {
            const double predicted = 0.5 * outDry[n] + 0.5 * outWet[n];
            worstAbs = std::max (worstAbs, std::abs (outHalf[n] - predicted));
            refPeak  = std::max (refPeak, std::abs (predicted));
        }
        const double tol = 1e-3 * refPeak + 1e-4; // float-rounding margin, not a loosened gate
        if (worstAbs > tol)
            fail ("Fs=" + std::to_string (Fs) + ": mix=0.5 (worst |diff|=" + std::to_string (worstAbs)
                  + ") != 0.5*dry+0.5*wet within " + std::to_string (tol));

        std::printf ("  worst |mix0.5 - 0.5*(dry+wet)| = %.6f (tol %.6f, ref peak %.4f)\n",
                     worstAbs, tol, refPeak);
    }
}

int main (int argc, char** argv)
{
    staticTransferInvariants();

    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        harmonicTests (Fs);
        gateTests (Fs);
        sputterTest (Fs);
        oscillationTests (Fs);
        stabilityTests (Fs);
        resetTests (Fs);
        aliasTests (Fs);
        dcTests (Fs);
        latencyTests (Fs);
        bypassTests (Fs);
        toneTests (Fs);
        levelTests (Fs);
        mixTests (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
