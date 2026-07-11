#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/OnsenDelay.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Onsen Delay. The AudioProcessor is a thin wrapper around
// factory_core::OnsenDelay (harmonic glide delay) — the DSP lives there,
// header-only and JUCE-independent, so tests/dsp_test.cpp exercises it
// headless. Everything is preallocated in prepareToPlay; processBlock does
// not allocate, lock, or make syscalls.
//
class OnsenDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    OnsenDelayAudioProcessor();
    ~OnsenDelayAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Onsen Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Max regen (0.95) decays ~0.45 dB per repeat; report a pragmatic tail so
    // hosts keep processing after input stops without claiming minutes.
    double getTailLengthSeconds() const override { return 8.0; }

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Sequencer step for the editor's step indicator (audio -> GUI, atomic).
    std::atomic<int> uiCurrentStep { 0 };

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    factory_presets::ProgramAdapter programs;
    factory_core::OnsenDelay engine;

    std::atomic<float>* timeParam     = nullptr; // ms
    std::atomic<float>* syncParam     = nullptr; // bool
    std::atomic<float>* divisionParam = nullptr; // choice index
    std::atomic<float>* int1Param     = nullptr; // choice index
    std::atomic<float>* int2Param     = nullptr; // choice index
    std::atomic<float>* glideParam    = nullptr; // %
    std::atomic<float>* regenParam    = nullptr; // %
    std::atomic<float>* toneParam     = nullptr; // Hz
    std::atomic<float>* mixParam      = nullptr; // %
    std::atomic<float>* stepModeParam = nullptr; // choice: 0=Auto 1=Manual
    std::atomic<float>* advanceParam  = nullptr; // momentary bool
    std::atomic<float>* outputParam   = nullptr; // dB
    std::atomic<float>* bypassParam   = nullptr; // bool

    bool prevAdvance  = false;
    bool prevBypassed = false;

    // Continuous parameters must be smoothed (regression policy): the engine
    // smooths its own controls internally; the output trim rides SmoothedValue.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnsenDelayAudioProcessor)
};
