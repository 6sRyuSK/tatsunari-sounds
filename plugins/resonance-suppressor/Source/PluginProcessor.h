#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/ReductionProfile.h"
#include "factory_core/ResonanceSuppressor.h"
#include "factory_core/StftResolution.h"

#include <array>
#include <atomic>

//
// Soothe-style dynamic resonance suppressor. The AudioProcessor is a thin
// wrapper around factory_core::ResonanceSuppressor (STFT spectral engine): per
// block it configures the engine from the parameters, rasterizes the per-band
// "reduction profile" nodes into a per-bin multiplier, then processes. It
// reports the engine's latency to the host and publishes the live magnitude /
// reduction spectra to the editor lock-free. processBlock does not allocate.
//
class ResonanceSuppressorAudioProcessor final : public juce::AudioProcessor
{
public:
    // The STFT order tracks the sample rate so the analyser resolution and the
    // suppressor's detection window stay constant in Hz / seconds (see
    // factory_core::fftOrderForSampleRate). Buffers are sized for the top order;
    // `activeBins` is the live bin count for the current sample rate.
    static constexpr int    kBaseFftOrder  = 11;        // N = 2048 at the 48 kHz reference
    static constexpr int    kMaxFftOrder   = 13;        // N = 8192, keeps ~23 Hz bins through 192 kHz
    static constexpr double kRefSampleRate = 48000.0;
    static constexpr int    kMaxBins       = (1 << kMaxFftOrder) / 2 + 1; // 4097
    static constexpr int    kNumBands      = 4; // + a low cut and a high cut

    ResonanceSuppressorAudioProcessor();
    ~ResonanceSuppressorAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Resonance Suppressor"; }
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
    double getSampleRateForDisplay() const noexcept { return currentSampleRate; }

    // Parameter-ID helpers (also used by the editor). Cuts: which 0 = low, 1 =
    // high. Bands: 0..kNumBands-1.
    static juce::String cutPid  (int which, const char* suffix);
    static juce::String bandPid (int band,  const char* suffix);
    // Cut slope choice index (0..3) -> dB/oct.
    static double slopeValue (int index) noexcept;
    // Build the reduction-node config from the current parameter values (GUI
    // thread: the editor curve uses the same mapping as the audio rasteriser).
    static factory_core::ReductionNodes readNodes (juce::AudioProcessorValueTreeState& apvts);

    // Editor display snapshots (GUI thread reads; lock-free).
    float displayMagDb (int bin) const noexcept { return pubMag[(size_t) bin].load (std::memory_order_relaxed); }
    float displayRedDb (int bin) const noexcept { return pubRed[(size_t) bin].load (std::memory_order_relaxed); }
    int   binsForDisplay() const noexcept { return activeBins.load (std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void rasterizeProfile();

    std::atomic<float>* depthParam  = nullptr;
    std::atomic<float>* sharpParam  = nullptr;
    std::atomic<float>* atkParam    = nullptr;
    std::atomic<float>* relParam    = nullptr;
    std::atomic<float>* mixParam    = nullptr;
    std::atomic<float>* deltaParam  = nullptr;
    std::atomic<float>* linkParam   = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* modeParam   = nullptr;

    // Reduction-node parameters, cached for the audio thread (lock-free reads).
    struct CutParams  { std::atomic<float>* on = nullptr; std::atomic<float>* freq = nullptr; std::atomic<float>* slope = nullptr; };
    struct BandParams { std::atomic<float>* on = nullptr; std::atomic<float>* freq = nullptr; std::atomic<float>* type = nullptr; std::atomic<float>* sens = nullptr; };
    CutParams lowCut, highCut;
    std::array<BandParams, kNumBands> bandParams;

    // Assemble the node config from the cached audio-thread pointers.
    factory_core::ReductionNodes currentNodes() const noexcept;

    factory_core::ResonanceSuppressor suppressor;
    double currentSampleRate = kRefSampleRate;
    int    currentFftOrder   = kBaseFftOrder;
    std::atomic<int> activeBins { (1 << kBaseFftOrder) / 2 + 1 };
    bool   wasBypassed       = false;
    std::array<double, kMaxBins> profileBuf {};
    std::array<double, kMaxBins> magScratch {};

    std::array<std::atomic<float>, kMaxBins> pubMag {};
    std::array<std::atomic<float>, kMaxBins> pubRed {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessor)
};
