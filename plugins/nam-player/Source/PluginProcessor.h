#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/NamRoutingEngine.h"
#include "factory_core/PartitionedConvolver.h"
#include "factory_core/RateBracket.h"
#include "factory_core/ResamplerLatency.h"
#include "factory_core/DelayLine.h"
#include "factory_core/Biquad.h"
#include "factory_core/Filters.h"

#include "NamModel.h"
#include "ModelHandoff.h"

#include <array>
#include <atomic>
#include <complex>
#include <functional>
#include <vector>

//
// NAM Player — loads up to three Neural Amp Modeler models + one cabinet IR and
// mixes them with per-slot Series/Parallel routing (factory_core::NamRoutingEngine),
// a zero-latency partitioned cab convolver (factory_core::PartitionedConvolver, with
// a time-based IR-length cap), tone hi/lo cut, a dry/wet blend and a click-free
// crossfade bypass. The AudioProcessor is a thin, real-time-safe wrapper: everything
// is preallocated in prepareToPlay and processBlock does not allocate/lock/syscall.
// An over-sized host block is chunked to the prepared size (never dropped).
//
// True stereo: L and R run through independent NAM instances (models are mono and
// stateful), so each slot holds a model per channel. Models and IR kernels are
// loaded on the message thread and handed to the audio thread lock-free via
// ModelHandoff; retired objects are freed back on the message thread (Timer).
//
// The NAM section always runs at kNamRate (48 kHz, the models' trained rate). When
// the host runs at a different rate the section is bracketed by band-limited
// resamplers inside factory_core::RateBracket (PolyphaseResampler, so the non-linear
// amp output does not alias); its output FIFO delivers exactly the host block size,
// giving a fixed, reported latency that the dry path is delayed to match.
//
class NamPlayerAudioProcessor final : public juce::AudioProcessor,
                                      private juce::Timer
{
public:
    static constexpr int    kNumSlots     = 3;
    static constexpr double kNamRate      = 48000.0;   // fixed internal NAM-section rate
    static constexpr double kMaxIrSeconds = 0.17;      // cab IR length cap in TIME (rate-independent)
    static constexpr int    kNamMaxBlock  = 8192;      // NAM Reset() max buffer (blocks are chunked to this)

    struct ImpulseKernel { factory_core::PartitionedConvolver::Kernel k; };

    NamPlayerAudioProcessor();
    ~NamPlayerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParamPtr; }

    const juce::String getName() const override { return "Super Tatsunari NAM Player"; }
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

    // ---- Message-thread API used by the editor -------------------------------
    void loadModel (int slot, const juce::File& file);
    void clearModel (int slot);
    void loadIr (const juce::File& file);
    void clearIr();

    juce::String slotName (int slot) const;
    juce::String irName() const;

    // Offline "reamp pair" export (MERGE): render `inWav` (must be 48 kHz) through the
    // captured chain to `outWav` (48 kHz mono float). Heavy + does file I/O — call off
    // the audio/message thread (the editor runs it on a background thread). Uses freshly
    // loaded models / a freshly built IR kernel, never the live audio objects.
    struct ReampResult { bool ok = false; juce::String message; };
    ReampResult renderReampToFile (const juce::File& inWav, const juce::File& outWav,
                                   bool includeIrTone, const std::function<void (float)>& onProgress);

    static juce::String slotPid (int slot, const char* suffix);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    juce::ValueTree filesTree();
    void reloadModelsFromState();
    void buildAndPublishIrKernels();

    // Process a contiguous slice [start, start+m) of the buffer (m <= currentBlock);
    // processBlock loops this so an over-sized host block is chunked, never dropped.
    void processChunk (juce::AudioBuffer<float>& buffer, int start, int m,
                       const ImpulseKernel* kernL, const ImpulseKernel* kernR) noexcept;

    struct SlotParams
    {
        std::atomic<float>* enable  = nullptr;
        std::atomic<float>* mode    = nullptr;
        std::atomic<float>* ingain  = nullptr;
        std::atomic<float>* out     = nullptr;
        std::atomic<float>* balance = nullptr;
    };
    std::array<SlotParams, kNumSlots> slot;

    std::atomic<float>* inTrimParam   = nullptr;
    std::atomic<float>* irEnableParam = nullptr;
    std::atomic<float>* irLevelParam  = nullptr;
    std::atomic<float>* outGainParam  = nullptr;
    std::atomic<float>* mixParam      = nullptr;
    std::atomic<float>* loCutParam    = nullptr;
    std::atomic<float>* hiCutParam    = nullptr;
    std::atomic<float>* bypassParam   = nullptr;
    juce::AudioProcessorParameter* bypassParamPtr = nullptr;   // for getBypassParameter()

    factory_core::NamRoutingEngine                    engine;
    std::array<factory_core::PartitionedConvolver, 2> irConv;
    std::array<factory_core::Biquad, 2>               loCut, hiCut;

    std::array<std::array<ModelHandoff<NamModel>, 2>, kNumSlots> modelHandoff;
    std::array<ModelHandoff<ImpulseKernel>, 2>                   irHandoff;

    std::array<std::vector<float>, 2> irRaw;
    double irRawRate = 0.0;
    bool   irLoaded  = false;
    int    maxIrSamples = 0;                 // kMaxIrSeconds * sampleRate, set in prepareToPlay

    // Bypass is a click-free crossfade (dry<->wet). While the fade is settled at fully
    // bypassed we skip the (expensive) wet chain; on release the crossfade covers the
    // transient from resuming the NAM section from stale state.
    juce::SmoothedValue<float> bypassSm;

    // Resampling around the 48 kHz NAM section (bypassed when host == 48 kHz).
    // The whole host<->48k bracket + output FIFO + latency reporting lives in the
    // headless factory_core::RateBracket so the wet/dry alignment is unit-testable.
    bool  resampling = false;
    int   reportedLatency = 0;
    int   namMaxBlk = 0;
    factory_core::RateBracket<>            bracket;
    std::array<factory_core::DelayLine, 2> dryDelay;

    juce::SmoothedValue<float> inTrimSm, outGainSm, mixSm;

    std::vector<float> wL, wR, dryL, dryR;
    double currentSampleRate = kNamRate;
    int    currentBlock = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamPlayerAudioProcessor)
};
