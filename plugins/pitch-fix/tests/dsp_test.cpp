//
// plugins/pitch-fix/tests/dsp_test.cpp — headless spec tests for pf_core::PfCore
// (Pitch TatFixer). Links only factory_core; no JUCE, no CLAP, no host.
//
// Oracles (independent of the implementation):
//   * Correction targets are MUSIC THEORY: a tone at a known offset from a
//     scale note must land on that note's equal-tempered frequency (measured
//     with an analysis FFT whose order follows the sample rate).
//   * The latency CONTRACT is the documented mode table
//     round(kLookaheadPeriods[mode] * fs / minPitch) — re-derived here — and
//     with Correction Amount at 0 the whole plugin must be an EXACT pure
//     delay of that many samples (asserted sample-by-sample on noise).
//   * Silence in -> silence out (detector absolute floor: no phantom output).
//   * Worst-case hold (Quality, amount 150%, retune 0) stays finite, bounded
//     and stable — the regression-policy long-hold gate.
//
#include "PfCore.h"

#include "factory_core/FFT.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fct = factory_core::testing;

static constexpr double kPi = 3.14159265358979323846;

static int g_failures = 0;

static void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("FAIL: %s\n", msg.c_str());
}

static double centsBetween (double a, double b)
{
    return 1200.0 * std::log2 (a / b);
}

static std::vector<float> makeNoise (int n, float amp, unsigned seed)
{
    std::vector<float> v ((size_t) n);
    unsigned s = seed;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        v[(size_t) i] = amp * (2.0f * ((float) (s >> 8) / 16777216.0f) - 1.0f);
    }
    return v;
}

static std::vector<float> makeSine (int n, double fs, double f, double amp)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = (float) (amp * std::sin (2.0 * kPi * f * (double) i / fs));
    return v;
}

struct ToneMeasure { double freqHz = 0.0, snrDb = 0.0, rms = 0.0; };

// Analysis-FFT tone measurement (Hann + parabolic interpolation); the order
// follows the sample rate so the oracle's resolution is rate-independent.
static ToneMeasure measureTone (const std::vector<double>& x, double Fs)
{
    const int order = factory_core::fftOrderForSampleRate (Fs, 15, 48000.0, 17);
    const int N     = 1 << order;
    ToneMeasure out;
    if ((int) x.size() < N)
        return out;

    factory_core::FFT fft;
    fft.prepare (order);
    std::vector<factory_core::FFT::cd> buf ((size_t) N);
    const size_t off = x.size() - (size_t) N;
    double sq = 0.0;
    for (int i = 0; i < N; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (N - 1));
        buf[(size_t) i] = factory_core::FFT::cd (x[off + (size_t) i] * w, 0.0);
        sq += x[off + (size_t) i] * x[off + (size_t) i];
    }
    out.rms = std::sqrt (sq / (double) N);
    fft.forward (buf.data());

    const int half = N / 2;
    std::vector<double> p ((size_t) half, 0.0);
    for (int k = 0; k < half; ++k)
        p[(size_t) k] = std::norm (buf[(size_t) k]);
    int kMaxBin = 3;
    for (int k = 3; k < half; ++k)
        if (p[(size_t) k] > p[(size_t) kMaxBin]) kMaxBin = k;

    double delta = 0.0;
    if (kMaxBin > 3 && kMaxBin < half - 1)
    {
        const double l0 = std::log (p[(size_t) (kMaxBin - 1)] + 1.0e-30);
        const double l1 = std::log (p[(size_t) kMaxBin] + 1.0e-30);
        const double l2 = std::log (p[(size_t) (kMaxBin + 1)] + 1.0e-30);
        const double den = l0 - 2.0 * l1 + l2;
        if (std::abs (den) > 1.0e-12)
            delta = std::max (-0.5, std::min (0.5, 0.5 * (l0 - l2) / den));
    }
    out.freqHz = ((double) kMaxBin + delta) * Fs / (double) N;

    double peak = 0.0, tot = 0.0;
    for (int k = 3; k < half; ++k)
    {
        tot += p[(size_t) k];
        if (std::abs (k - kMaxBin) <= 3) peak += p[(size_t) k];
    }
    out.snrDb = 10.0 * std::log10 (peak / std::max (tot - peak, 1.0e-30));
    return out;
}

