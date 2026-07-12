#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
MadoromiAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // "log" ranges: geometric-mean centre skew (matches the fleet's frequency /
    // time knob convention, e.g. nam-player's logFreqRange / tumble-delay's
    // logRange helpers). Linear % ranges below are plain.
    auto logRange = [] (float lo, float hi)
    {
        juce::NormalisableRange<float> r { lo, hi };
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "clock", 1 }, "Clock",
        logRange (8000.0f, 48000.0f), 32000.0f,
        juce::AudioParameterFloatAttributes().withLabel (" Hz")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "wash", 1 }, "Wash",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        juce::AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone", 1 }, "Tone",
        logRange (1500.0f, 18000.0f), 6000.0f,
        juce::AudioParameterFloatAttributes().withLabel (" Hz")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "length", 1 }, "Length",
        logRange (50.0f, 2000.0f), 500.0f,
        juce::AudioParameterFloatAttributes().withLabel (" ms")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "loop", 1 }, "Loop", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "balance", 1 }, "Balance",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        juce::AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        juce::AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

MadoromiAudioProcessor::MadoromiAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    clockParam   = apvts.getRawParameterValue ("clock");
    washParam    = apvts.getRawParameterValue ("wash");
    toneParam    = apvts.getRawParameterValue ("tone");
    lengthParam  = apvts.getRawParameterValue ("length");
    loopParam    = apvts.getRawParameterValue ("loop");
    balanceParam = apvts.getRawParameterValue ("balance");
    mixParam     = apvts.getRawParameterValue ("mix");
    outputParam  = apvts.getRawParameterValue ("output");
    bypassParam  = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, madoromi_presets::bank,
                        madoromi_presets::kExclude, madoromi_presets::kNumExclude);
}

void MadoromiAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    engine.prepare (sampleRate, 2);

    const bool bypassed = bypassParam->load() > 0.5f;

    engine.setClockHz       ((double) clockParam->load());
    engine.setWash01        ((double) washParam->load()   / 100.0);
    engine.setToneHz        ((double) toneParam->load());
    engine.setLengthSeconds ((double) lengthParam->load() / 1000.0);
    engine.setFrozen        (loopParam->load() > 0.5f);
    engine.setBalance01     ((double) balanceParam->load() / 100.0);
    engine.setMix01         (bypassed ? 0.0 : (double) mixParam->load() / 100.0);

    // Two-stage reset (house pattern): prepare() already reset the engine once
    // with its own default-constructed targets; now that the real APVTS-loaded
    // values have been pushed, reset() again so every smoother snaps straight
    // onto them instead of audibly gliding in over the first block.
    engine.reset();

    setLatencySamples (engine.latencySamples());

    wasBypassed = bypassed;

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));
}

bool MadoromiAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void MadoromiAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    const int n        = buffer.getNumSamples();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, n);

    const bool bypassed = bypassParam->load() > 0.5f;

    // Reset the engine on either bypass transition (regression policy: state
    // reset on bypass transitions, both directions).
    if (bypassed != wasBypassed)
    {
        engine.reset();
        wasBypassed = bypassed;
    }

    // Push every parameter every block; the engine keeps running even while
    // bypassed. Bypass pushes mix=0 instead of skipping process() entirely, so
    // the output stays the engine's own delay-compensated dry copy -- exactly
    // latency-aligned with the reported latency at every bypass state
    // ("saturator" style true bypass, see plugins/saturator).
    engine.setClockHz       ((double) clockParam->load());
    engine.setWash01        ((double) washParam->load()   / 100.0);
    engine.setToneHz        ((double) toneParam->load());
    engine.setLengthSeconds ((double) lengthParam->load() / 1000.0);
    engine.setFrozen        (loopParam->load() > 0.5f);
    engine.setBalance01     ((double) balanceParam->load() / 100.0);
    engine.setMix01 (bypassed ? 0.0 : (double) mixParam->load() / 100.0);

    const int numCh = juce::jmin (totalIn, totalOut);
    engine.process (buffer.getArrayOfWritePointers(), numCh, n);

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* MadoromiAudioProcessor::createEditor()
{
    return new MadoromiAudioProcessorEditor (*this);
}

void MadoromiAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute -- old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void MadoromiAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MadoromiAudioProcessor();
}
