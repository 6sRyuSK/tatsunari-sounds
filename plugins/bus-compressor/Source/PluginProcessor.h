#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/Compressor.h"

#include <atomic>

//
// SSL-G-style stereo bus compressor. The AudioProcessor is a thin wrapper around
// factory_core::Compressor (stereo-linked detection). Parameters are read per
// block; processBlock does not allocate, lock, or make syscalls. Current gain
// reduction is published to the editor through an atomic for metering.
//
class BusCompressorAudioProcessor final : public juce::AudioProcessor
{
public:
    BusCompressorAudioProcessor();
    ~BusCompressorAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunari Bus Compressor"; }
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

    // For the editor's gain-reduction meter (<= 0 dB). Message-thread read.
    float getGainReductionDb() const noexcept { return gainReductionDb.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* thresholdParam = nullptr;
    std::atomic<float>* ratioParam     = nullptr; // choice index
    std::atomic<float>* attackParam    = nullptr;
    std::atomic<float>* releaseParam   = nullptr;
    std::atomic<float>* makeupParam    = nullptr;
    std::atomic<float>* mixParam       = nullptr;
    std::atomic<float>* bypassParam    = nullptr;

    factory_core::Compressor compressor;
    std::atomic<float> gainReductionDb { 0.0f };

    // Bypass edge detection: reset ballistics on the bypass->active transition
    // so releasing bypass does not click with stale gain state.
    bool wasBypassed = false;

    // Zipper-free parameter delivery: ramp makeup (dB) and mix over ~40 ms.
    juce::SmoothedValue<double> makeupSmoothed;
    juce::SmoothedValue<double> mixSmoothed;

    static double ratioFromIndex (int index) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusCompressorAudioProcessor)
};
