#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/ResonanceSuppressor.h"

#include <array>
#include <atomic>

//
// Soothe-style dynamic resonance suppressor. The AudioProcessor is a thin
// wrapper around factory_core::ResonanceSuppressor (STFT spectral engine): per
// block it configures the engine from the parameters, rasterizes the per-band
// "reduction profile" nodes into a per-bin multiplier, then processes. It
// reports the engine's latency to the host and publishes the live magnitude /
// reduction spectra to the editor lock-free. processBlock does not allocate.
//
class ResonanceSuppressorAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kFftOrder = 11;
    static constexpr int kNumBins  = (1 << kFftOrder) / 2 + 1; // 1025
    static constexpr int kNumNodes = 6;

    ResonanceSuppressorAudioProcessor();
    ~ResonanceSuppressorAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Resonance Suppressor"; }
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

    static juce::String nodePid (int node, const char* suffix);

    // Editor display snapshots (GUI thread reads; lock-free).
    float displayMagDb (int bin) const noexcept { return pubMag[(size_t) bin].load (std::memory_order_relaxed); }
    float displayRedDb (int bin) const noexcept { return pubRed[(size_t) bin].load (std::memory_order_relaxed); }
    int   binsForDisplay() const noexcept { return kNumBins; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void rasterizeProfile();

    std::atomic<float>* depthParam  = nullptr;
    std::atomic<float>* sharpParam  = nullptr;
    std::atomic<float>* lowParam    = nullptr;
    std::atomic<float>* highParam   = nullptr;
    std::atomic<float>* atkParam    = nullptr;
    std::atomic<float>* relParam    = nullptr;
    std::atomic<float>* mixParam    = nullptr;
    std::atomic<float>* deltaParam  = nullptr;
    std::atomic<float>* linkParam   = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    struct NodeParams { std::atomic<float>* on = nullptr; std::atomic<float>* freq = nullptr; std::atomic<float>* amt = nullptr; };
    std::array<NodeParams, kNumNodes> nodes;

    factory_core::ResonanceSuppressor suppressor;
    double currentSampleRate = 44100.0;
    std::array<double, kNumBins> profileBuf {};

    std::array<std::atomic<float>, kNumBins> pubMag {};
    std::array<std::atomic<float>, kNumBins> pubRed {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessor)
};
