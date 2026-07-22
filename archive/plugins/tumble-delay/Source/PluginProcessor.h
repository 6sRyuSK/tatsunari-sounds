#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include "factory_core/TumbleDelay.h"
#include "factory_core/GranularDelay.h" // factory_core::tempoSyncSeconds

#include <array>
#include <atomic>

//
// Tumble Delay. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class TumbleDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    TumbleDelayAudioProcessor();
    ~TumbleDelayAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tumble Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return engine.tailSeconds(); }

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // UI mirror passthrough (message thread; lock-free inside the engine).
    int    snapshotBalls (factory_core::TumbleDelay::BallView* dst, int maxCount) const noexcept { return engine.snapshotBalls (dst, maxCount); }
    int    drainHits     (factory_core::TumbleDelay::HitEvent* dst, int maxCount) noexcept       { return engine.drainHits (dst, maxCount); }
    double boxAngle()    const noexcept { return engine.boxAngle(); }

    // Effective Box Size in seconds AFTER resolving the tempo-sync override —
    // the visualizer scales the drawn box from this, so it must match what the
    // engine was actually given (reading the "boxSize" atomic alone would be
    // wrong whenever boxSizeSync != Off).
    double boxSizeSeconds() const noexcept { return effBoxSizeSec.load (std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Reads every parameter atomic once at block head, resolves tempo-sync
    // overrides (wrapper responsibility, spec §11) and pushes the globals plus
    // four SlotParams to the engine. RT-safe: no allocation, lock, or syscall.
    void pushParametersToEngine (double bpm) noexcept;

    factory_presets::ProgramAdapter programs;

    // The DSP engine. Preallocates its ring in prepareToPlay; processBlock only
    // calls its noexcept setters and processStereo.
    factory_core::TumbleDelay engine;

    // Cached atomic parameter pointers (fetched once in the constructor).
    std::atomic<float>* outputParam = nullptr; // dB   (scaffold)
    std::atomic<float>* bypassParam = nullptr; // bool (scaffold)

    std::atomic<float>* boxShapeParam    = nullptr;
    std::atomic<float>* boxSizeParam     = nullptr;
    std::atomic<float>* boxSizeSyncParam = nullptr;
    std::atomic<float>* spinParam        = nullptr;
    std::atomic<float>* spinSyncParam    = nullptr;
    std::atomic<float>* pivotXParam      = nullptr;
    std::atomic<float>* pivotYParam      = nullptr;
    std::atomic<float>* gravityParam     = nullptr;
    std::atomic<float>* ballCollideParam = nullptr;
    std::atomic<float>* senseParam       = nullptr;
    std::atomic<float>* retrigParam      = nullptr;
    std::atomic<float>* spawnSpreadParam = nullptr;
    std::atomic<float>* refeedParam      = nullptr;
    std::atomic<float>* toneParam        = nullptr;
    std::atomic<float>* mixParam         = nullptr;

    // The 25 per-slot atomics, one struct per slot (A/B/C/D). Built in the ctor
    // via a prefix ID builder; read as a POD group each block.
    struct SlotRefs
    {
        std::atomic<float>* on           = nullptr;
        std::atomic<float>* count        = nullptr;
        std::atomic<float>* ballSize     = nullptr;
        std::atomic<float>* speed        = nullptr;
        std::atomic<float>* direction    = nullptr;
        std::atomic<float>* dirRandom    = nullptr;
        std::atomic<float>* preDelay     = nullptr;
        std::atomic<float>* preDelaySync = nullptr;
        std::atomic<float>* time         = nullptr;
        std::atomic<float>* timeSync     = nullptr;
        std::atomic<float>* bounce       = nullptr;
        std::atomic<float>* drag         = nullptr;
        std::atomic<float>* decayCurve   = nullptr;
        std::atomic<float>* lifeMode     = nullptr;
        std::atomic<float>* lifeTime     = nullptr;
        std::atomic<float>* lifeBounces  = nullptr;
        std::atomic<float>* pitch        = nullptr;
        std::atomic<float>* pitchRand    = nullptr;
        std::atomic<float>* grain        = nullptr;
        std::atomic<float>* reverse      = nullptr;
        std::atomic<float>* motion       = nullptr;
        std::atomic<float>* step         = nullptr;
        std::atomic<float>* spray        = nullptr;
        std::atomic<float>* panMode      = nullptr;
        std::atomic<float>* gain         = nullptr;
    };
    std::array<SlotRefs, 4> slotRefs {};

    // Continuous output gain (regression policy: smoothed). Reset in
    // prepareToPlay, target set per block, ramped per sample.
    juce::SmoothedValue<float> outputGain;

    // Sync-resolved Box Size mirrored for the UI (written per block, read by the
    // visualizer's timer; atomic<double> is lock-free here, same as boxAngle).
    std::atomic<double> effBoxSizeSec { 0.40 };

    // Bypass edge tracking -> engine.reset() on either transition (class E).
    bool wasBypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TumbleDelayAudioProcessor)
};
