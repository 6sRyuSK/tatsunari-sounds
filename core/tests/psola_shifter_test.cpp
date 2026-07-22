//
// core/tests/psola_shifter_test.cpp — spec tests for factory_core::PsolaShifter
// (pitch-synchronous overlap-add shifter).
//
// Oracles (all independent of the implementation):
//   * IDENTITY: with an unvoiced track and ratio 1 the spec says the output is
//     a PURE DELAY of exactly latencySamples() — compared sample-by-sample
//     against the input itself. This nails the latency contract to the sample.
//   * SHIFT: a synthesised sine at a known f0, shifted by ratio r, must come
//     out as a tone at f0*r — measured with an analysis FFT whose order comes
//     from fftOrderForSampleRate (resolution follows the rate), peak-picked
//     with parabolic interpolation. Residual (everything but the target tone)
//     is bounded — the "audibly clean" spec floor, not a value derived from
//     the implementation.
//   * Unity-gain: the raised-cosine overlap normalisation keeps RMS within
//     ±0.15 of the input for correction-range ratios.
//   * Worst case: a period exceeding the lookahead budget must degrade to the
//     delayed-identity path (never read unwritten input, never blow up).
//
#include "factory_core/PsolaShifter.h"
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

struct ToneMeasure
{
    double freqHz = 0.0;
    double snrDb  = 0.0;
    double rms    = 0.0;
};

// Analysis-FFT tone measurement (Hann window, parabolic peak interpolation).
// The order follows the sample rate so the bin width — and thus the frequency
// resolution of this oracle — is rate-independent (~1.5 Hz).
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
        if (p[(size_t) k] > p[(size_t) kMaxBin])
            kMaxBin = k;

    double delta = 0.0;
    if (kMaxBin > 3 && kMaxBin < half - 1 && p[(size_t) kMaxBin] > 0.0)
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
        if (std::abs (k - kMaxBin) <= 3)
            peak += p[(size_t) k];
    }
    out.snrDb = 10.0 * std::log10 (peak / std::max (tot - peak, 1.0e-30));
    return out;
}

static double centsBetween (double a, double b)
{
    return 1200.0 * std::log2 (a / b);
}

