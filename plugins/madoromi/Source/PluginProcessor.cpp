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
    const bool bypassed = bypassParam->load() > 0.5f;

    // Push every APVTS-loaded parameter BEFORE prepare() -- this matters more
    // than it used to (D2 fix): prepare() now captures the CLOCK value in
    // effect at that exact moment and fixes the reported latency to it (see
    // Madoromi.h's Latency contract), so if clock were pushed only AFTER
    // prepare(), a host that restores a saved project (non-default clock)
    // before the first prepareToPlay would get a latency computed from the
    // engine's stale/default clock instead of the real one. Pushing first
    // means prepare()'s own reset() already snaps every smoother straight
    // onto the loaded targets, so no separate "two-stage reset" is needed.
    engine.setClockHz       ((double) clockParam->load());
    engine.setWash01        ((double) washParam->load()   / 100.0);
    engine.setToneHz        ((double) toneParam->load());
    engine.setLengthSeconds ((double) lengthParam->load() / 1000.0);
    engine.setFrozen        (loopParam->load() > 0.5f);
    engine.setBalance01     ((double) balanceParam->load() / 100.0);
    engine.setMix01         (bypassed ? 0.0 : (double) mixParam->load() / 100.0);

    engine.prepare (sampleRate, 2);

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

    // Push every parameter every block; the engine keeps running even while
    // bypassed. Bypass pushes mix=0 instead of skipping process() entirely, so
    // the output stays the engine's own delay-compensated dry copy -- exactly
    // latency-aligned with the reported latency at every bypass state
    // ("saturator" style true bypass, see plugins/saturator). Pushed BEFORE
    // the bypass-transition reset below so resetForBypass()'s smoother snap
    // (mixSm in particular) lands on the bypass-appropriate target, not the
    // previous block's stale one.
    engine.setClockHz       ((double) clockParam->load());
    engine.setWash01        ((double) washParam->load()   / 100.0);
    engine.setToneHz        ((double) toneParam->load());
    engine.setLengthSeconds ((double) lengthParam->load() / 1000.0);
    engine.setFrozen        (loopParam->load() > 0.5f);
    engine.setBalance01     ((double) balanceParam->load() / 100.0);
    engine.setMix01 (bypassed ? 0.0 : (double) mixParam->load() / 100.0);

    // D6 fix (bypass de-click, approved): resetForBypass() clears every
    // actual DSP/feedback node (wash, looper, resamplers, FIFOs) but PRESERVES
    // the dry compensation delay line, so the dry path stays sample-
    // continuous across the transition instead of dropping out (a plain
    // reset() zeroed the dry ring line too -- audible dropout). See its
    // contract comment in Madoromi.h.
    if (bypassed != wasBypassed)
    {
        engine.resetForBypass();
        wasBypassed = bypassed;
    }

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
