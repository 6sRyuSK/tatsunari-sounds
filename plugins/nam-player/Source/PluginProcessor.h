#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/NamRoutingEngine.h"
#include "factory_core/FftConvolver.h"
#include "factory_core/Resampler.h"
#include "factory_core/ResamplerLatency.h"
#include "factory_core/DelayLine.h"
#include "factory_core/Biquad.h"
#include "factory_core/Filters.h"

#include "NamModel.h"
#include "ModelHandoff.h"

#include <array>
#include <atomic>
#include <complex>
#include <vector>

//
// NAM Player — loads up to three Neural Amp Modeler models + one cabinet IR and
// mixes them with per-slot Series/Parallel routing (factory_core::NamRoutingEngine),
// a zero-latency FFT cab convolver (factory_core::FftConvolver), tone hi/lo cut and
// a dry/wet blend. The AudioProcessor is a thin, real-time-safe wrapper: everything
// is preallocated in prepareToPlay and processBlock does not allocate/lock/syscall.
//
// True stereo: L and R run through independent NAM instances (models are mono and
// stateful), so each slot holds a model per channel. Models and IR kernels are
// loaded on the message thread and handed to the audio thread lock-free via
// ModelHandoff; retired objects are freed back on the message thread (Timer).
//
// The NAM section always runs at kNamRate (48 kHz, the models' trained rate). When
// the host runs at a different rate the section is bracketed by streaming resamplers
// (factory_core::Resampler); a small output FIFO delivers exactly the host block
// size, giving a fixed, reported latency that the dry path is delayed to match.
//
class NamPlayerAudioProcessor final : public juce::AudioProcessor,
                                      private juce::Timer
{
public:
    static constexpr int    kNumSlots     = 3;
    static constexpr double kNamRate      = 48000.0;   // fixed internal NAM-section rate
    static constexpr int    kMaxIrSamples = 8192;      // cab IR length cap (~170 ms @ 48 kHz)
    static constexpr int    kNamMaxBlock  = 8192;      // NAM Reset() max buffer (blocks are chunked to this)

    struct ImpulseKernel { std::vector<std::complex<double>> H; };

    NamPlayerAudioProcessor();
    ~NamPlayerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "NAM Player"; }
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

    static juce::String slotPid (int slot, const char* suffix);

private:
    // Fixed-capacity SPSC-free ring used on the audio thread to buffer resampled
    // output so exactly `blockSize` host samples can be delivered each callback.
    struct HostFifo
    {
        std::vector<float> buf;
        int mask = 0, rd = 0, wr = 0, count = 0;
        void prepare (int minSize)
        {
            int p = 1; while (p < minSize) p <<= 1;
            buf.assign ((size_t) p, 0.0f); mask = p - 1; rd = wr = count = 0;
        }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); rd = wr = count = 0; }
        void pushZeros (int m) noexcept
        {
            for (int i = 0; i < m; ++i) { buf[(size_t) wr] = 0.0f; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
        }
        void push (const float* x, int m) noexcept
        {
            for (int i = 0; i < m; ++i) { buf[(size_t) wr] = x[i]; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
        }
        void pull (float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i) { if (count > 0) { out[i] = buf[(size_t) rd]; rd = (rd + 1) & mask; --count; } else out[i] = 0.0f; }
        }
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    juce::ValueTree filesTree();
    void reloadModelsFromState();
    void buildAndPublishIrKernels();

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

    factory_core::NamRoutingEngine            engine;
    std::array<factory_core::FftConvolver, 2> irConv;
    std::array<factory_core::Biquad, 2>       loCut, hiCut;

    std::array<std::array<ModelHandoff<NamModel>, 2>, kNumSlots> modelHandoff;
    std::array<ModelHandoff<ImpulseKernel>, 2>                   irHandoff;

    std::array<std::vector<float>, 2> irRaw;
    double irRawRate = 0.0;
    bool   irLoaded  = false;

    // Resampling around the 48 kHz NAM section (bypassed when host == 48 kHz).
    bool  resampling = false;
    int   reportedLatency = 0;
    int   namMaxBlk = 0;
    std::array<factory_core::Resampler, 2> downSamp, upSamp;
    std::array<std::vector<float>, 2>      namBuf, upScratch;
    std::array<HostFifo, 2>                outFifo;
    std::array<factory_core::DelayLine, 2> dryDelay;

    juce::SmoothedValue<float> inTrimSm, outGainSm, mixSm;

    std::vector<float> wL, wR, dryL, dryR;
    double currentSampleRate = kNamRate;
    int    currentBlock = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamPlayerAudioProcessor)
};
