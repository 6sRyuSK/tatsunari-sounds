#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Tumble Delay. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class TumbleDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    TumbleDelayAudioProcessor();
    ~TumbleDelayAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tumble Delay"; }
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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    factory_presets::ProgramAdapter programs;

    // TODO(scaffold): add the plugin's real parameters here (atomic pointers via
    // apvts.getRawParameterValue) and the factory_core engine instance.
    std::atomic<float>* outputParam = nullptr; // dB
    std::atomic<float>* bypassParam = nullptr; // bool

    // Continuous parameters must be smoothed (regression policy): reset in
    // prepareToPlay, setTargetValue in processBlock, ramp per sample.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TumbleDelayAudioProcessor)
};
