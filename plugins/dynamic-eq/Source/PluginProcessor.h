#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/DynamicEqBand.h"

#include <array>
#include <atomic>

//
// Multi-band dynamic parametric EQ (Pro-Q-style). N bands, each a
// factory_core::DynamicEqBand (bell/shelf/HP/LP with optional per-band
// dynamics). The AudioProcessor is a thin wrapper: per block it configures the
// bands from the parameters, then cascades them per sample. A lock-free ring of
// recent input samples feeds the editor's spectrum analyzer. processBlock does
// not allocate, lock, or make syscalls.
//
class DynamicEqAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kNumBands = 24;

    DynamicEqAudioProcessor();
    ~DynamicEqAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Dynamic Tatsunari EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    double getSampleRateForDisplay() const noexcept { return currentSampleRate; }

    // Parameter id helpers (shared with the editor).
    static juce::String pid (int band, const char* suffix);

    // Copy the latest `num` analyzer samples (mono) into dest. `post` selects
    // the post-EQ ring instead of the pre-EQ input. GUI thread.
    void copyAnalyzerSamples (float* dest, int num, bool post = false) const noexcept;

    // Per-band effective gain (dB) including the live dynamic offset, published
    // each block by the audio thread. Lets the editor animate the band as it
    // moves. Lock-free; GUI thread reads.
    float getLiveGainDb (int band) const noexcept
    {
        return liveGainDb[(size_t) band].load (std::memory_order_relaxed);
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    struct BandParams
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* byp = nullptr;
        std::atomic<float>* lsn = nullptr;
        std::atomic<float>* chan = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;
        std::atomic<float>* slope = nullptr;
        std::atomic<float>* dyn = nullptr;
        std::atomic<float>* thr = nullptr;
        std::atomic<float>* rng = nullptr;
        std::atomic<float>* atk = nullptr;
        std::atomic<float>* rel = nullptr;
        std::atomic<float>* knee = nullptr;
    };

    std::array<BandParams, kNumBands> params;
    std::array<factory_core::DynamicEqBand, kNumBands> bands;
    std::atomic<float>* bypassParam = nullptr;

    // Per-band smoothing of the continuous Freq / Gain / Q parameters, driven
    // in sub-block chunks in processBlock so automation/fast moves don't zip or
    // click. Sized once (members) and reset in prepareToPlay — no allocation on
    // the audio thread.
    std::array<juce::SmoothedValue<double>, kNumBands> freqSmooth;
    std::array<juce::SmoothedValue<double>, kNumBands> gainSmooth;
    std::array<juce::SmoothedValue<double>, kNumBands> qSmooth;
    static constexpr int kSmoothChunk = 32; // samples per coefficient update

    double currentSampleRate = 44100.0;

    // Analyzer ring buffer (single producer: audio thread).
    static constexpr int kRingSize = 1 << 14; // 16384 (>= analyzer FFT, with margin)
    static constexpr int kRingMask = kRingSize - 1;
    std::array<float, kRingSize> analyzerRing {};     // pre-EQ (input)
    std::array<float, kRingSize> analyzerRingPost {}; // post-EQ (output)
    std::atomic<int> ringWrite { 0 };

    // Live per-band effective gain (dB) for the editor's animated display.
    std::array<std::atomic<float>, kNumBands> liveGainDb {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicEqAudioProcessor)
};
