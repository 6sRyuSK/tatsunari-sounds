#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/Waveshaper.h"

#include <atomic>
#include <memory>

//
// Waveshaping soft-clip saturator. The AudioProcessor is a thin wrapper around
// factory_core::Waveshaper; the only audio-thread work beyond that is fixed-
// ratio oversampling (juce::dsp::Oversampling) to keep aliasing in check.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class SaturatorAudioProcessor final : public juce::AudioProcessor
{
public:
    SaturatorAudioProcessor();
    ~SaturatorAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Taturator"; }
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

    // A shaper configured from the current parameter values, for the editor's
    // transfer-curve display (called on the message thread).
    factory_core::Waveshaper makeDisplayShaper() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* driveParam  = nullptr; // dB
    std::atomic<float>* mixParam    = nullptr; // %
    std::atomic<float>* outputParam = nullptr; // dB
    std::atomic<float>* bypassParam = nullptr; // bool

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    factory_core::Waveshaper shaper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SaturatorAudioProcessor)
};
