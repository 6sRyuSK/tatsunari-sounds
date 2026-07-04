#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/Compressor.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

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

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

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

    factory_presets::ProgramAdapter programs;

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