static void shifterTests (double Fs)
{
    const int maxBlock  = 512;
    const int maxPeriod = (int) std::ceil (Fs / 50.0) + 4;
    const int maxLook   = (int) std::ceil (0.08 * Fs);

    // --- 1. unvoiced identity: exact pure delay across irregular block sizes ---
    {
        factory_core::PsolaShifter sh;
        sh.prepare (Fs, maxBlock, maxLook, maxPeriod);
        const int L = (int) std::ceil (0.045 * Fs);
        sh.setLookahead (L);
        if (sh.latencySamples() != L)
            fail ("latencySamples != setLookahead @" + std::to_string (Fs));

        const int N = (int) (1.5 * Fs);
        auto xl = makeNoise (N, 0.5f, 0xBEEFu);
        auto xr = makeNoise (N, 0.5f, 0xCAFEu);
        std::vector<float> yl (xl), yr (xr);

        const int sizes[] = { 512, 33, 257, 128, 480, 64 };
        int pos = 0, si = 0;
        while (pos < N)
        {
            const int m = std::min (sizes[si++ % 6], N - pos);
            sh.setTrack (Fs / 123.0, false);   // period argument must be ignored
            sh.setRatio (1.0);
            sh.process (yl.data() + pos, yr.data() + pos, m);
            pos += m;
        }

        double maxDiff = 0.0;
        for (int t = 0; t < N; ++t)
        {
            const float dl = t >= L ? xl[(size_t) (t - L)] : 0.0f;
            const float dr = t >= L ? xr[(size_t) (t - L)] : 0.0f;
            maxDiff = std::max ({ maxDiff,
                                  std::abs ((double) yl[(size_t) t] - (double) dl),
                                  std::abs ((double) yr[(size_t) t] - (double) dr) });
        }
        if (maxDiff > 1.0e-4)
            fail ("unvoiced identity deviates from pure delay by "
                  + std::to_string (maxDiff) + " @" + std::to_string (Fs));
    }

    // --- 2. voiced sine: shift lands on f0*ratio, clean and unity-gain ---------
    {
        const double f0 = 220.0;
        const double ratios[] = { std::pow (2.0, -100.0 / 1200.0),   // -1 st
                                  1.0,
                                  std::pow (2.0,   50.0 / 1200.0),   // +0.5 st
                                  std::pow (2.0,  100.0 / 1200.0) }; // +1 st
        for (double r : ratios)
        {
            factory_core::PsolaShifter sh;
            sh.prepare (Fs, maxBlock, maxLook, maxPeriod);
            sh.setLookahead ((int) std::ceil (0.045 * Fs));

            const int N = (int) (2.2 * Fs);
            std::vector<float> yl ((size_t) N), yr ((size_t) N);
            for (int i = 0; i < N; ++i)
                yl[(size_t) i] = yr[(size_t) i]
                    = (float) (0.5 * std::sin (2.0 * kPi * f0 * (double) i / Fs));

            for (int pos = 0; pos < N; pos += maxBlock)
            {
                const int m = std::min (maxBlock, N - pos);
                sh.setTrack (Fs / f0, true);
                sh.setRatio (r);
                sh.process (yl.data() + pos, yr.data() + pos, m);
            }

            // Stereo lock: identical channels in, bit-identical channels out.
            double chDiff = 0.0;
            for (int t = 0; t < N; ++t)
                chDiff = std::max (chDiff, std::abs ((double) yl[(size_t) t] - (double) yr[(size_t) t]));
            if (chDiff > 1.0e-9)
                fail ("stereo channels diverged (" + std::to_string (chDiff)
                      + ") ratio " + std::to_string (r) + " @" + std::to_string (Fs));

            std::vector<double> tail;
            tail.reserve ((size_t) N);
            for (int t = (int) (0.5 * Fs); t < N; ++t)
                tail.push_back ((double) yl[(size_t) t]);
            const auto m = measureTone (tail, Fs);

            const double expected = f0 * r;
            if (std::abs (centsBetween (m.freqHz, expected)) > 6.0)
                fail ("shifted freq " + std::to_string (m.freqHz) + " != "
                      + std::to_string (expected) + " (ratio " + std::to_string (r)
                      + ") @" + std::to_string (Fs));

            const double snrFloor = (r == 1.0) ? 25.0 : 20.0;
            if (m.snrDb < snrFloor)
                fail ("shift SNR " + std::to_string (m.snrDb) + " dB < "
                      + std::to_string (snrFloor) + " (ratio " + std::to_string (r)
                      + ") @" + std::to_string (Fs));

            const double inRms = 0.5 / std::sqrt (2.0);
            if (std::abs (m.rms / inRms - 1.0) > 0.15)
                fail ("RMS drift " + std::to_string (m.rms / inRms) + " (ratio "
                      + std::to_string (r) + ") @" + std::to_string (Fs));
        }
    }

    // --- 3. worst case: period over the lookahead budget degrades to identity --
    {
        factory_core::PsolaShifter sh;
        sh.prepare (Fs, maxBlock, maxLook, maxPeriod);
        const int L = (int) std::ceil (2.4 * Fs / 70.0);
        sh.setLookahead (L);

        const int N = (int) (1.0 * Fs);
        auto x = makeNoise (N, 0.5f, 0xF00Du);
        std::vector<float> y (x);
        for (int pos = 0; pos < N; pos += maxBlock)
        {
            const int m = std::min (maxBlock, N - pos);
            sh.setTrack (Fs / 40.0, true);   // 2*P ≈ Fs/20 > L: must degrade
            sh.setRatio (1.12);
            sh.process (y.data() + pos, nullptr, m);
        }
        double maxDiff = 0.0;
        for (int t = 0; t < N; ++t)
        {
            const float d = t >= L ? x[(size_t) (t - L)] : 0.0f;
            maxDiff = std::max (maxDiff, std::abs ((double) y[(size_t) t] - (double) d));
        }
        if (maxDiff > 1.0e-4)
            fail ("over-budget period did not degrade to identity (diff "
                  + std::to_string (maxDiff) + ") @" + std::to_string (Fs));
    }

    // --- 4. torture: ratio random-walk on rich material stays finite/bounded ---
    {
        factory_core::PsolaShifter sh;
        sh.prepare (Fs, maxBlock, maxLook, maxPeriod);
        sh.setLookahead ((int) std::ceil (0.05 * Fs));

        const int N = (int) (3.0 * Fs);
        std::vector<float> y ((size_t) N);
        double inPeak = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double t = (double) i / Fs;
            double s = 0.0;                       // band-limited-ish saw at 150 Hz
            for (int k = 1; k <= 8; ++k)
                s += std::sin (2.0 * kPi * 150.0 * k * t) / (double) k;
            const double am = 0.6 + 0.4 * std::sin (2.0 * kPi * 3.0 * t);
            y[(size_t) i] = (float) (0.25 * am * s);
            inPeak = std::max (inPeak, std::abs ((double) y[(size_t) i]));
        }

        unsigned seed = 0xA5A5A5u;
        double ratio = 1.0;
        for (int pos = 0; pos < N; pos += maxBlock)
        {
            const int m = std::min (maxBlock, N - pos);
            seed = seed * 1664525u + 1013904223u;
            const double rnd = (double) (seed >> 8) / 16777216.0;
            ratio = std::clamp (ratio + (rnd - 0.5) * 0.2, 0.7, 1.5);
            const bool voiced = ((seed >> 4) & 7u) != 0;   // occasionally unvoiced
            sh.setTrack (Fs / 150.0, voiced);
            sh.setRatio (ratio);
            sh.process (y.data() + pos, nullptr, m);
        }

        std::vector<double> yd (y.begin(), y.end());
        if (! fct::allFinite (yd))
            fail ("torture output not finite @" + std::to_string (Fs));
        if (fct::peakAbs (yd) > 1.5 * inPeak + 1.0e-6)
            fail ("torture peak " + std::to_string (fct::peakAbs (yd))
                  + " exceeds 1.5x input peak @" + std::to_string (Fs));
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        shifterTests (Fs);

    if (g_failures > 0)
    {
        std::printf ("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf ("psola_shifter_test OK\n");
    return 0;
}