// Run a mono signal through a fresh core (duplicated to stereo), return L.
static std::vector<float> run (pf_core::PfCore& core, const std::vector<float>& x,
                               const pf_core::PfParamSnapshot& s, int block = 512)
{
    std::vector<float> l (x), r (x);
    for (int pos = 0; pos < (int) x.size(); pos += block)
    {
        const int m = std::min (block, (int) x.size() - pos);
        core.process (l.data() + pos, r.data() + pos, m, s);
    }
    return l;
}

static std::vector<double> tailOf (const std::vector<float>& y, double Fs, double fromSec)
{
    std::vector<double> t;
    for (int i = (int) (fromSec * Fs); i < (int) y.size(); ++i)
        t.push_back ((double) y[(size_t) i]);
    return t;
}

static pf_core::PfParamSnapshot tightSnapshot()
{
    pf_core::PfParamSnapshot s;
    s.retuneMs    = 5.0f;
    s.glideMs     = 0.0f;
    s.toleranceCt = 0.0f;
    return s;
}

static void coreTests (double Fs)
{
    // --- 1. chromatic correction: +45 cents lands back on A4 ------------------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        const double fIn = 440.0 * std::pow (2.0, 45.0 / 1200.0);
        auto y = run (core, makeSine ((int) (2.5 * Fs), Fs, fIn, 0.5), s);
        const auto m = measureTone (tailOf (y, Fs, 1.0), Fs);
        if (std::abs (centsBetween (m.freqHz, 440.0)) > 5.0)
            fail ("chromatic +45ct: got " + std::to_string (m.freqHz)
                  + " Hz (want 440) @" + std::to_string (Fs));
        if (m.snrDb < 18.0)
            fail ("chromatic correction SNR " + std::to_string (m.snrDb)
                  + " dB < 18 @" + std::to_string (Fs));
    }

    // --- 2. scale masks: A major / A minor pick the right target --------------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        s.key   = 9;  // A
        s.scale = 1;  // Major: 462 Hz -> A4 440 (A# is not in A major)
        auto y = run (core, makeSine ((int) (2.5 * Fs), Fs, 462.0, 0.5), s);
        const auto m = measureTone (tailOf (y, Fs, 1.0), Fs);
        if (std::abs (centsBetween (m.freqHz, 440.0)) > 5.0)
            fail ("A-major mask: got " + std::to_string (m.freqHz)
                  + " Hz (want 440) @" + std::to_string (Fs));
    }
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        s.key   = 9;  // A
        s.scale = 2;  // Minor: 528 Hz -> C5 523.25
        auto y = run (core, makeSine ((int) (2.5 * Fs), Fs, 528.0, 0.5), s);
        const auto m = measureTone (tailOf (y, Fs, 1.0), Fs);
        if (std::abs (centsBetween (m.freqHz, 523.2511)) > 5.0)
            fail ("A-minor mask: got " + std::to_string (m.freqHz)
                  + " Hz (want 523.25) @" + std::to_string (Fs));
    }

    // --- 3. tolerance deadzone: +20 ct inside a 35 ct window stays put --------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        s.toleranceCt = 35.0f;
        const double fIn = 440.0 * std::pow (2.0, 20.0 / 1200.0);
        auto y = run (core, makeSine ((int) (2.5 * Fs), Fs, fIn, 0.5), s);
        const auto m = measureTone (tailOf (y, Fs, 1.0), Fs);
        if (std::abs (centsBetween (m.freqHz, fIn)) > 3.0)
            fail ("tolerance: input moved by "
                  + std::to_string (centsBetween (m.freqHz, fIn)) + " ct @" + std::to_string (Fs));
        if (std::abs (centsBetween (m.freqHz, 440.0)) < 15.0)
            fail ("tolerance: input was pulled to the note @" + std::to_string (Fs));
    }

    // --- 4. latency contract: amount 0 == exact pure delay, per mode ----------
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            pf_core::PfCore core;
            core.prepare (Fs, 512);
            pf_core::PfParamSnapshot s;
            s.amount = 0.0f;
            s.buffer = mode;
            const int N = (int) (1.2 * Fs);
            auto x = makeNoise (N, 0.5f, 0x5EED0u + (unsigned) mode);
            auto y = run (core, x, s);

            const int expectL =
                (int) std::lround (pf_core::PfCore::kLookaheadPeriods[mode] * Fs / 75.0);
            if (core.latencySamples() != expectL)
                fail ("mode " + std::to_string (mode) + " latency "
                      + std::to_string (core.latencySamples()) + " != spec "
                      + std::to_string (expectL) + " @" + std::to_string (Fs));

            const int L = core.latencySamples();
            double maxDiff = 0.0;
            for (int t = 0; t < N; ++t)
            {
                const float d = t >= L ? x[(size_t) (t - L)] : 0.0f;
                maxDiff = std::max (maxDiff, std::abs ((double) y[(size_t) t] - (double) d));
            }
            if (maxDiff > 1.0e-4)
                fail ("mode " + std::to_string (mode) + " amount-0 path deviates from "
                      "pure delay by " + std::to_string (maxDiff) + " @" + std::to_string (Fs));
        }
    }

    // --- 5. latency ladder: monotonic in mode, follows Min Pitch --------------
    {
        int prev = 0;
        for (int mode = 0; mode < 4; ++mode)
        {
            pf_core::PfCore core;
            core.prepare (Fs, 512);
            pf_core::PfParamSnapshot s;
            s.buffer = mode;
            std::vector<float> blk (512, 0.0f);
            core.process (blk.data(), nullptr, 512, s);
            if (core.latencySamples() <= prev)
                fail ("latency not increasing at mode " + std::to_string (mode)
                      + " @" + std::to_string (Fs));
            prev = core.latencySamples();
        }
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        pf_core::PfParamSnapshot s;
        s.minPitchHz = 150.0f;
        std::vector<float> blk (512, 0.0f);
        core.process (blk.data(), nullptr, 512, s);
        const int expect = (int) std::lround (pf_core::PfCore::kLookaheadPeriods[2] * Fs / 150.0);
        if (core.latencySamples() != expect)
            fail ("minPitch 150 latency " + std::to_string (core.latencySamples())
                  + " != spec " + std::to_string (expect) + " @" + std::to_string (Fs));
    }

    // --- 6. silence in -> silence out (no phantom correction/output) ----------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        pf_core::PfParamSnapshot s;
        std::vector<float> x ((size_t) (1.0 * Fs), 0.0f);
        auto y = run (core, x, s);
        std::vector<double> yd (y.begin(), y.end());
        if (fct::peakAbs (yd) > 1.0e-9)
            fail ("silence produced output (peak " + std::to_string (fct::peakAbs (yd))
                  + ") @" + std::to_string (Fs));
    }

    // --- 7. worst-case hold: Quality, amount 150%, retune 0, 4 s --------------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        pf_core::PfParamSnapshot s;
        s.buffer      = 3;
        s.amount      = 150.0f;
        s.retuneMs    = 0.0f;
        s.glideMs     = 0.0f;
        s.toleranceCt = 0.0f;
        const double fIn = 440.0 * std::pow (2.0, 49.0 / 1200.0);
        auto y = run (core, makeSine ((int) (4.0 * Fs), Fs, fIn, 0.5), s);
        std::vector<double> yd (y.begin(), y.end());
        if (! fct::allFinite (yd))
            fail ("worst-case hold not finite @" + std::to_string (Fs));
        if (fct::peakAbs (yd) > 0.8)
            fail ("worst-case hold peak " + std::to_string (fct::peakAbs (yd))
                  + " > 0.8 @" + std::to_string (Fs));

        // 150% amount overshoots the target: out = det + 1.5*(target - det)
        // = +49 - 73.5 = -24.5 ct from A4.
        const auto m = measureTone (tailOf (y, Fs, 3.0), Fs);
        const double expected = 440.0 * std::pow (2.0, -24.5 / 1200.0);
        if (std::abs (centsBetween (m.freqHz, expected)) > 8.0)
            fail ("worst-case hold freq " + std::to_string (m.freqHz) + " != "
                  + std::to_string (expected) + " @" + std::to_string (Fs));

        const auto early = measureTone (tailOf (y, Fs, 1.5), Fs);
        if (early.rms > 1.0e-12 && std::abs (m.rms / early.rms - 1.0) > 0.12)
            fail ("worst-case hold RMS drift " + std::to_string (m.rms / early.rms)
                  + " @" + std::to_string (Fs));
    }

    // --- 8. determinism: prepare + identical input twice == identical output --
    {
        pf_core::PfCore core;
        pf_core::PfParamSnapshot s;
        auto x = makeSine ((int) (1.0 * Fs), Fs, 445.0, 0.4);
        core.prepare (Fs, 512);
        auto y1 = run (core, x, s);
        core.prepare (Fs, 512);
        auto y2 = run (core, x, s);
        for (size_t i = 0; i < y1.size(); ++i)
            if (y1[i] != y2[i])
            {
                fail ("output not deterministic across prepare() @" + std::to_string (Fs));
                break;
            }
    }

    // --- 9. parameter random-walk torture stays finite and bounded ------------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        const int N = (int) (2.0 * Fs);
        std::vector<float> x ((size_t) N);
        double inPeak = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double t = (double) i / Fs;
            x[(size_t) i] = (float) (0.25 * std::sin (2.0 * kPi * 220.0 * t)
                                   + 0.10 * std::sin (2.0 * kPi * 373.0 * t));
            inPeak = std::max (inPeak, std::abs ((double) x[(size_t) i]));
        }
        std::vector<float> l (x), r (x);
        unsigned seed = 0xDEC0DEu;
        auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return (double) (seed >> 8) / 16777216.0;
        };
        for (int pos = 0; pos < N; pos += 512)
        {
            pf_core::PfParamSnapshot s;
            s.amount       = (float) (rnd() * 150.0);
            s.retuneMs     = (float) (rnd() * 600.0);
            s.glideMs      = (float) (rnd() * 750.0);
            s.toleranceCt  = (float) (rnd() * 75.0);
            s.hysteresisCt = (float) (rnd() * 75.0);
            s.minPitchHz   = (float) (25.0 + rnd() * 475.0);
            s.maxPitchHz   = (float) (200.0 + rnd() * 3800.0);
            s.thresholdPct = (float) (50.0 + rnd() * 49.0);
            s.buffer       = (int) (rnd() * 3.999);
            s.key          = (int) (rnd() * 11.999);
            s.scale        = (int) (rnd() * 2.999);
            s.a4Hz         = (float) (400.0 + rnd() * 80.0);
            s.mixPct       = (float) (rnd() * 100.0);
            s.outDb        = (float) (-24.0 + rnd() * 24.0);   // gain <= unity
            const int m = std::min (512, N - pos);
            core.process (l.data() + pos, r.data() + pos, m, s);
        }
        std::vector<double> yd (l.begin(), l.end());
        if (! fct::allFinite (yd))
            fail ("param torture output not finite @" + std::to_string (Fs));
        if (fct::peakAbs (yd) > 1.5 * inPeak + 1.0e-6)
            fail ("param torture peak " + std::to_string (fct::peakAbs (yd))
                  + " exceeds bound @" + std::to_string (Fs));
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures > 0)
    {
        std::printf ("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf ("pitch_fix_dsp_test OK\n");
    return 0;
}
