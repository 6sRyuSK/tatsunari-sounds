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

    // Coefficient smoothing (#32): when new coeffs arrive we ramp from the
    // currently-applied `currentCoeffs` to `targetCoeffs` over `rampSamples`,
    // updating the biquads at sub-block granularity to avoid a discontinuous
    // step (zipper/click) with non-zero z-state. All state below is preallocated
    // in prepareToPlay; processBlock never allocates.
    static constexpr int kRampUpdateInterval = 16; // samples between coeff updates
    factory_core::BiquadCoeffs currentCoeffs {};   // ramp start (last settled coeffs)
    factory_core::BiquadCoeffs targetCoeffs {};    // destination of the active ramp
    factory_core::BiquadCoeffs lastApplied {};     // most recent interpolated coeffs
    int rampSamples    = 0;   // total ramp length in samples (derived from sampleRate)
    int rampRemaining  = 0;   // samples left in the active ramp (0 == not ramping)

    // Bypass smoothing (#41): reset filter state on the bypass->active transition
    // so stale z-state can't click back in.
    bool wasBypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingleBandEqAudioProcessor)
};
