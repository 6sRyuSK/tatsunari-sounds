#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/MultibandCompressor.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <array>
#include <atomic>

//
// Vocal-friendly 3-band compressor. The AudioProcessor maps a few easy macro
// controls onto factory_core::MultibandCompressor: a global Compress amount and
// per-band trims set each band's threshold/ratio with auto makeup gain (so more
// compression doesn't just change the level), over fixed vocal-tuned ballistics.
// processBlock does not allocate, lock, or make syscalls.
//
class VocalMbCompAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kBands = 3;

    VocalMbCompAudioProcessor();
    ~VocalMbCompAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParamPtr; }

    const juce::String getName() const override { return "Multi Tatsunari Comp"; }
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

    float getBandGainReductionDb (int band) const noexcept { return bandGr[(size_t) band].load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* compressParam = nullptr;
    std::array<std::atomic<float>*, kBands> bandTrim { nullptr, nullptr, nullptr };
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* lowFreqParam = nullptr;
    std::atomic<float>* highFreqParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    juce::AudioProcessorParameter* bypassParamPtr = nullptr; // for getBypassParameter()

    factory_presets::ProgramAdapter programs;

    factory_core::MultibandCompressor mb;
    std::array<std::atomic<float>, kBands> bandGr { };

    // Crossover frequency smoothing (log-domain) to avoid LR4 coefficient
    // zipper/clicks when the crossover parameters are automated. We smooth
    // log(freq) then exp, and only push new coeffs to the crossover at
    // sub-block granularity and when the value actually moved.
    juce::SmoothedValue<double> lowFreqSmoothed;
    juce::SmoothedValue<double> highFreqSmoothed;
    double lastLowFreq  = 0.0; // cache of last value pushed to mb.setCrossover
    double lastHighFreq = 0.0;
    static constexpr int kXoverUpdateSamples = 32; // sub-block granularity

    // Fixed vocal-tuned ballistics per band.
    static constexpr double kAttackMs[kBands]  = { 25.0, 12.0, 4.0 };
    static constexpr double kReleaseMs[kBands] = { 180.0, 120.0, 70.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMbCompAudioProcessor)
};
