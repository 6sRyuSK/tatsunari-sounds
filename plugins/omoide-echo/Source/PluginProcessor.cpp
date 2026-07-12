#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
OmoideEchoAudioProcessor::createParameterLayout()
{
    using F = juce::AudioParameterFloat;
    using A = juce::AudioParameterFloatAttributes;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Log-skewed range centred on the default (house convention: a
    // NormalisableRange over just {lo, hi} defaults to a linear/continuous
    // interval, and setSkewForCentre places `centre` at the knob's midpoint).
    auto logRange = [] (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r { lo, hi };
        r.setSkewForCentre (centre);
        return r;
    };

    layout.add (std::make_unique<F> (juce::ParameterID { "delay", 1 }, "Delay",
        logRange (100.0f, 10000.0f, 500.0f), 500.0f, A().withLabel (" ms")));

    layout.add (std::make_unique<F> (juce::ParameterID { "regen", 1 }, "Regen",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 40.0f, A().withLabel (" %")));

    // 500-11000 Hz (log skew, default 6000): the 2026-07-12 revision of the
    // engine's kToneMaxHz (18000 -> 11000, an internal-24kHz-Nyquist fix) --
    // this APVTS range is built to the amended spec regardless of what the
    // header's constant momentarily reads.
    layout.add (std::make_unique<F> (juce::ParameterID { "tone", 1 }, "Tone",
        logRange (500.0f, 11000.0f, 6000.0f), 6000.0f, A().withLabel (" Hz")));

    layout.add (std::make_unique<F> (juce::ParameterID { "scan", 1 }, "Scan",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f, A().withLabel (" %")));

    // "Memory" is the user-facing name for the scan-head level; the APVTS ID
    // stays "scanlevel" (engine-facing / preset-facing, state-stable).
    layout.add (std::make_unique<F> (juce::ParameterID { "scanlevel", 1 }, "Memory",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f, A().withLabel (" %")));

    layout.add (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 35.0f, A().withLabel (" %")));

    layout.add (std::make_unique<F> (juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, A().withLabel (" dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

OmoideEchoAudioProcessor::OmoideEchoAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    delayParam      = apvts.getRawParameterValue ("delay");
    regenParam      = apvts.getRawParameterValue ("regen");
    toneParam       = apvts.getRawParameterValue ("tone");
    scanParam       = apvts.getRawParameterValue ("scan");
    scanLevelParam  = apvts.getRawParameterValue ("scanlevel");
    mixParam        = apvts.getRawParameterValue ("mix");
    outputParam     = apvts.getRawParameterValue ("output");
    bypassParam     = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, omoide_echo_presets::bank,
                        omoide_echo_presets::kExclude, omoide_echo_presets::kNumExclude);
}

void OmoideEchoAudioProcessor::pushParamsToEngine (bool bypassed) noexcept
{
    engine.setDelayMs     (delayParam->load());
    engine.setRegen01     (regenParam->load() / 100.0);
    engine.setToneHz      (toneParam->load());
    engine.setScan01      (scanParam->load() / 100.0);
    engine.setScanLevel01 (scanLevelParam->load() / 100.0);
    // Bypass never hard-bypasses the engine (that would break latency
    // alignment) -- it pushes mix = 0 instead, so the output is exactly the
    // latency-compensated dry path while the engine keeps running.
    engine.setMix01 (bypassed ? 0.0 : (mixParam->load() / 100.0));
}

void OmoideEchoAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    const int numChannels = juce::jmin (getTotalNumOutputChannels(), 2);
    engine.prepare (sampleRate, numChannels);

    const bool bypassed = bypassParam->load() > 0.5f;
    pushParamsToEngine (bypassed);
    engine.reset(); // snaps every smoother/glide to the just-pushed targets
    wasBypassed = bypassed;

    setLatencySamples (engine.latencySamples());

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));
}

bool OmoideEchoAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void OmoideEchoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;
    if (bypassed != wasBypassed)
    {
        // Bypass transition: reset the engine (regression policy: state
        // reset on bypass transitions), not a hard bypass around it -- see
        // pushParamsToEngine's mix=0 push below, which keeps the engine's
        // latency-aligned dry path exactly correct while bypassed.
        engine.reset();
        wasBypassed = bypassed;
    }

    pushParamsToEngine (bypassed);

    const int numCh = juce::jmin (totalIn, totalOut, 2);
    engine.process (buffer.getArrayOfWritePointers(), numCh, buffer.getNumSamples());

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    const int n = buffer.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* OmoideEchoAudioProcessor::createEditor()
{
    return new OmoideEchoAudioProcessorEditor (*this);
}

void OmoideEchoAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void OmoideEchoAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OmoideEchoAudioProcessor();
}
