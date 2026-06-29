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
    static constexpr int kNumBands = 6;

    DynamicEqAudioProcessor();
    ~DynamicEqAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Dynamic Parametric EQ"; }
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

    // Copy the latest `num` analyzer samples (mono) into dest. GUI thread.
    void copyAnalyzerSamples (float* dest, int num) const noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    struct BandParams
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;
        std::atomic<float>* dyn = nullptr;
        std::atomic<float>* thr = nullptr;
        std::atomic<float>* rng = nullptr;
    };

    std::array<BandParams, kNumBands> params;
    std::array<factory_core::DynamicEqBand, kNumBands> bands;
    std::atomic<float>* bypassParam = nullptr;

    double currentSampleRate = 44100.0;

    // Analyzer ring buffer (single producer: audio thread).
    static constexpr int kRingSize = 1 << 13; // 8192
    static constexpr int kRingMask = kRingSize - 1;
    std::array<float, kRingSize> analyzerRing {};
    std::atomic<int> ringWrite { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicEqAudioProcessor)
};
