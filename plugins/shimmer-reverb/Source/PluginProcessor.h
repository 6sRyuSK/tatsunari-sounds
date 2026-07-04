#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/ShimmerReverb.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Shimmer reverb. The AudioProcessor is a thin wrapper around
// factory_core::ShimmerReverb: per block it pushes the parameters, then runs
// the engine per sample. processBlock does not allocate, lock, or make
// syscalls. Output level is published to the editor's visualiser.
//
class ShimmerReverbAudioProcessor final : public juce::AudioProcessor
{
public:
    ShimmerReverbAudioProcessor();
    ~ShimmerReverbAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tammer Reverb"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    float getOutputLevel() const noexcept { return outputLevel.load(); }
    static double pitchSemis (int index) noexcept; // choice index -> semitones

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* sizeParam     = nullptr;
    std::atomic<float>* decayParam    = nullptr;
    std::atomic<float>* dampParam     = nullptr;
    std::atomic<float>* preDelayParam = nullptr;
    std::atomic<float>* shimmerParam  = nullptr;
    std::atomic<float>* pitchAParam   = nullptr;
    std::atomic<float>* pitchBParam   = nullptr;
    std::atomic<float>* voiceMixParam = nullptr;
    std::atomic<float>* lowCutParam   = nullptr;
    std::atomic<float>* highCutParam  = nullptr;
    std::atomic<float>* modRateParam  = nullptr;
    std::atomic<float>* modDepthParam = nullptr;
    std::atomic<float>* freezeParam   = nullptr;
    std::atomic<float>* mixParam      = nullptr;
    std::atomic<float>* bypassParam   = nullptr;

    factory_presets::ProgramAdapter programs;

    factory_core::ShimmerReverb engine;
    std::atomic<float> outputLevel { 0.0f };

    // Smoothed continuous params to avoid zipper/click when moved (issue #40).
    juce::SmoothedValue<double> sizeSmoothed;
    juce::SmoothedValue<double> mixSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbAudioProcessor)
};
