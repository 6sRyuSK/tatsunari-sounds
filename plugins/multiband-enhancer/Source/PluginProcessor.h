#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/MultibandEnhancer.h"
#include "factory_core/StftResolution.h"

#include <array>
#include <atomic>
#include <vector>

//
// Tatsumin Enhancer — 5-band parallel harmonic enhancer. The AudioProcessor is a
// thin wrapper around factory_core::MultibandEnhancer: per block it pushes the
// parameters to the engine, runs it, crossfades to a latency-matched dry
// reference when bypassed, and publishes pre / post / delta analyser rings plus
// per-band residual meters to the editor lock-free. processBlock does not
// allocate, lock, or make syscalls.
//
class MultibandEnhancerAudioProcessor final : public juce::AudioProcessor,
                                              private juce::AsyncUpdater
{
public:
    static constexpr int kBands = 5;
    // Analyser ring: display FFT tracks the sample rate (fftOrderForSampleRate
    // base 13 .. max 15), so the ring must hold at least 2^15 samples.
    static constexpr int kRingSize = 1 << 16; // 65536
    static constexpr int kRingMask = kRingSize - 1;
    static constexpr int kMaxLatency = 64;    // >= 51 (the HQ oversampler latency)

    enum Ring { RingPre = 0, RingPost, RingDelta };

    MultibandEnhancerAudioProcessor();
    ~MultibandEnhancerAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsumin Enhancer"; }
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

    // ---- editor helpers (GUI thread, lock-free) --------------------------
    double getSampleRateForDisplay() const noexcept { return currentSampleRate; }
    int    displayFftOrder() const noexcept
    {
        return factory_core::fftOrderForSampleRate (currentSampleRate, 13, 48000.0, 15);
    }
    // Copy the latest `num` mono samples from ring `which` into dest (newest last).
    void copyRing (float* dest, int num, int which) const noexcept;
    float bandResidualRmsDb (int band) const noexcept { return bandRmsDb[(size_t) band].load (std::memory_order_relaxed); }
    double effectiveCrossoverHz (int i) const noexcept { return effXover[(size_t) i].load (std::memory_order_relaxed); }

    // Parameter id helpers (shared with the editor).
    static juce::String enhId (int b)   { return "enh" + juce::String (b + 1); }
    static juce::String widthId (int b) { return "wid" + juce::String (b + 1); }
    static juce::String xoverId (int i) { return "xov" + juce::String (i + 1); }

    static constexpr const char* kBandNames[kBands] = { "LO", "LO-MID", "MID", "HI-MID", "HI" };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void handleAsyncUpdate() override; // apply a latency change on the message thread

    factory_core::MultibandEnhancer engine;

    std::atomic<float>* enhP[kBands]   { };
    std::atomic<float>* widthP[kBands] { };
    std::atomic<float>* xovP[4]        { };
    std::atomic<float>* modeP    = nullptr;
    std::atomic<float>* directP  = nullptr;
    std::atomic<float>* wetP     = nullptr;
    std::atomic<float>* outputP  = nullptr;
    std::atomic<float>* qualityP = nullptr;
    std::atomic<float>* deltaP   = nullptr;
    std::atomic<float>* bypassP  = nullptr;

    double currentSampleRate = 44100.0;

    // scratch (sized in prepareToPlay; no audio-thread allocation)
    std::vector<float> deltaL, deltaR, dryL, dryR, rBuf;

    // latency-matched dry reference for the bypass crossfade
    std::array<float, kMaxLatency> dryHistL {}, dryHistR {};
    juce::SmoothedValue<float> bypassMix; // 0 = engine, 1 = dry reference

    // analyser rings (single producer: audio thread)
    std::array<float, kRingSize> ringPre {}, ringPost {}, ringDelta {};
    std::atomic<int> ringWrite { 0 };

    std::array<std::atomic<float>, kBands> bandRmsDb {};
    std::array<std::atomic<double>, 4>     effXover {};
    std::atomic<int> reportedLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEnhancerAudioProcessor)
};
