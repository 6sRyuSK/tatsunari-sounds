#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/Surikire.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Tatsunari Surikire. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class SurikireAudioProcessor final : public juce::AudioProcessor
{
public:
    SurikireAudioProcessor();
    ~SurikireAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunari Surikire"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // The wow/flutter delay line and dropout dips ring past the input; report
    // a conservative 0.5 s tail (spec).
    double getTailLengthSeconds() const override { return 0.5; }

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

    // Forward the atomic parameter targets to the engine setters (audio
    // thread; plain clamped stores, no allocation).
    void pushParametersToEngine();

    factory_presets::ProgramAdapter programs;

    // Lo-fi media-degradation engine (wow/flutter, generation-loss filters,
    // saturation, hiss, dropouts). Every continuous parameter is smoothed
    // inside the engine (20 ms one-pole), so the wrapper only forwards the
    // atomic targets once per block.
    factory_core::Surikire engine;

    std::atomic<float>* wowParam      = nullptr; // %
    std::atomic<float>* flutterParam  = nullptr; // %
    std::atomic<float>* genParam      = nullptr; // %
    std::atomic<float>* saturateParam = nullptr; // %
    std::atomic<float>* noiseParam    = nullptr; // %
    std::atomic<float>* failureParam  = nullptr; // %
    std::atomic<float>* hpParam       = nullptr; // Hz
    std::atomic<float>* lpParam       = nullptr; // Hz
    std::atomic<float>* mixParam      = nullptr; // %
    std::atomic<float>* outputParam   = nullptr; // dB
    std::atomic<float>* bypassParam   = nullptr; // bool

    // Audio-thread only: detects bypass transitions (regression policy: full
    // engine state reset on entering AND leaving bypass).
    bool wasBypassed = false;

    // Continuous parameters must be smoothed (regression policy): reset in
    // prepareToPlay, setTargetValue in processBlock, ramp per sample.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurikireAudioProcessor)
};
