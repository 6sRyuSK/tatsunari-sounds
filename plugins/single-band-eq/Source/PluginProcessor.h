#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/Biquad.h"

#include <array>
#include <atomic>

//
// A single-band parametric (peaking) EQ. The AudioProcessor is a thin wrapper:
// all DSP lives in factory_core::Biquad. Coefficients are recomputed OFF the
// audio thread (a control-rate timer on the message thread) and handed to the
// audio thread through a preallocated lock-free FIFO. processBlock never
// allocates, locks, or makes syscalls.
//
class SingleBandEqAudioProcessor final : public juce::AudioProcessor,
                                         private juce::Timer
{
public:
    SingleBandEqAudioProcessor();
    ~SingleBandEqAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Single-Band EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;                              // message thread: produce coeffs
    factory_core::BiquadCoeffs computeCoeffs() const noexcept;  // off the audio thread

    std::atomic<float>* freqParam   = nullptr;
    std::atomic<float>* gainParam   = nullptr;
    std::atomic<float>* qParam      = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    double currentSampleRate = 44100.0;

    // Snapshot the producer last computed from, to avoid redundant pushes.
    float lastFreq = 0.0f, lastGain = 0.0f, lastQ = 0.0f;
    bool  haveSnapshot = false;

    // Lock-free coefficient handoff. Single producer (timer), single consumer
    // (audio thread). Backing storage is preallocated; nothing here allocates.
    static constexpr int kFifoCapacity = 32;
    juce::AbstractFifo coeffFifo { kFifoCapacity };
    std::array<factory_core::BiquadCoeffs, kFifoCapacity> coeffStore {};

    // Audio-thread-owned state.
    factory_core::Biquad filters[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingleBandEqAudioProcessor)
};
