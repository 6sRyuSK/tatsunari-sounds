#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <memory>
#include <vector>

//
// factory_ui spectrum-analyser model — the shared maths behind every plugin's
// spectrum display: Hann window + FFT + magnitude→dB with fast-attack /
// slow-release smoothing and peak-hold, kept per "channel" (e.g. pre / post /
// delta spectra). Header-only, GUI-thread only.
//
// Construct one per display, call setup() once (or again when the FFT order
// tracks the sample rate), then process() each frame with a freshly filled time
// buffer and read smoothedDb() / peakDb() back. This is only the maths —
// coordinate mapping and path drawing live in SpectrumDisplay.h. The lock-free
// hand-off of samples from the processor stays in each plugin: the caller fills
// the time buffer (its own ring copy) and passes it in.
//
namespace factory_ui
{
    class SpectrumAnalyzerModel
    {
    public:
        // Per-frame display options. Defaults match the house analyser: fast up /
        // slow down at 0.25, peak falls 0.6 dB/frame, −120 dB floor, no tilt.
        struct Options
        {
            float smoothing    = 0.25f;   // smoothed lerp toward the instantaneous value when falling
            float peakFallDb   = 0.6f;    // peak-hold decay per frame
            float floorDb      = -120.0f; // gain→dB floor
            float tiltDbPerOct = 0.0f;    // display tilt (e.g. +3 dB/oct to flatten pink-ish spectra)
        };

        // (Re)allocate for `numChannels` spectra at `fftOrder`, resetting all
        // smoothed / peak state to the floor. A no-op when nothing changed, so
        // per-frame state survives repeated calls (e.g. once per timer tick).
        void setup (int numChannels, int fftOrder)
        {
            if (fftOrder == order_ && numChannels == (int) smoothed_.size())
                return;

            order_ = fftOrder;
            size_  = 1 << fftOrder;
            fft_   = std::make_unique<juce::dsp::FFT> (fftOrder);
            window_.resize ((size_t) size_);
            juce::dsp::WindowingFunction<float>::fillWindowingTables (
                window_.data(), (size_t) size_, juce::dsp::WindowingFunction<float>::hann, false);

            smoothed_.assign ((size_t) numChannels, std::vector<float> ((size_t) (size_ / 2), kFloorInit));
            peak_.assign     ((size_t) numChannels, std::vector<float> ((size_t) (size_ / 2), kFloorInit));
        }

        // Change the FFT order while keeping the channel count; resets state.
        void setOrder (int fftOrder) { setup ((int) smoothed_.size(), fftOrder); }

        int order()   const noexcept { return order_; }
        int size()    const noexcept { return size_; }
        int numBins() const noexcept { return size_ / 2; }

        // Window + FFT `timeData` (which must have capacity 2*size(): the first
        // size() samples are the frame, the tail is used as FFT scratch) and fold
        // the magnitudes into channel `ch`'s smoothed + peak-held dB state.
        void process (int ch, float* timeData, double sampleRate, Options opt = {})
        {
            if (fft_ == nullptr)
                return;

            const int N = size_;
            for (int i = 0; i < N; ++i)     timeData[(size_t) i] *= window_[(size_t) i];
            for (int i = N; i < N * 2; ++i) timeData[(size_t) i] = 0.0f;
            fft_->performFrequencyOnlyForwardTransform (timeData);

            auto& sm = smoothed_[(size_t) ch];
            auto& pk = peak_[(size_t) ch];
            for (int bin = 0; bin < N / 2; ++bin)
            {
                const float mag  = timeData[(size_t) bin] / (float) (N * 0.5);
                float       inst = juce::Decibels::gainToDecibels (mag, opt.floorDb);
                if (opt.tiltDbPerOct != 0.0f)
                {
                    const float freq = (float) ((double) bin * sampleRate / N);
                    inst += opt.tiltDbPerOct * std::log2 (juce::jmax (1.0f, freq) / 1000.0f);
                }

                float& s = sm[(size_t) bin];
                s = (inst > s) ? inst : s + (inst - s) * opt.smoothing; // fast up, slow down
                float& p = pk[(size_t) bin];
                p = (s > p) ? s : juce::jmax (s, p - opt.peakFallDb);   // hold then fall
            }
        }

        float smoothedDb (int ch, int bin) const noexcept { return smoothed_[(size_t) ch][(size_t) bin]; }
        float peakDb     (int ch, int bin) const noexcept { return peak_[(size_t) ch][(size_t) bin]; }

        // Centre frequency of an FFT bin for a given sample rate / transform size.
        static float binFrequency (int bin, double sampleRate, int fftSize) noexcept
        {
            return (float) ((double) bin * sampleRate / fftSize);
        }

    private:
        static constexpr float kFloorInit = -120.0f;

        int order_ = 0, size_ = 0;
        std::unique_ptr<juce::dsp::FFT> fft_;
        std::vector<float> window_;
        std::vector<std::vector<float>> smoothed_, peak_;
    };
} // namespace factory_ui
