// Host-side (no visage / no JUCE) unit check for SpectrumModel — the JUCE-free
// spectrum maths (FFT via factory_core::FFT).
//
//   c++ -std=c++17 -I ../include -I ../../../core/include  (cont.)
//       spectrum_model_test.cpp ../src/SpectrumModel.cpp -o t && ./t [<fs>]
//
// SPEC + INDEPENDENT ORACLE (oracle rules, CLAUDE.md):
//   Feed a full-scale sinusoid placed EXACTLY on an FFT bin (f = k·fs/N) through
//   the model at every standard sample rate. The expected outputs are derived
//   analytically, NOT read from the model:
//     * peak bin  = k                    (the sinusoid's bin; argmax must land there)
//     * peak dB   = 20·log10(A · CG)      where CG = Hann coherent gain = 0.5
//         (textbook: an on-bin sinusoid of amplitude A through a Hann window, in
//          the |X|/(N/2) normalisation, reads A·CG. CG is verified independently
//          from the window sum, and cross-checked against the analytic 0.5.)
//     * log-x     = LogFreqAxis(f)        (independent closed-form log mapping)
//   Plus the resolution-follows-rate invariant (CLAUDE.md): bin width fs/N stays
//   bounded across 44.1–192 kHz (a fixed order would blow it up at high rates).
#include "factory_ui_visage/SpectrumModel.h"
#include "factory_core/StftResolution.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using factory_ui_visage::SpectrumModel;
using factory_ui_visage::LogFreqAxis;

static int g_failures = 0;
static void check (const std::string& name, bool ok, const std::string& detail = "")
{
    std::printf ("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                 detail.empty() ? "" : "  -> ", detail.c_str());
    if (! ok) ++g_failures;
}

static constexpr double kPi = 3.14159265358979323846;

int main (int argc, char** argv)
{
    const std::vector<double> rates = factory_core::testing::sampleRatesFromArgs (argc, argv);
    const float A = 1.0f; // full-scale sinusoid amplitude

    for (double fs : rates)
    {
        const std::string at = "  @" + std::to_string ((int) fs) + "Hz";

        SpectrumModel model;
        model.setOrderForSampleRate (fs);
        const int N = model.size();
        const int order = model.order();

        // Sanity: order tracks the rate exactly as StftResolution prescribes.
        check ("order == fftOrderForSampleRate" + at,
               order == factory_core::fftOrderForSampleRate (fs),
               "order " + std::to_string (order) + " N " + std::to_string (N));

        // Resolution-follows-rate invariant: bin width stays bounded (a fixed
        // order would give ~94 Hz bins at 192 kHz — issue #16).
        const double binWidth = fs / N;
        check ("bin width bounded (<30 Hz)" + at, binWidth < 30.0,
               std::to_string (binWidth) + " Hz");

        // Place the sinusoid EXACTLY on a mid-band bin near 2 kHz.
        const int k = (int) std::lround (2000.0 * N / fs);
        const float f = (float) (k * fs / N); // on-bin frequency
        check ("test bin in analysed half" + at, k >= 1 && 2 * k < N / 2,
               "k " + std::to_string (k) + " f " + std::to_string (f) + "Hz");

        // Generate exactly N samples of A·cos(2π k n / N) and feed them.
        std::vector<float> frame ((std::size_t) N);
        for (int n = 0; n < N; ++n)
            frame[(std::size_t) n] = A * (float) std::cos (2.0 * kPi * k * n / N);
        model.writeSamples (frame.data(), N);
        model.update (fs); // default options (tilt 0)

        // --- oracle 1: independent Hann coherent gain from the window sum -------
        double windowSum = 0.0;
        for (int n = 0; n < N; ++n)
            windowSum += 0.5 * (1.0 - std::cos (2.0 * kPi * n / N));
        const double cg = windowSum / N;
        check ("Hann coherent gain == 0.5" + at, std::abs (cg - 0.5) < 1e-6,
               std::to_string (cg));

        const float expectedDb = 20.0f * std::log10 (A * (float) cg); // ≈ -6.0206 dB

        // --- assertion: peak bin is k ------------------------------------------
        int argmax = 0;
        float maxDb = model.smoothedDb (0);
        for (int bin = 1; bin < model.numBins(); ++bin)
            if (model.smoothedDb (bin) > maxDb) { maxDb = model.smoothedDb (bin); argmax = bin; }
        check ("peak bin == k" + at, argmax == k,
               "argmax " + std::to_string (argmax) + " expected " + std::to_string (k));

        // --- assertion: peak dB matches the analytic level ---------------------
        check ("peak dB == 20log10(A·CG)" + at,
               std::abs (model.peakDb (k) - expectedDb) < 0.15f,
               "got " + std::to_string (model.peakDb (k)) + " expected " + std::to_string (expectedDb));
        // On the first update, fast-up is instant, so smoothed == peak at the bin.
        check ("smoothed == peak at k" + at,
               std::abs (model.smoothedDb (k) - model.peakDb (k)) < 1e-4f);

        // --- assertion: bin frequency + log-x position -------------------------
        check ("binFrequency(k) == f" + at,
               std::abs (SpectrumModel::binFrequency (k, fs, N) - f) < 0.01f,
               std::to_string (SpectrumModel::binFrequency (k, fs, N)));

        LogFreqAxis axis { 0.0f, 1000.0f };
        const float xModel  = axis.freqToX (SpectrumModel::binFrequency (argmax, fs, N));
        const float xInline = 1000.0f * (float) (std::log (f / 20.0f) / std::log (1000.0f)); // independent
        check ("peak log-x in plot" + at, xModel >= 0.0f && xModel <= 1000.0f, std::to_string (xModel));
        check ("peak log-x == closed-form" + at, std::abs (xModel - xInline) < 0.05f,
               "axis " + std::to_string (xModel) + " inline " + std::to_string (xInline));
    }

    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
