#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "factory_core/ReductionProfile.h"
#include "factory_core/MultiResSuppressor.h"
#include "factory_core/StftResolution.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <array>
#include <atomic>

//
// Soothe-style dynamic resonance suppressor. The AudioProcessor is a thin
// wrapper around factory_core::MultiResSuppressor, a dual-resolution STFT engine:
// an LR4 crossover at 3 kHz splits the signal into a low band (order O) and a
// high band (order O-2, a 4x shorter window/hop that reacts 4x faster to airband
// harshness), each suppressed by its own engine and summed at the shared latency
// N_L. Per block it configures the engine from the parameters, rasterizes the
// per-band "reduction profile" nodes into a per-bin multiplier on BOTH engines'
// grids, then processes. It reports the low engine's latency to the host and
// publishes the live magnitude / reduction spectra -- merged onto the low
// (display) grid -- to the editor lock-free. processBlock does not allocate.
//
// Pass 3B routing: an optional stereo Sidechain input bus keys detection off an
// external signal (gated on a live connection, so an unpatched bus falls back to
// internal detection), a Channel Mode switch runs the engine in Stereo or Mid/Side,
// and Link Amount is a continuous per-channel <-> stereo-linked detection blend.
//
// Phase 5a-1: the editor can solo one reduction node's removed signal (Listen,
// setListenNode/getListenNode -- NOT an APVTS parameter, so it is never saved or
// automated). While a node is soloed, processBlock rasterises a single-node
// profile (only that node's cut/band contributes) onto both engines' grids and
// forces delta=true / mix=1.0 for that block only (the APVTS delta/mix params
// are untouched), so the output is exactly what that one node removes; SC Listen
// is forced off (Listen outranks it). displayMagPreDb() additionally publishes
// the PRE-gain (input) spectrum alongside displayMagDb() (post), so the editor
// can show input vs. suppressed side by side.
//
class ResonanceSuppressorAudioProcessor final : public juce::AudioProcessor
{
public:
    // The STFT order tracks the sample rate so the analyser resolution and the
    // suppressor's detection window stay constant in Hz / seconds (see
    // factory_core::fftOrderForSampleRate). Buffers are sized for the top order;
    // `activeBins` is the live bin count for the current sample rate AND Quality
    // (a Quality switch changes the active window length, so it is renegotiated in
    // processBlock — see the Quality wiring there).
    static constexpr int    kBaseFftOrder  = 11;        // N = 2048 at the 48 kHz reference (Normal quality)
    // Buffers must cover the largest window the engine can reach: High quality is
    // the Normal order + 1, and the Normal order tops out at 13 (N = 8192) around
    // 176.4/192 kHz, so High reaches order 14 (N = 16384) there. Sizing for order
    // 13 would overflow the display / profile arrays the moment High is selected.
    static constexpr int    kMaxFftOrder   = 14;        // N = 16384 (High quality at 176.4/192 kHz)
    static constexpr double kRefSampleRate = 48000.0;
    static constexpr int    kMaxBins       = (1 << kMaxFftOrder) / 2 + 1; // 8193
    static constexpr int    kNumBands      = 8; // + a low cut and a high cut

