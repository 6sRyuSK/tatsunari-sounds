#pragma once

#include <complex>
#include <vector>

#include "factory_core/FFT.h"

//
// factory_ui_visage::SpectrumModel — the JUCE-free maths behind the spectrum
// display, ported from ui/include/factory_ui/SpectrumAnalyzerModel.h. Same house
// constants (smoothing 0.25 fast-up/slow-down, peak fall 0.6 dB/frame, −120 dB
// floor, optional dB/oct tilt) and Hann window, but the transform is
// factory_core::FFT (core/include/factory_core/FFT.h) instead of juce::dsp::FFT,
// so this is visage-free AND JUCE-free and can be exercised headless with a host
// compiler (like Theme).
//
// It is a PURE MODEL — no drawing. Input is a mono float stream written into an
// internal ring (`writeSamples`, the "pull" source); `update()` pulls the most
// recent size() samples, windows + FFTs them, and folds the magnitudes into the
// per-bin smoothed-dB and peak-hold arrays that SpectrumView reads. GUI-thread
// only; every buffer is preallocated in setup().
//
// The FFT order follows the sample rate via factory_core::fftOrderForSampleRate
// (a fixed order is forbidden by CLAUDE.md — it silently loses resolution at high
// rates), so bin width (fs / N) and window length (N / fs) stay ~constant.
//
namespace factory_ui_visage
{
    // --- pure geometry (JUCE-free, visage-free) — shared by SpectrumView + tests.

    // 20 Hz – 20 kHz logarithmic frequency axis (three decades over `width`), the
    // standard analyser mapping (ported from factory_ui::LogFreqAxis).
    struct LogFreqAxis
    {
        float x     = 0.0f;
        float width = 1.0f;

        static constexpr float kMinHz = 20.0f;
        static constexpr float kMaxHz = 20000.0f;
        static constexpr float kSpan  = 1000.0f; // kMinHz * kSpan == kMaxHz

        float freqToX (float f) const;
        float xToFreq (float px) const;
    };

    // Linear value→Y mapping (ported from factory_ui::VerticalAxis): `topValue` at
    // the plot top edge, `bottomValue` at the foot; values clamp to that range.
    struct VerticalAxis
    {
        float y      = 0.0f;
        float height = 1.0f;
        float topValue    = 0.0f;
        float bottomValue = -100.0f;

        float toY (float v) const;
    };

    // Per-frame options; defaults match the house analyser. Kept at namespace
    // scope (not nested) because a nested struct with default member initializers
    // cannot be used as a defaulted argument (`const Options& = {}`) — GCC rejects
    // it exactly as noted in factory_ui::SpectrumAnalyzerModel. SpectrumModel::
    // Options aliases this for call sites.
    struct SpectrumOptions
    {
        float smoothing    = 0.25f;   // lerp toward the instantaneous value when falling
        float peakFallDb   = 0.6f;    // peak-hold decay per frame
        float floorDb      = -120.0f; // gain→dB floor
        float tiltDbPerOct = 0.0f;    // display tilt (e.g. +3 dB/oct to flatten pink)
    };

    class SpectrumModel
    {
    public:
        using Options = SpectrumOptions;

        // (Re)allocate for `fftOrder`, resetting smoothed/peak state to the floor
        // and clearing the input ring. A no-op when the order is unchanged.
        void setup (int fftOrder);

        // Pick the order from the sample rate (factory_core::fftOrderForSampleRate)
        // and setup() it. Call whenever the rate changes.
        void setOrderForSampleRate (double sampleRate);

        int order()   const noexcept { return order_; }
        int size()    const noexcept { return size_; }
        int numBins() const noexcept { return size_ / 2; }

        // Clear the input ring and reset every bin to the floor.
        void reset();

        // Append mono samples into the ring (wraps). The pull source: whatever the
        // caller has (a processor's lock-free ring copy, or the gallery's synthetic
        // generator) feeds frames here between update() calls.
        void writeSamples (const float* data, int numSamples);

        // Pull the most recent size() samples, window + FFT, and fold the
        // magnitudes into the smoothed + peak-held dB state for the given rate.
        void update (double sampleRate, const Options& opt = {});

        float smoothedDb (int bin) const noexcept { return smoothed_[(std::size_t) bin]; }
        float peakDb     (int bin) const noexcept { return peak_[(std::size_t) bin]; }

        // Centre frequency of an FFT bin.
        static float binFrequency (int bin, double sampleRate, int fftSize) noexcept
        {
            return (float) ((double) bin * sampleRate / fftSize);
        }

    private:
        static constexpr float kFloorInit = -120.0f;

        int order_ = 0;
        int size_  = 0;

        factory_core::FFT fft_;
        std::vector<double>               window_;  // periodic Hann, length size_
        std::vector<std::complex<double>> scratch_; // length size_ (FFT in place)
        std::vector<float>                ring_;     // capacity size_ (mono input)
        int                               writePos_ = 0;
        std::vector<float> smoothed_, peak_;         // length size_/2
    };
}
