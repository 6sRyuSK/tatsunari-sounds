#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/GranularDelay.h"

#include <atomic>

//
// Granular delay. The AudioProcessor is a thin wrapper around
// factory_core::GranularDelay: per sample it computes the (optionally
// tempo-synced, LFO-modulated) delay time and runs the engine. processBlock
// does not allocate, lock, or make syscalls. Output level is published to the
// editor's grain-cloud visualiser via an atomic.
//
class GranularDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    GranularDelayAudioProcessor();
    ~GranularDelayAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunular Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // For the editor's visualiser.
    float getOutputLevel() const noexcept { return outputLevel.load(); }

    static double divisionBeats (int index) noexcept; // choice index -> beats

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* delayParam    = nullptr;
    std::atomic<float>* feedbackParam = nullptr;
    std::atomic<float>* mixParam      = nullptr;
    std::atomic<float>* sizeParam     = nullptr;
    std::atomic<float>* densityParam  = nullptr;
    std::atomic<float>* jitterParam   = nullptr;
    std::atomic<float>* pitchParam    = nullptr;
    std::atomic<float>* pitchRndParam = nullptr;
    std::atomic<float>* spreadParam   = nullptr;
    std::atomic<float>* syncParam     = nullptr;
    std::atomic<float>* divisionParam = nullptr;
    std::atomic<float>* lfoRateParam  = nullptr;
    std::atomic<float>* lfoDepthParam = nullptr;
    std::atomic<float>* bypassParam   = nullptr;

    factory_core::GranularDelay engine;
    double currentSampleRate = 44100.0;
    double lfoPhase = 0.0;
    std::atomic<float> outputLevel { 0.0f };

    // Zipper-free parameter ramps (P3). Preallocated members; reset in
    // prepareToPlay, driven per sample in processBlock — RT-safe.
    juce::SmoothedValue<double> feedbackSmoothed;
    juce::SmoothedValue<double> mixSmoothed;
    juce::SmoothedValue<double> delaySamplesSmoothed;

    // Tempo-sync worst case governs the delay-buffer length (issue #37):
    //   maxDivisionBeats(1.5) * 60 / kMinSyncBpm(20) * lfoMaxFactor(1.25)
    //   = 1.5 * 60 / 20 * 1.25 = 5.625 s  ->  size for 6.0 s.
    // Free mode (2000 ms) * 1.25 LFO = 2.5 s is also covered.
    static constexpr double kMinSyncBpm     = 20.0;
    static constexpr double kMaxDelaySeconds = 6.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GranularDelayAudioProcessor)
};
