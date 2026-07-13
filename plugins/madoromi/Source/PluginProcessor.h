#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/Madoromi.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Tatsunari Madoromi. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/Madoromi.h) -- the DSP itself lives there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class MadoromiAudioProcessor final : public juce::AudioProcessor
{
public:
    MadoromiAudioProcessor();
    ~MadoromiAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunari Madoromi"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; } // wash decay tail

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

    // The DSP engine (variable-CLOCK bracket + wash + micro-loop). Preallocates
    // its scratch/history in prepareToPlay; processBlock only calls its noexcept
    // setters and process().
    factory_core::Madoromi engine;

    // Cached atomic parameter pointers (fetched once in the constructor).
    std::atomic<float>* clockParam   = nullptr; // Hz
    std::atomic<float>* washParam    = nullptr; // %
    std::atomic<float>* toneParam    = nullptr; // Hz
    std::atomic<float>* lengthParam  = nullptr; // ms
    std::atomic<float>* loopParam    = nullptr; // bool (latched freeze)
    std::atomic<float>* balanceParam = nullptr; // %
    std::atomic<float>* mixParam     = nullptr; // %
    std::atomic<float>* outputParam  = nullptr; // dB
    std::atomic<float>* bypassParam  = nullptr; // bool

    // Continuous output trim (regression policy: smoothed). Reset in
    // prepareToPlay, target set per block, ramped per sample. This sits AFTER
    // the engine (which does its own internal parameter smoothing and dry
    // compensation) -- "output" is not one of the engine's parameters.
    juce::SmoothedValue<float> outputGain;

    // Bypass edge tracking -> engine.resetForBypass() on either transition
    // (state reset on bypass entry AND exit, regression policy; the dry
    // compensation delay line is deliberately preserved -- see Madoromi.h's
    // resetForBypass() contract -- to avoid an audible dropout, D6 fix).
    bool wasBypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MadoromiAudioProcessor)
};
