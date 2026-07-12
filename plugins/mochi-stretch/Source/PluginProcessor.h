#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_core/MochiStretch.h"
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

#include <atomic>

//
// Tatsunari Mochi Stretch. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class MochiStretchAudioProcessor final : public juce::AudioProcessor
{
public:
    MochiStretchAudioProcessor();
    ~MochiStretchAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tatsunari Mochi Stretch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // Window can hold up to kWindowMaxMs (4 s) plus the wrap crossfade; 5 s of
    // tail comfortably covers a HOLD loop release at the largest window.
    double getTailLengthSeconds() const override { return 5.0; }

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override { return programs.getNumPrograms(); }
    int getCurrentProgram() override { return programs.getCurrentProgram(); }
    void setCurrentProgram (int index) override { programs.setCurrentProgram (index); }
    const juce::String getProgramName (int index) override { return programs.getProgramName (index); }
    void changeProgramName (int, const juce::String&) override {} // factory presets are immutable

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    factory_presets::ProgramAdapter programs;

    factory_core::MochiStretch engine;

    std::atomic<float>* speedParam  = nullptr; // %  (-200..+200), engine wants v/100
    std::atomic<float>* pitchParam  = nullptr; // semitones (-12..+12)
    std::atomic<float>* windowParam = nullptr; // ms (100..4000)
    std::atomic<float>* holdParam   = nullptr; // bool (latching)
    std::atomic<float>* mixParam    = nullptr; // %  (0..100), engine wants v/100
    std::atomic<float>* outputParam = nullptr; // dB, applied AFTER the engine
    std::atomic<float>* bypassParam = nullptr; // bool

    // Bypass strategy: keep the engine running at all times (its own tape/
    // transport keep rolling — HOLD stays independently latched) and force
    // the engine's OWN 20 ms-smoothed mix target to 0 while bypassed, instead
    // of skipping process() entirely. latencySamples() is always 0, so either
    // choice is spec-authorized; this one is click-free on the A/B toggle
    // (the mix ramps down/up through the engine's existing smoother rather
    // than a hard edit-splice) and needs no extra dry-buffer bookkeeping here.
    // engine.reset() still runs on every bypass transition (regression policy:
    // state reset on bypass transitions) — see prepareToPlay/processBlock.
    bool lastBypassed = false;

    // Continuous parameters that live entirely inside the engine (speed,
    // pitch, window, mix) ride the engine's OWN internal smoothers/glide (see
    // MochiStretch.h's "Parameter contracts") — pushed raw, every block. Only
    // `output` is applied in this wrapper (after the engine), so it is the
    // only one that needs a JUCE-side SmoothedValue here.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MochiStretchAudioProcessor)
};
