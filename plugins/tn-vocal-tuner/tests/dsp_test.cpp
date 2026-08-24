//
// plugins/tn-vocal-tuner/tests/dsp_test.cpp — headless spec tests for pf_core::PfCore
// (TN Vocal Tuner). Links only factory_core; no JUCE, no CLAP, no host.
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

#include "factory_core/Biquad.h"
#include "factory_core/FFT.h"
#include "factory_core/Filters.h"
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

// Process `x` (duplicated to stereo) through a FRESH core with a fixed
// maxBlock (so ring sizes are identical across variants) but a caller-supplied
// block-size SEQUENCE, isolating the test to where host block boundaries fall.
static std::vector<float> runBlockSeq (double Fs, const std::vector<float>& x,
                                       const pf_core::PfParamSnapshot& s,
                                       const std::vector<int>& seq)
{
    pf_core::PfCore core;
    core.prepare (Fs, 2048);
    std::vector<float> l (x), r (x);
    int pos = 0, bi = 0;
    while (pos < (int) x.size())
    {
        const int m = std::min (seq[(size_t) (bi++ % (int) seq.size())], (int) x.size() - pos);
        core.process (l.data() + pos, r.data() + pos, m, s);
        pos += m;
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

    // --- 4. latency contract: amount 0 == pure delay (within tol), per mode ----
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

    // --- 10. BLOCK-SIZE INVARIANCE: correction is driven by the internal hop -----
    //     grid, not the host block boundaries. Same input, every block size, same
    //     output. Signal is voiced (vibrato) + a transient, amount>0 so PSOLA runs.
    {
        const int N = (int) (1.0 * Fs);
        std::vector<float> x ((size_t) N);
        double ph = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double t = (double) i / Fs;
            const double f = 200.0 * std::pow (2.0, (40.0 / 1200.0) * std::sin (2.0 * kPi * 5.0 * t));
            ph += 2.0 * kPi * f / Fs;
            double v = 0.4 * std::sin (ph);
            if (i % (N / 4) == 0) v += 0.5;               // transients on the grid
            x[(size_t) i] = (float) v;
        }
        pf_core::PfParamSnapshot s = tightSnapshot();      // retune 5ms, tol 0 -> active
        const auto ref = runBlockSeq (Fs, x, s, { 512 });
        const std::vector<std::vector<int>> seqs = {
            { 64 }, { 127 }, { 2048 }, { 1 }, { 33, 512, 200, 1, 480, 65 }
        };
        for (const auto& seq : seqs)
        {
            const auto y = runBlockSeq (Fs, x, s, seq);
            double maxDiff = 0.0;
            for (int i = 0; i < N; ++i)
                maxDiff = std::max (maxDiff, std::abs ((double) y[(size_t) i] - (double) ref[(size_t) i]));
            if (maxDiff > 1.0e-4)
                fail ("block-size dependence: seq[0]=" + std::to_string (seq[0])
                      + " diff " + std::to_string (maxDiff) + " @" + std::to_string (Fs));
        }
    }

    // --- 11. LATENCY CONSISTENCY: a mid-stream Buffer change moves the REPORTED --
    //     latency (so the shell restarts) but NOT the actual DSP delay — the dry/
    //     wet alignment stays at the committed lookahead until the next prepare().
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        const int N = (int) (1.2 * Fs);
        auto x = makeNoise (N, 0.5f, 0x1A7E0u);
        std::vector<float> l (x), r (x);

        pf_core::PfParamSnapshot s;                        // amount 0 -> exact identity
        s.amount = 0.0f;
        s.buffer = 2;                                       // Normal
        const int half = N / 2;
        int pos = 0;
        while (pos < half)                                  // pos += m: end exactly at half
        {
            const int m = std::min (512, half - pos);
            core.process (l.data() + pos, r.data() + pos, m, s);
            pos += m;
        }

        const int committedL =
            (int) std::lround (pf_core::PfCore::kLookaheadPeriods[2] * Fs / 75.0);
        if (core.latencySamples() != committedL)
            fail ("pre-switch latency wrong @" + std::to_string (Fs));

        s.buffer = 0;                                       // Realtime — different lookahead
        while (pos < N)
        {
            const int m = std::min (512, N - pos);
            core.process (l.data() + pos, r.data() + pos, m, s);
            pos += m;
        }

        const int realtimeL =
            (int) std::lround (pf_core::PfCore::kLookaheadPeriods[0] * Fs / 75.0);
        // Reported latency followed the live param (so the shell will restart)...
        if (core.latencySamples() != realtimeL)
            fail ("reported latency did not follow Buffer change (" + std::to_string (core.latencySamples())
                  + " != " + std::to_string (realtimeL) + ") @" + std::to_string (Fs));
        // ...but the ACTUAL delay stayed committed: the whole stream is a pure
        // delay of the ORIGINAL (Normal) lookahead, NOT the Realtime one.
        double maxDiff = 0.0;
        for (int t = 0; t < N; ++t)
        {
            const float d = t >= committedL ? x[(size_t) (t - committedL)] : 0.0f;
            maxDiff = std::max (maxDiff, std::abs ((double) l[(size_t) t] - (double) d));
        }
        if (maxDiff > 1.0e-4)
            fail ("actual DSP latency shifted mid-stream (diff " + std::to_string (maxDiff)
                  + ") @" + std::to_string (Fs));
    }

    // --- 12. AMOUNT 0 IS A PURE DELAY (WITHIN TOL) ON VOICED MATERIAL -----------
    //     (not just noise): the voiced PSOLA path is bypassed at amount 0, so a
    //     clean sine and a vibrato tone pass through as a pure delay (tol 1e-4).
    {
        const int N = (int) (1.5 * Fs);
        std::vector<std::vector<float>> inputs;
        inputs.push_back (makeSine (N, Fs, 220.0, 0.5));   // steady voiced
        {
            std::vector<float> v ((size_t) N);             // vibrato voiced
            double ph = 0.0;
            for (int i = 0; i < N; ++i)
            {
                const double t = (double) i / Fs;
                const double f = 330.0 * std::pow (2.0, (60.0 / 1200.0) * std::sin (2.0 * kPi * 6.0 * t));
                ph += 2.0 * kPi * f / Fs;
                v[(size_t) i] = (float) (0.5 * std::sin (ph));
            }
            inputs.push_back (v);
        }
        for (const auto& x : inputs)
        {
            pf_core::PfCore core;
            core.prepare (Fs, 512);
            pf_core::PfParamSnapshot s;
            s.amount = 0.0f;
            auto y = run (core, x, s);
            const int L = core.latencySamples();
            double maxDiff = 0.0;
            for (int t = 0; t < N; ++t)
            {
                const float d = t >= L ? x[(size_t) (t - L)] : 0.0f;
                maxDiff = std::max (maxDiff, std::abs ((double) y[(size_t) t] - (double) d));
            }
            if (maxDiff > 1.0e-4)
                fail ("amount-0 voiced path not exact delay (diff " + std::to_string (maxDiff)
                      + ") @" + std::to_string (Fs));
        }
    }

    // --- 13. Key/Scale change drops a now-forbidden target note -----------------
    //     Input at 545 Hz sits between C5 (523.25) and C#5 (554.37), a touch
    //     closer to C# — so the target is UNAMBIGUOUS on each side of the change
    //     (no C/D equidistance tie). Chromatic snaps it up to C#; switching to C
    //     major (C# forbidden) must drop that held target and pull DOWN to C5.
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        const int N = (int) (4.0 * Fs);
        auto x = makeSine (N, Fs, 545.0, 0.5);
        std::vector<float> l (x), r (x);

        pf_core::PfParamSnapshot s = tightSnapshot();
        s.scale = 0;                                        // Chromatic: C# allowed
        const int half = N / 2;
        int pos = 0;
        while (pos < half)
        {
            const int m = std::min (512, half - pos);
            core.process (l.data() + pos, r.data() + pos, m, s);
            pos += m;
        }
        {
            std::vector<double> seg (l.begin() + (size_t) (Fs * 1.0), l.begin() + (size_t) (Fs * 2.0));
            const auto m = measureTone (seg, Fs);
            if (std::abs (centsBetween (m.freqHz, 554.365)) > 12.0)
                fail ("chromatic did not snap 545 -> C#5 (got " + std::to_string (m.freqHz)
                      + ") @" + std::to_string (Fs));
        }

        s.key = 0; s.scale = 1;                             // C major: C# forbidden
        while (pos < N)
        {
            const int m = std::min (512, N - pos);
            core.process (l.data() + pos, r.data() + pos, m, s);
            pos += m;
        }
        std::vector<double> seg (l.begin() + (size_t) (Fs * 3.0), l.end());
        const auto m = measureTone (seg, Fs);
        if (std::abs (centsBetween (m.freqHz, 523.251)) > 18.0)
            fail ("Key/Scale change kept a forbidden target (got " + std::to_string (m.freqHz)
                  + ", want ~523.25) @" + std::to_string (Fs));
    }

    // --- 14. reset() clears audio + tracking without changing latency -----------
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        pf_core::PfParamSnapshot s = tightSnapshot();
        auto tone = makeSine ((int) (0.6 * Fs), Fs, 330.0, 0.5);
        auto y = run (core, tone, s);
        const int latBefore = core.latencySamples();
        std::vector<double> yd (y.begin(), y.end());
        if (fct::peakAbs (yd) < 0.05)
            fail ("pre-reset output unexpectedly silent @" + std::to_string (Fs));

        core.reset();
        if (core.latencySamples() != latBefore)
            fail ("reset() changed the reported latency @" + std::to_string (Fs));

        // Post-reset silence must come out silent immediately: no stale tone lingers
        // in the dry ring / OLA accumulator for the lookahead span (the seek bleed).
        const int M = latBefore + (int) (0.05 * Fs);
        std::vector<float> sil ((size_t) M, 0.0f);
        auto ys = run (core, sil, s);
        std::vector<double> ysd (ys.begin(), ys.end());
        if (fct::peakAbs (ysd) > 1.0e-3)
            fail ("reset() left stale audio (peak " + std::to_string (fct::peakAbs (ysd))
                  + ") @" + std::to_string (Fs));
    }

    // --- 15. Max Pitch is respected: a tone above it is out of band (unvoiced) ---
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        const int N = (int) (1.2 * Fs);
        auto x = makeSine (N, Fs, 500.0, 0.5);              // 500 Hz
        pf_core::PfParamSnapshot s;
        s.amount     = 100.0f;
        s.minPitchHz = 300.0f;
        s.maxPitchHz = 400.0f;                              // 500 > max -> not detected
        auto y = run (core, x, s);
        const int L = core.latencySamples();
        double maxDiff = 0.0;
        for (int t = 0; t < N; ++t)
        {
            const float d = t >= L ? x[(size_t) (t - L)] : 0.0f;
            maxDiff = std::max (maxDiff, std::abs ((double) y[(size_t) t] - (double) d));
        }
        if (maxDiff > 1.0e-4)
            fail ("tone above Max Pitch was corrected (band widened past the UI value; diff "
                  + std::to_string (maxDiff) + ") @" + std::to_string (Fs));
    }

    // --- 16. sub-Min-Pitch energy must not destroy detection --------------------
    // A male fundamental sits at 80-160 Hz, so the source is never high-passed
    // hard and proximity build-up / stage rumble rides underneath it. Such a
    // component does not merely bias the NSDF: when its period exceeds the whole
    // lag range the NSDF decays monotonically across [lagMin, lagMax], MPM finds
    // no local maximum, and the frame comes back UNVOICED — the correction stops
    // dead on perfectly good voiced material.
    //
    // Oracle is music theory: a 41-cent-sharp G2 contaminated with a 38 Hz tone
    // 9 dB below it — well under Min Pitch, so the detector may reject it but
    // nothing may filter it out of the AUDIO — must still be pulled onto G2.
    // At this level the pre-fix core corrected NOTHING (output stayed at the
    // +41 ct input); the tolerance below is set to separate "corrected" from
    // "not corrected at all", not to pin the residual accuracy.
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        s.minPitchHz = 75.0f;
        const double g2   = 98.0;                            // G2, ET at A4=440
        const double fIn  = 98.0 * std::pow (2.0, 41.0 / 1200.0);
        const int    N    = (int) (4.0 * Fs);
        auto x = makeSine (N, Fs, fIn, 0.45);
        const auto rumble = makeSine (N, Fs, 38.0, 0.45 * std::pow (10.0, -9.0 / 20.0));
        for (int i = 0; i < N; ++i)
            x[(size_t) i] = (float) (x[(size_t) i] + rumble[(size_t) i]);

        auto y = run (core, x, s);

        // The plugin must not filter the AUDIO, so the rumble is still in the
        // output and would win a plain peak-pick. Strip it in the MEASUREMENT
        // instrument (a test-side 4th-order Butterworth at 60 Hz, well below G2)
        // and read the surviving tone with the same rate-following analysis FFT
        // used everywhere else: its parabolic interpolation resolves the 2.4 Hz
        // between G2 and the uncorrected input pitch far inside one bin.
        auto seg = tailOf (y, Fs, 2.0);
        {
            factory_core::Biquad m1, m2;
            m1.setCoeffs (factory_core::designHpLpStage (
                factory_core::BandType::HighPass, 60.0, 0.70710678118654752440, 0, 2, Fs));
            m2.setCoeffs (factory_core::designHpLpStage (
                factory_core::BandType::HighPass, 60.0, 0.70710678118654752440, 1, 2, Fs));
            for (auto& v : seg)
                v = m2.processSample (m1.processSample (v));
        }
        const auto m = measureTone (seg, Fs);
        if (std::abs (centsBetween (m.freqHz, g2)) > 20.0)
            fail ("sub-Min-Pitch energy killed detection: got " + std::to_string (m.freqHz)
                  + " Hz, want " + std::to_string (g2) + " (input was "
                  + std::to_string (fIn) + ") @" + std::to_string (Fs));
    }

    // --- 17. PSOLA budget: no silently-uncorrected band at the bottom -----------
    // PsolaShifter demands 2*P + P/4 + 4 <= lookahead for a VOICED grain and
    // degrades to its unvoiced IDENTITY path otherwise — silently, so the pitch
    // still shows in the UI while nothing is corrected. The lookahead table is
    // written in periods of Min Pitch, so the guarantee only holds while the
    // tracked period stays <= Fs/minPitch.
    //
    // The invariant that keeps the two in step is: ANY pitch the core reports as
    // tracked must be one the shifter can actually correct. PitchDetector accepts
    // down to 0.9*Min Pitch, which at Realtime (2.35 periods) needs 2.5 — so the
    // pre-fix core tracked and DISPLAYED roughly a semitone at the bottom of the
    // range that it then quietly refused to correct. Sweep across that boundary
    // and assert the invariant directly against the reported latency, so it stays
    // formula-independent (no re-derivation of the mode table here).
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            const double minPitch = 75.0;
            for (double mult : { 0.86, 0.90, 0.93, 0.97, 1.0, 1.1, 1.4 })
            {
                pf_core::PfCore core;
                core.prepare (Fs, 512);
                auto s = tightSnapshot();
                s.buffer     = mode;
                s.minPitchHz = (float) minPitch;
                auto x = makeSine ((int) (1.5 * Fs), Fs, minPitch * mult, 0.5);
                std::vector<float> l (x), r (x);
                for (int pos = 0; pos < (int) x.size(); pos += 512)
                {
                    const int m = std::min (512, (int) x.size() - pos);
                    core.process (l.data() + pos, r.data() + pos, m, s);
                    const double det = core.uiDetectedHz.load();
                    if (det <= 0.0)
                        continue;
                    const double needed =
                        pf_core::PfCore::kPsolaBudgetPeriods * Fs / det + 4.0;
                    if (needed > (double) core.latencySamples())
                        fail ("mode " + std::to_string (mode) + ": tracked " + std::to_string (det)
                              + " Hz needs " + std::to_string (needed)
                              + " samples of lookahead but only "
                              + std::to_string (core.latencySamples())
                              + " is reported — correction silently disabled @"
                              + std::to_string (Fs));
                }
            }
        }

        // And the end-to-end consequence at the worst case the budget must cover:
        // a tone AT Min Pitch has to be really corrected in every mode, not passed
        // through. Oracle: music theory (a 40-cent-sharp D2 must land on D2).
        for (int mode = 0; mode < 4; ++mode)
        {
            pf_core::PfCore core;
            core.prepare (Fs, 512);
            auto s = tightSnapshot();
            s.buffer     = mode;
            s.minPitchHz = 73.416f;                     // D2
            const double fIn = 73.416 * std::pow (2.0, 40.0 / 1200.0);
            auto y = run (core, makeSine ((int) (4.0 * Fs), Fs, fIn, 0.5), s);
            const auto m = measureTone (tailOf (y, Fs, 2.5), Fs);
            if (std::abs (centsBetween (m.freqHz, 73.416)) > 12.0)
                fail ("mode " + std::to_string (mode)
                      + ": tone at Min Pitch left uncorrected (got "
                      + std::to_string (m.freqHz) + " Hz, want 73.416) @"
                      + std::to_string (Fs));
        }
    }

    // --- 18. a wrong note at the onset must not own the whole note -------------
    // targetNote is latched from the first voiced hop with NO hysteresis and then
    // DEFENDED by it. The onset is the least reliable frame in the note (its
    // analysis window is still half attack transient), so one bad frame could own
    // everything after it: latched onto A#4, a steady 49-cent-sharp A4 can never
    // leave, because defending A#4 only costs 49 + 18 < 51.
    //
    // Reproduce it deterministically — 12 ms of A#4 (inside the settling window),
    // then a steady A4 + 49 ct. Oracle is music theory: the sustained pitch is
    // nearer A4, so that is where it must land.
    {
        pf_core::PfCore core;
        core.prepare (Fs, 512);
        auto s = tightSnapshot();
        const int    nPre = (int) (0.012 * Fs);
        const int    N    = (int) (3.0 * Fs);
        const double fPre = 440.0 * std::pow (2.0, 100.0 / 1200.0);   // A#4
        const double fIn  = 440.0 * std::pow (2.0, 49.0 / 1200.0);

        std::vector<float> x ((size_t) N);
        double ph = 0.0;
        for (int i = 0; i < N; ++i)
        {
            ph += 2.0 * kPi * (i < nPre ? fPre : fIn) / Fs;
            x[(size_t) i] = (float) (0.5 * std::sin (ph));            // phase-continuous
        }

        auto y = run (core, x, s);
        const auto m = measureTone (tailOf (y, Fs, 1.5), Fs);
        if (std::abs (centsBetween (m.freqHz, 440.0)) > 10.0)
            fail ("a wrong onset note was latched for the whole note: got "
                  + std::to_string (m.freqHz) + " Hz (want 440) @" + std::to_string (Fs));
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