    ResonanceSuppressorAudioProcessor();
    ~ResonanceSuppressorAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParamPtr; }

    const juce::String getName() const override { return "Resonance TatSuppressor"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // The OLA output ring holds a real tail of the engine's latency (N samples),
    // so report it (not 0) — the engine keeps running through bypass.
    double getTailLengthSeconds() const override { return suppressor.latencySamples() / currentSampleRate; }

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

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
    // Pre-gain (input) spectrum snapshot (Phase 5a-1-C), same bin grid as
    // displayMagDb() -- the input spectrum ahead of suppression.
    float displayMagPreDb (int bin) const noexcept { return pubMagPre[(size_t) bin].load (std::memory_order_relaxed); }
    int   binsForDisplay() const noexcept { return activeBins.load (std::memory_order_relaxed); }

    // Listen (Phase 5a-1-B): solo one reduction node's removed signal (output =
    // exactly what that node is cutting, mix-independent). NOT an APVTS
    // parameter -- non-automatable, non-persisted, editor-only transient state;
    // GUI thread writes, audio thread reads once per block. id convention
    // matches the reduction nodes: 0 = low cut, 1 = high cut, 2..(1+kNumBands)
    // = bands, -1 = disabled (normal processing).
    void setListenNode (int id) noexcept { listenNode.store (id, std::memory_order_relaxed); }
    int  getListenNode() const noexcept { return listenNode.load (std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void rasterizeProfile();
    // Listen (5a-1-B): rasterise a profile with only ONE node (nodeId) copied
    // from the live parameters, everything else off -- same per-engine grid
    // mapping as rasterizeProfile(), just a single-node config.
    void rasterizeListenProfile (int nodeId);

    std::atomic<float>* depthParam  = nullptr;
    std::atomic<float>* sharpParam  = nullptr;
    std::atomic<float>* atkParam    = nullptr;
    std::atomic<float>* relParam    = nullptr;
    std::atomic<float>* mixParam    = nullptr;
    std::atomic<float>* deltaParam  = nullptr;
    std::atomic<float>* linkParam   = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* modeParam   = nullptr;
    std::atomic<float>* selParam    = nullptr; // Selectivity (0..100 %)
    std::atomic<float>* tiltParam   = nullptr; // Tilt (-100..+100 %)
    std::atomic<float>* qualityParam = nullptr; // Quality choice (0 Fast, 1 Normal, 2 High)
    // Pass 3B routing params.
    std::atomic<float>* linkAmtParam     = nullptr; // Link Amount (0..100 %)
    std::atomic<float>* channelModeParam = nullptr; // Channel mode (0 Stereo, 1 Mid-Side)
    std::atomic<float>* scEnableParam    = nullptr; // Sidechain detection enable
    std::atomic<float>* scListenParam    = nullptr; // Monitor the sidechain
    juce::AudioProcessorParameter* bypassParamPtr = nullptr; // for getBypassParameter()

    factory_presets::ProgramAdapter programs;

    // Reduction-node parameters, cached for the audio thread (lock-free reads).
    struct CutParams  { std::atomic<float>* on = nullptr; std::atomic<float>* freq = nullptr; std::atomic<float>* slope = nullptr; };
    struct BandParams { std::atomic<float>* on = nullptr; std::atomic<float>* freq = nullptr; std::atomic<float>* type = nullptr; std::atomic<float>* sens = nullptr; std::atomic<float>* width = nullptr; };
    CutParams lowCut, highCut;
    std::array<BandParams, kNumBands> bandParams;

    // Assemble the node config from the cached audio-thread pointers.
    factory_core::ReductionNodes currentNodes() const noexcept;

    factory_core::MultiResSuppressor suppressor;
    double currentSampleRate = kRefSampleRate;
    int    currentFftOrder   = kBaseFftOrder;
    std::atomic<int> activeBins { (1 << kBaseFftOrder) / 2 + 1 };
    std::array<double, kMaxBins> profileBuf {};      // reduction profile on the low (display) grid
    std::array<double, kMaxBins> profileBufHigh {};  // reduction profile on the high engine's grid
    std::array<double, kMaxBins> magScratch {};      // merged-magnitude display scratch (low grid)
    std::array<double, kMaxBins> redScratch {};      // merged-reduction display scratch (low grid)
    std::array<double, kMaxBins> preScratch {};      // merged pre-gain magnitude display scratch (low grid)
    // Listen (5a-1-B): single-node profile, rasterised fresh each block on both
    // engines' grids (prepared arrays, no allocation -- mirrors profileBuf/High).
    std::array<double, kMaxBins> listenProfileLow {};
    std::array<double, kMaxBins> listenProfileHigh {};
    // Listen target: -1 = disabled (normal processing). GUI thread writes,
    // audio thread reads once per block (see setListenNode/getListenNode).
    std::atomic<int> listenNode { -1 };

    std::array<std::atomic<float>, kMaxBins> pubMag {};
    std::array<std::atomic<float>, kMaxBins> pubRed {};
    std::array<std::atomic<float>, kMaxBins> pubMagPre {}; // pre-gain (input) spectrum (5a-1-C)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessor)
};
