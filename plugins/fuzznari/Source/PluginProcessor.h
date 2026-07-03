#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/FuzzEngine.h"

#include <array>
#include <atomic>
#include <vector>

//
// Fuzznari — continuous-morph fuzz. The AudioProcessor is a thin wrapper
// around factory_core::FuzzEngine, which owns the entire DSP including the
// RateBracket oversampling, the parameter smoothing and the bypass fade; the
// only work here is delivering atomic parameter targets once per block and
// publishing output samples to the editor's scope ring. Everything is
// preallocated in prepareToPlay; processBlock does not allocate, lock, or
// make syscalls.
//
class FuzznariAudioProcessor final : public juce::AudioProcessor
{
public:
    FuzznariAudioProcessor();
    ~FuzznariAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Fuzznari"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // The squeal is parameter-sustained, not a decay tail, so no tail is
    // reported (a render may truncate a held squeal — accepted by design).
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Most recent post-engine output samples for the editor's waveform overlay
    // (acquire side of the lock-free scope ring; the dynamic-eq pattern).
    void copyScopeSamples (float* dest, int num) const noexcept
    {
        const int w = ringWrite.load (std::memory_order_acquire);
        for (int i = 0; i < num; ++i)
            dest[i] = scopeRing[(size_t) ((w - num + i) & kRingMask)];
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void pushParamsToEngine() noexcept;

    std::atomic<float>* driveParam  = nullptr; // dB
    std::atomic<float>* biasParam   = nullptr; // %
    std::atomic<float>* gateParam   = nullptr; // %
    std::atomic<float>* stabParam   = nullptr; // %
    std::atomic<float>* oscParam    = nullptr; // bool ("Squeal")
    std::atomic<float>* toneParam   = nullptr; // %
    std::atomic<float>* levelParam  = nullptr; // dB
    std::atomic<float>* mixParam    = nullptr; // %
    std::atomic<float>* bypassParam = nullptr; // bool

    factory_core::FuzzEngine engine;

    // Scratch for the right channel when the bus is mono (the engine is
    // stereo-shaped; writing both channels into one buffer would double-process).
    std::vector<float> monoScratch;

    static constexpr int kRingSize = 1 << 14;
    static constexpr int kRingMask = kRingSize - 1;
    std::array<float, kRingSize> scopeRing {};
    std::atomic<int> ringWrite { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FuzznariAudioProcessor)
};
