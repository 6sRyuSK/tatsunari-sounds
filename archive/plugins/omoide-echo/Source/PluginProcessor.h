#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/OmoideEcho.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Tatsunari Omoide Echo. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class OmoideEchoAudioProcessor final : public juce::AudioProcessor
{
public:
    OmoideEchoAudioProcessor();
    ~OmoideEchoAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunari Omoide Echo"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // 12 s tail: long enough to let the regen = 0.95 feedback loop ring out.
    double getTailLengthSeconds() const override { return 12.0; }

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

    // Pushes every current APVTS value into the engine (called both from
    // prepareToPlay, before the post-prepare reset(), and every processBlock).
    // `bypassed` overrides the effective mix target to 0 so the engine keeps
    // running (latency-aligned dry passthrough) instead of being hard-bypassed.
    void pushParamsToEngine (bool bypassed) noexcept;

    factory_presets::ProgramAdapter programs;

    factory_core::OmoideEcho engine;
    bool wasBypassed = false; // edge-detects the bypass transition -> engine.resetForBypass()
                              // (D6: NOT engine.reset() -- that would zero the dry
                              // compensation ring and drop the dry path for L samples
                              // on every bypass engage/disengage; see OmoideEcho.h's
                              // resetForBypass() contract)

    std::atomic<float>* delayParam      = nullptr; // ms
    std::atomic<float>* regenParam      = nullptr; // %
    std::atomic<float>* toneParam       = nullptr; // Hz
    std::atomic<float>* scanParam       = nullptr; // %
    std::atomic<float>* scanLevelParam  = nullptr; // % ("Memory")
    std::atomic<float>* mixParam        = nullptr; // %
    std::atomic<float>* outputParam     = nullptr; // dB
    std::atomic<float>* bypassParam     = nullptr; // bool

    // Output trim is applied AFTER the engine (house convention: forced to
    // unity while bypassed, so bypass A/B compares the untrimmed dry path).
    // Continuous parameters must be smoothed (regression policy): reset in
    // prepareToPlay, setTargetValue in processBlock, ramp per sample.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OmoideEchoAudioProcessor)
};
