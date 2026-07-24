//
// core/tests/pitch_detector_test.cpp — spec tests for factory_core::PitchDetector
// (McLeod/NSDF monophonic f0 estimator).
//
// Oracle: the generator. Every frame is synthesised at an exactly known
// fundamental (sine, band-limited sawtooth, missing-fundamental harmonic
// stack), so the expected f0 is independent of the implementation. Accuracy is
// asserted in CENTS (rate-independent), across the full standard sample-rate
// matrix; the analysis window is derived from the rate (periods of the lowest
// detectable pitch), so a rate-blind implementation fails the high-rate cases.
//
// Regression-policy gates: absolute silence floor (no phantom pitch on silence
// or sub-floor level), no voiced verdict on white noise, no octave errors on
// harmonic-rich material.
//
#include "factory_core/PitchDetector.h"
#include "factory_core/testing/DspInvariants.h"

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

// Deterministic white noise in [-amp, amp] (LCG — no <random> in tests).
static std::vector<float> makeNoise (int n, float amp, unsigned seed = 0x1234567u)
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

// Band-limited sawtooth: sum of sin(2*pi*k*f*t)/k below 0.45*fs.
static std::vector<float> makeSaw (int n, double fs, double f, double amp)
{
    std::vector<float> v ((size_t) n, 0.0f);
    const int K = (int) std::floor (0.45 * fs / f);
    for (int k = 1; k <= K; ++k)
        for (int i = 0; i < n; ++i)
            v[(size_t) i] += (float) (amp * std::sin (2.0 * kPi * k * f * (double) i / fs) / (double) k);
    return v;
}

// Harmonics 2..5 of f only — the missing-fundamental stack (telephone voice).
static std::vector<float> makeMissingFundamental (int n, double fs, double f, double amp)
{
    std::vector<float> v ((size_t) n, 0.0f);
    for (int k = 2; k <= 5; ++k)
        for (int i = 0; i < n; ++i)
            v[(size_t) i] += (float) (amp * std::sin (2.0 * kPi * k * f * (double) i / fs) / (double) k);
    return v;
}

