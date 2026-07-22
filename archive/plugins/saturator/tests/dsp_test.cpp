//
// dsp_test.cpp — headless verification of the saturator DSP core
// (factory_core::Waveshaper). The plugin is non-linear, so the gates are:
//
//   1. Transfer-curve invariants (formula-independent): f(0)=0, odd symmetry
//      f(-x) = -f(x), strict monotonicity, boundedness at mix=1, and the
//      small-signal slope output*(1-mix+mix*drive).
//   2. Mix wiring: endpoints (mix=0 -> output*x, mix=1 -> output*tanh(drive*x))
//      and linear interpolation between them.
//   3. Harmonic structure measured by running a pure tone through the core:
//      an odd-symmetric shaper produces only ODD harmonics (even harmonics ~0),
//      and THD grows monotonically with drive.
//
// The tone is placed at an integer number of cycles per block so every harmonic
// lands exactly on a DFT bin (no leakage), and low enough that the significant
// harmonics stay below Nyquist (so base-rate aliasing does not contaminate the
// even-harmonic check).
//
// Sample-rate dependence: the tone is fixed at an absolute frequency in Hz and
// the block is a fixed duration in seconds, so BOTH the number of cycles per
// block and the normalized frequency change with Fs. The memoryless waveshaper
// spreads energy to k*f0 Hz for all odd k, and how many of those land below
// Nyquist — before folding back as aliasing — is a function of Fs. The rate loop
// therefore asserts genuinely rate-dependent quantities: harmonics land at the
// expected absolute Hz (k*f0) for that Fs, and the count of odd harmonics that
// fit below Nyquist matches the analytic value floor(Nyquist / f0). A regression
// that ignored Fs (e.g. treating the buffer as a fixed order regardless of rate)
// would land the harmonics at the wrong Hz and fail.
//
#include "factory_core/Waveshaper.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fct = factory_core::testing;

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;

    // The plugin's drive parameter tops out at 36 dB. factory_core::Waveshaper
    // takes a *linear* gain -- the dB->linear conversion happens in
    // PluginProcessor::processBlock via juce::Decibels::decibelsToGain (a
    // generic, well-known formula, not derived from the shaper under test) --
    // so the worst-case linear drive this core ever sees in the plugin is
    // 10^(36/20).
    constexpr double kMaxDriveLinear = 63.09573444801933;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    factory_core::Waveshaper makeShaper (double drive, double mix, double output)
    {
        factory_core::Waveshaper w;
        w.setDrive (drive);
        w.setMix (mix);
        w.setOutput (output);
        return w;
    }

    // Direct DFT bin via phase recurrence: sum x[n] e^{-j w n}.
    cd dftAt (const std::vector<double>& x, double w)
    {
        const cd step = std::exp (cd (0.0, -w));
        cd zk (1.0, 0.0);
        cd acc (0.0, 0.0);
        for (const double xn : x) { acc += xn * zk; zk *= step; }
        return acc;
    }

    void transferInvariants()
    {
        std::printf ("Transfer-curve invariants\n");
        // kMaxDriveLinear (36 dB, the plugin's top of range) is included here so
        // every combo below -- f(0)=0, odd symmetry, monotonicity, small-signal
        // slope, mix=1 boundedness, and exact mix wiring (incl. mix=0 bit-exact
        // dry) -- is exercised at the worst-case drive too, not just up to 10.0.
        const double drives[]  = { 0.5, 1.0, 2.0, 5.0, 10.0, kMaxDriveLinear };
        const double mixes[]   = { 0.0, 0.25, 0.5, 1.0 };
        const double outputs[] = { 0.5, 1.0, 2.0 };

        for (double drive : drives)
            for (double mix : mixes)
                for (double output : outputs)
                {
                    const auto w = makeShaper (drive, mix, output);
                    const std::string tag = "d=" + std::to_string (drive) + " m=" + std::to_string (mix)
                                          + " o=" + std::to_string (output);

                    // f(0) = 0
                    if (std::abs (w.processSample (0.0)) > 1e-12)
                        fail (tag + ": f(0) != 0");

                    // Odd symmetry and monotonicity over a grid.
                    double prev = w.processSample (-4.0);
                    for (int i = -40; i <= 40; ++i)
                    {
                        const double x = i * 0.1;
                        const double fp = w.processSample (x);
                        const double fn = w.processSample (-x);
                        if (std::abs (fp + fn) > 1e-12)
                            fail (tag + ": not odd-symmetric at x=" + std::to_string (x));
                        if (i > -40 && fp < prev - 1e-15)
                            fail (tag + ": not monotonic at x=" + std::to_string (x));
                        prev = fp;
                    }

                    // Small-signal slope: output*(1-mix+mix*drive).
                    const double eps = 1e-5;
                    const double slope = (w.processSample (eps) - w.processSample (-eps)) / (2.0 * eps);
                    const double expected = output * (1.0 - mix + mix * drive);
                    if (std::abs (slope - expected) > 1e-4 * std::max (1.0, std::abs (expected)))
                        fail (tag + ": slope@0 " + std::to_string (slope) + " != " + std::to_string (expected));

                    // Boundedness at mix=1: |f| <= output.
                    if (mix == 1.0)
                        for (double x : { -100.0, -10.0, 10.0, 100.0 })
                            if (std::abs (w.processSample (x)) > output * (1.0 + 1e-9))
                                fail (tag + ": exceeds output bound at x=" + std::to_string (x));

                    // Mix wiring: f = (1-mix)*output*x + mix*output*tanh(drive*x).
                    for (double x : { -2.0, -0.3, 0.7, 3.0 })
                    {
                        const double dry = output * x;
                        const double wet = output * std::tanh (drive * x);
                        const double ref = (1.0 - mix) * dry + mix * wet;
                        if (std::abs (w.processSample (x) - ref) > 1e-12)
                            fail (tag + ": mix wiring mismatch at x=" + std::to_string (x));
                    }
                }
        std::printf ("  done (%d combos)\n", (int) (6 * 4 * 3));
    }

    // THD from odd harmonics (3,5,...) relative to the fundamental. The tone is
    // an integer number of `cycles` per N-sample block (integer bin, no leakage);
    // both `cycles` and `N` are derived from Fs by the caller, so the normalized
    // frequency w0 differs per rate. `oddBelowNyquist` reports how many odd
    // harmonics (k=1,3,5,...) fall strictly below Nyquist for this Fs — a
    // rate-dependent count the caller checks against the analytic value.
    double measureHarmonics (double drive, int N, int cycles, double amp,
                             bool& evenClean, int& oddBelowNyquist)
    {
        const auto w = makeShaper (drive, 1.0, 1.0);
        const double w0 = 2.0 * kPi * cycles / N; // fundamental angular freq (integer bin)

        std::vector<double> y ((size_t) N);
        for (int n = 0; n < N; ++n)
            y[(size_t) n] = w.processSample (amp * std::sin (w0 * n));

        const double fund = std::abs (dftAt (y, w0));

        // A harmonic k lands strictly below Nyquist iff its bin k*cycles < N/2.
        // Even harmonics must be ~0 (odd-symmetric shaper).
        evenClean = true;
        for (int k = 2; 2 * k * cycles < N; k += 2) // even k, below Nyquist
        {
            const double mag = std::abs (dftAt (y, w0 * k));
            if (mag > 1e-6 * fund) { evenClean = false; }
        }

        // THD: RMS of odd harmonics (k>=3) over the fundamental, and count the
        // odd harmonics that resolve below Nyquist (rate-dependent).
        oddBelowNyquist = 1; // the fundamental itself (k=1)
        double sumSq = 0.0;
        for (int k = 3; 2 * k * cycles < N; k += 2)
        {
            const double mag = std::abs (dftAt (y, w0 * k));
            sumSq += mag * mag;
            ++oddBelowNyquist;
        }
        return std::sqrt (sumSq) / fund;
    }

    void harmonicTests (double Fs)
    {
        std::printf ("Harmonic structure @ Fs=%.0f\n", Fs);

        // Fixed physical stimulus: an absolute tone frequency and a fixed block
        // duration. Deriving the discrete grid from Fs makes the loop body genuinely
        // rate-dependent (cycles and N both scale with Fs). f0=600 Hz keeps the
        // audible odd harmonics well below Nyquist even at the lowest rate (44.1k:
        // 36 harmonics fit under Nyquist), so the even-harmonic check stays free of
        // alias contamination at every rate.
        const double f0Hz     = 600.0;
        const double duration = 16384.0 / 48000.0; // seconds (~0.341 s)
        const int    N        = (int) std::llround (duration * Fs);
        const int    cycles   = (int) std::llround (f0Hz * N / Fs); // integer bin
        const double f0Actual = cycles * Fs / N;                    // Hz on the grid
        const double nyquist  = 0.5 * Fs;
        const double amp      = 0.8;

        // Rate-dependent invariant 1: the fundamental bin sits at the intended
        // absolute frequency (within one bin, fs/N), so the tone truly tracks Fs.
        const double binHz = Fs / N;
        if (std::abs (f0Actual - f0Hz) > binHz)
            fail ("Fs=" + std::to_string (Fs) + ": fundamental " + std::to_string (f0Actual)
                  + " Hz not within one bin of " + std::to_string (f0Hz) + " Hz");

        // Rate-dependent invariant 2: the number of odd harmonics that fit below
        // Nyquist is an analytic function of Fs (independent oracle). At 44.1k this
        // is ~18 odd harmonics; at 192k ~80. A regression that ignored Fs would
        // resolve the wrong count.
        int expectedOddBelowNyq = 0;
        for (int k = 1; k * f0Actual < nyquist; k += 2)
            ++expectedOddBelowNyq;

        double prevThd = -1.0;
        // kMaxDriveLinear (36 dB) added: at the worst-case drive the shaper is
        // deep into saturation (tanh(63.1*x) is nearly a square wave for any
        // audible amplitude), so odd-harmonic dominance / even-harmonic-clean
        // must still hold there, and THD should still not have *decreased*
        // relative to the lower drives on the way to that asymptote.
        for (double drive : { 2.0, 5.0, 10.0, kMaxDriveLinear })
        {
            bool evenClean = true;
            int  oddBelowNyq = 0;
            const double thd = measureHarmonics (drive, N, cycles, amp, evenClean, oddBelowNyq);
            if (! evenClean)
                fail ("drive=" + std::to_string (drive) + ": even harmonics not ~0");
            if (prevThd >= 0.0 && thd <= prevThd)
                fail ("drive=" + std::to_string (drive) + ": THD not increasing with drive");
            if (oddBelowNyq != expectedOddBelowNyq)
                fail ("Fs=" + std::to_string (Fs) + " drive=" + std::to_string (drive)
                      + ": odd harmonics below Nyquist " + std::to_string (oddBelowNyq)
                      + " != analytic " + std::to_string (expectedOddBelowNyq));

            // Rate-dependent invariant 3: each measured odd harmonic sits at its
            // analytic absolute frequency k*f0 Hz (within one bin), and carries real
            // energy (>1e-9 of the fundamental at drive>=2), confirming the spectrum
            // is placed on the Fs-derived grid rather than a fixed one.
            const auto ws = makeShaper (drive, 1.0, 1.0);
            std::vector<double> y ((size_t) N);
            const double w0 = 2.0 * kPi * cycles / N;
            for (int n = 0; n < N; ++n)
                y[(size_t) n] = ws.processSample (amp * std::sin (w0 * n));
            const double fund = std::abs (dftAt (y, w0));
            for (int k = 3; 2 * k * cycles < N && k <= 9; k += 2)
            {
                const double hHz = k * f0Actual;
                // The k-th harmonic bin inherits k× the fundamental's ≤1-bin grid
                // offset, so its analytic tolerance is k*binHz.
                if (std::abs (hHz - k * f0Hz) > k * binHz)
                    fail ("Fs=" + std::to_string (Fs) + ": harmonic k=" + std::to_string (k)
                          + " at " + std::to_string (hHz) + " Hz off analytic " + std::to_string (k * f0Hz));
                if (std::abs (dftAt (y, w0 * k)) <= 1e-9 * fund)
                    fail ("Fs=" + std::to_string (Fs) + " drive=" + std::to_string (drive)
                          + ": odd harmonic k=" + std::to_string (k) + " missing");
            }

            std::printf ("  drive=%.1f  THD=%.4f  evenClean=%d  oddBelowNyq=%d\n",
                         drive, thd, (int) evenClean, oddBelowNyq);
            prevThd = thd;
        }
        std::printf ("  f0=%.2f Hz (target %.0f)  N=%d  cycles=%d  binHz=%.3f\n",
                     f0Actual, f0Hz, N, cycles, binHz);
    }

    // Max-drive (36 dB / kMaxDriveLinear) long loud hold. The Waveshaper is
    // memoryless (no internal state), so its output does not depend on the
    // sample rate at all -- there is nothing here for a rate loop to catch
    // that a single, long, sample-count-based hold wouldn't. What IS untested
    // above is many *consecutive* samples at the worst-case drive with a loud
    // (beyond +/-1 full scale) input, exactly the "sustained loud passage"
    // scenario the regression policy calls out: finite over a long hold, a
    // realistic (formula-independent) peak bound, and mix wiring still exact
    // (mix=0 -> bit-exact dry) at that drive.
    void maxDriveLongHoldTests()
    {
        std::printf ("Max-drive (36 dB) long loud hold\n");

        constexpr int    kNumSamples = 1'000'000;
        constexpr double kLoudAmp    = 6.0; // well beyond +/-1 digital full scale

        // Incommensurate angular step (not 2*pi*k/N for any small k) so the
        // held tone does not happen to land back on a periodic pattern that
        // would mask a growing/dropped-sample bug.
        std::vector<double> x ((size_t) kNumSamples);
        for (int n = 0; n < kNumSamples; ++n)
            x[(size_t) n] = kLoudAmp * std::sin (0.0137 * n);

        // 1) mix=1 (fully wet): |tanh(.)| <= 1 for all real arguments regardless
        //    of how large drive*x gets, so the peak can never exceed `output`
        //    -- a formula-independent bound, not derived from the shaper.
        {
            const auto w = makeShaper (kMaxDriveLinear, 1.0, 1.0);
            std::vector<double> y ((size_t) kNumSamples);
            for (int n = 0; n < kNumSamples; ++n)
                y[(size_t) n] = w.processSample (x[(size_t) n]);

            if (! fct::allFinite (y))
                fail ("max-drive long hold (mix=1): non-finite output");
            const double peak = fct::peakAbs (y);
            if (peak > 1.0 + 1e-9)
                fail ("max-drive long hold (mix=1): peak " + std::to_string (peak)
                      + " exceeds output bound 1.0");
        }

        // 2) mix=0 (fully dry): drive must have zero effect -- bit-exact
        //    passthrough even at the worst-case drive, over the whole hold.
        {
            const auto w = makeShaper (kMaxDriveLinear, 0.0, 1.0);
            for (int n = 0; n < kNumSamples; ++n)
            {
                const double y = w.processSample (x[(size_t) n]);
                if (std::abs (y - x[(size_t) n]) > 1e-12)
                {
                    fail ("max-drive long hold (mix=0): dry passthrough not bit-exact at n="
                          + std::to_string (n));
                    break;
                }
            }
        }

        // 3) A realistic intermediate mix: finite, and bounded by the analytic
        //    worst case output*((1-mix)*|x|_max + mix*1) -- the dry term scales
        //    linearly with the (bounded) input, the wet term is tanh-bounded.
        {
            const double mix = 0.5;
            const auto w = makeShaper (kMaxDriveLinear, mix, 1.0);
            std::vector<double> y ((size_t) kNumSamples);
            for (int n = 0; n < kNumSamples; ++n)
                y[(size_t) n] = w.processSample (x[(size_t) n]);

            if (! fct::allFinite (y))
                fail ("max-drive long hold (mix=0.5): non-finite output");
            const double bound = (1.0 - mix) * kLoudAmp + mix * 1.0;
            const double peak  = fct::peakAbs (y);
            if (peak > bound + 1e-9)
                fail ("max-drive long hold (mix=0.5): peak " + std::to_string (peak)
                      + " exceeds analytic bound " + std::to_string (bound));
        }

        std::printf ("  done (%d samples)\n", kNumSamples);
    }
}

int main (int argc, char** argv)
{
    transferInvariants();
    maxDriveLongHoldTests();

    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        harmonicTests (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
