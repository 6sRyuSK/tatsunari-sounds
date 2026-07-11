#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
    using Interval = factory_core::OnsenDelay::Interval;

    const juce::StringArray kIntervalNames { "Oct -", "5th -", "4th -",
                                             "Unison", "4th +", "5th +", "Oct +" };

    // Host-sync note divisions, in quarter-note beats (4/4 whole note = 4).
    const juce::StringArray kDivisionNames { "1/1", "1/2", "1/4.", "1/4", "1/8.", "1/4T", "1/8", "1/8T", "1/16" };
    constexpr double kDivisionBeats[] = { 4.0, 2.0, 1.5, 1.0, 0.75, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25 };

    Interval intervalFromIndex (float v) noexcept
    {
        const int i = juce::jlimit (0, factory_core::OnsenDelay::kNumIntervals - 1, (int) std::lround (v));
        return (Interval) i;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
OnsenDelayAudioProcessor::createParameterLayout()
{
    using namespace juce;
    namespace core = factory_core;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto logRange = [] (float lo, float hi)
    {
        NormalisableRange<float> r { lo, hi };
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    };

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "time", 1 }, "Time",
        logRange ((float) core::OnsenDelay::kMinTimeMs, (float) core::OnsenDelay::kMaxTimeMs), 350.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "sync", 1 }, "Sync", false));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "division", 1 }, "Division", kDivisionNames, 3)); // 1/4

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "int1", 1 }, "Interval 1", kIntervalNames, 3)); // Unison

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "int2", 1 }, "Interval 2", kIntervalNames, 3)); // Unison

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "glide", 1 }, "Glide",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 25.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "regen", 1 }, "Regen",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 40.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "tone", 1 }, "Tone",
        logRange ((float) core::OnsenDelay::kMinToneHz, (float) core::OnsenDelay::kMaxToneHz), 6000.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 35.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "stepmode", 1 }, "Step Mode", StringArray { "Auto", "Manual" }, 0));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "advance", 1 }, "Advance", false));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

OnsenDelayAudioProcessor::OnsenDelayAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    timeParam     = apvts.getRawParameterValue ("time");
    syncParam     = apvts.getRawParameterValue ("sync");
    divisionParam = apvts.getRawParameterValue ("division");
    int1Param     = apvts.getRawParameterValue ("int1");
    int2Param     = apvts.getRawParameterValue ("int2");
    glideParam    = apvts.getRawParameterValue ("glide");
    regenParam    = apvts.getRawParameterValue ("regen");
    toneParam     = apvts.getRawParameterValue ("tone");
    mixParam      = apvts.getRawParameterValue ("mix");
    stepModeParam = apvts.getRawParameterValue ("stepmode");
    advanceParam  = apvts.getRawParameterValue ("advance");
    outputParam   = apvts.getRawParameterValue ("output");
    bypassParam   = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, onsen_delay_presets::bank,
                        onsen_delay_presets::kExclude, onsen_delay_presets::kNumExclude);
}

void OnsenDelayAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    engine.prepare (sampleRate, 2);
    prevAdvance.store (advanceParam->load() > 0.5f, std::memory_order_relaxed);
    prevBypassed = bypassParam->load() > 0.5f;

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));
}

bool OnsenDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void OnsenDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;

    // Base time: free ms, or the host tempo mapped through the division table.
    double timeMs = (double) timeParam->load();
    if (syncParam->load() > 0.5f)
    {
        double bpm = 120.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto hostBpm = pos->getBpm())
                    if (*hostBpm > 0.0)
                        bpm = *hostBpm;

        const int div = juce::jlimit (0, kDivisionNames.size() - 1,
                                      (int) std::lround (divisionParam->load()));
        timeMs = kDivisionBeats[div] * 60000.0 / bpm; // engine clamps to its range
    }

    engine.setBaseTimeMs (timeMs);
    engine.setIntervals (intervalFromIndex (int1Param->load()),
                         intervalFromIndex (int2Param->load()));
    engine.setGlide  (glideParam->load() * 0.01);
    engine.setRegen  (regenParam->load() * 0.01);
    engine.setToneHz (toneParam->load());
    engine.setMix    (mixParam->load() * 0.01);
    engine.setManualStep (stepModeParam->load() > 0.5f);

    const bool advance = advanceParam->load() > 0.5f;
    if (advance && ! prevAdvance.load (std::memory_order_relaxed))
        engine.triggerStep();
    prevAdvance.store (advance, std::memory_order_relaxed);

    // State reset on bypass transitions (regression policy): entering bypass
    // drops the tail, leaving it starts clean. Both paths have zero latency.
    if (bypassed != prevBypassed)
    {
        engine.reset();
        prevBypassed = bypassed;
    }

    const int numCh = juce::jmin (totalIn, totalOut, 2);
    const int n     = buffer.getNumSamples();

    if (! bypassed && numCh > 0)
    {
        float* chans[2] = { buffer.getWritePointer (0),
                            numCh > 1 ? buffer.getWritePointer (1) : nullptr };
        engine.process (chans, numCh, n);
    }

    uiCurrentStep.store (engine.currentStep(), std::memory_order_relaxed);

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* OnsenDelayAudioProcessor::createEditor()
{
    return new OnsenDelayAudioProcessorEditor (*this);
}

void OnsenDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void OnsenDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());

    // Re-seed after a host restores state (setStateInformation can arrive after
    // prepareToPlay): an incoming advance=1 must not read as a fresh 0->1 edge
    // on the next processBlock.
    prevAdvance.store (advanceParam->load() > 0.5f, std::memory_order_relaxed);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OnsenDelayAudioProcessor();
}