static void detectorTests (double Fs)
{
    factory_core::PitchDetector det;
    det.prepare (Fs, 25.0, 3.5);   // worst case the pitch-fix core uses

    const double kThresh = 0.80;

    // --- 1. sine accuracy over the vocal range (window: 3 periods of 70 Hz) ---
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        const double freqs[] = { 82.41, 110.0, 146.83, 196.0, 220.0, 261.63,
                                 329.63, 440.0, 587.33, 880.0, 1174.66 };
        for (double f : freqs)
        {
            auto x = makeSine (W, Fs, f, 0.5);
            const auto e = det.estimate (x.data(), W, 70.0, 1600.0, kThresh);
            if (! e.voiced)
                fail ("sine " + std::to_string (f) + " Hz not voiced @" + std::to_string (Fs));
            else if (std::abs (centsBetween (e.f0Hz, f)) > 3.0)
                fail ("sine " + std::to_string (f) + " Hz off by "
                      + std::to_string (centsBetween (e.f0Hz, f)) + " cents @" + std::to_string (Fs));
        }
    }

    // --- 2. low range down to the parameter floor (25 Hz) --------------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 25.0);
        for (double f : { 27.5, 41.2, 55.0 })
        {
            auto x = makeSine (W, Fs, f, 0.5);
            const auto e = det.estimate (x.data(), W, 25.0, 400.0, kThresh);
            if (! e.voiced || std::abs (centsBetween (e.f0Hz, f)) > 3.0)
                fail ("low sine " + std::to_string (f) + " Hz failed @" + std::to_string (Fs));
        }
    }

    // --- 3. high range --------------------------------------------------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 200.0);
        for (double f : { 1046.5, 1567.98, 2093.0 })
        {
            auto x = makeSine (W, Fs, f, 0.5);
            const auto e = det.estimate (x.data(), W, 200.0, 4000.0, kThresh);
            if (! e.voiced || std::abs (centsBetween (e.f0Hz, f)) > 3.0)
                fail ("high sine " + std::to_string (f) + " Hz failed @" + std::to_string (Fs));
        }
    }

    // --- 4. harmonic-rich material: no octave errors --------------------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        for (double f : { 110.0, 220.0, 293.66 })
        {
            auto x = makeSaw (W, Fs, f, 0.4);
            const auto e = det.estimate (x.data(), W, 70.0, 1600.0, kThresh);
            if (! e.voiced || std::abs (centsBetween (e.f0Hz, f)) > 5.0)
                fail ("saw " + std::to_string (f) + " Hz failed (got "
                      + std::to_string (e.f0Hz) + ") @" + std::to_string (Fs));
        }
    }

    // --- 5. missing fundamental (harmonics 2..5 only) --------------------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        auto x = makeMissingFundamental (W, Fs, 220.0, 0.4);
        const auto e = det.estimate (x.data(), W, 70.0, 1600.0, kThresh);
        if (! e.voiced || std::abs (centsBetween (e.f0Hz, 220.0)) > 5.0)
            fail ("missing-fundamental 220 Hz failed (got "
                  + std::to_string (e.f0Hz) + ") @" + std::to_string (Fs));
    }

    // --- 6. amplitude invariance above the floor, floor below it ---------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        auto quiet = makeSine (W, Fs, 440.0, 0.01);            // -40 dBFS
        const auto e1 = det.estimate (quiet.data(), W, 70.0, 1600.0, kThresh);
        if (! e1.voiced || std::abs (centsBetween (e1.f0Hz, 440.0)) > 3.0)
            fail ("-40 dB sine not detected @" + std::to_string (Fs));

        auto subFloor = makeSine (W, Fs, 440.0, 5.0e-5);       // below kPowerFloor
        const auto e2 = det.estimate (subFloor.data(), W, 70.0, 1600.0, kThresh);
        if (e2.voiced || e2.f0Hz != 0.0)
            fail ("sub-floor sine produced phantom pitch @" + std::to_string (Fs));
    }

    // --- 7. silence & noise stay unvoiced (absolute floor / clarity) -----------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        std::vector<float> zeros ((size_t) W, 0.0f);
        const auto ez = det.estimate (zeros.data(), W, 70.0, 1600.0, kThresh);
        if (ez.voiced || ez.f0Hz != 0.0)
            fail ("silence produced phantom pitch @" + std::to_string (Fs));

        auto noise = makeNoise (W, 0.3f);
        const auto en = det.estimate (noise.data(), W, 70.0, 1600.0, kThresh);
        if (en.voiced)
            fail ("white noise judged voiced (clarity " + std::to_string (en.clarity)
                  + ") @" + std::to_string (Fs));
        if (en.clarity > 0.5)
            fail ("white noise clarity too high @" + std::to_string (Fs));
    }

    // --- 8. clarity is a meaningful confidence ---------------------------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        auto x = makeSine (W, Fs, 440.0, 0.5);
        const auto e = det.estimate (x.data(), W, 70.0, 1600.0, kThresh);
        if (e.clarity < 0.95)
            fail ("clean sine clarity below 0.95 @" + std::to_string (Fs));
    }

    // --- 9. vibrato stays voiced and inside the excursion band -----------------
    {
        const int W = (int) std::ceil (3.0 * Fs / 70.0);
        std::vector<float> v ((size_t) W);
        double ph = 0.0;
        for (int i = 0; i < W; ++i)
        {
            const double t = (double) i / Fs;
            const double f = 440.0 * std::pow (2.0, (50.0 / 1200.0) * std::sin (2.0 * kPi * 6.0 * t));
            ph += 2.0 * kPi * f / Fs;
            v[(size_t) i] = (float) (0.5 * std::sin (ph));
        }
        const auto e = det.estimate (v.data(), W, 70.0, 1600.0, kThresh);
        if (! e.voiced || e.f0Hz < 415.0 || e.f0Hz > 466.0)
            fail ("vibrato sine outside band (got " + std::to_string (e.f0Hz)
                  + ") @" + std::to_string (Fs));
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        detectorTests (Fs);

    if (g_failures > 0)
    {
        std::printf ("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf ("pitch_detector_test OK\n");
    return 0;
}
