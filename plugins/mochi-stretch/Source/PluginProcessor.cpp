#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
MochiStretchAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "speed", 1 }, "Speed",
        juce::NormalisableRange<float> { -200.0f, 200.0f, 0.1f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float> { -12.0f, 12.0f, 1.0f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("st")));

    // Log skew: default (1000 ms) sits at the slider's centre position.
    juce::NormalisableRange<float> windowRange { 100.0f, 4000.0f, 1.0f };
    windowRange.setSkewForCentre (1000.0f);
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "window", 1 }, "Window",
        windowRange, 1000.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "hold", 1 }, "Hold", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

MochiStretchAudioProcessor::MochiStretchAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    speedParam  = apvts.getRawParameterValue ("speed");
    pitchParam  = apvts.getRawParameterValue ("pitch");
    windowParam = apvts.getRawParameterValue ("window");
    holdParam   = apvts.getRawParameterValue ("hold");
    mixParam    = apvts.getRawParameterValue ("mix");
    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, mochi_stretch_presets::bank,
                        mochi_stretch_presets::kExclude, mochi_stretch_presets::kNumExclude);
}

void MochiStretchAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    const int numCh = juce::jmax (1, juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    engine.prepare (sampleRate, numCh);

    // Push every current parameter into the engine BEFORE reset(), so reset()'s
    // smoother-snap-to-target lands on the real, current values (house order:
    // prepare -> push params -> reset -> setLatencySamples).
    engine.setSpeed      ((double) speedParam->load() * 0.01);
    engine.setPitchSemis ((double) pitchParam->load());
    engine.setWindowMs   ((double) windowParam->load());
    engine.setHold       (holdParam->load() > 0.5f);

    lastBypassed = bypassParam->load() > 0.5f;
    engine.setMix01 (lastBypassed ? 0.0 : ((double) mixParam->load() * 0.01));

    engine.reset();
    setLatencySamples (engine.latencySamples()); // always 0, per engine contract

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        lastBypassed ? 1.0f : juce::Decibels::decibelsToGain (outputParam->load()));
}

bool MochiStretchAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void MochiStretchAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;
    if (bypassed != lastBypassed)
    {
        lastBypassed = bypassed;
        engine.reset(); // regression policy: state reset on bypass transitions
    }

    // Engine setters every block, current values, non-finite-guarded inside
    // the engine itself -- cheap, allocation/lock-free (see MochiStretch.h).
    engine.setSpeed      ((double) speedParam->load() * 0.01);
    engine.setPitchSemis ((double) pitchParam->load());
    engine.setWindowMs   ((double) windowParam->load());
    engine.setHold       (holdParam->load() > 0.5f);
    // Bypass forces the engine's own mix target to 0 (keep-running strategy;
    // see PluginProcessor.h) rather than the user's actual `mix` parameter.
    engine.setMix01 (bypassed ? 0.0 : ((double) mixParam->load() * 0.01));

    const int numCh = juce::jmin (totalIn, totalOut);
    const int n     = buffer.getNumSamples();

    if (numCh > 0)
    {
        float* channelPtrs[factory_core::MochiStretch::kMaxChannels] = {};
        const int engineCh = juce::jmin (numCh, (int) factory_core::MochiStretch::kMaxChannels);
        for (int ch = 0; ch < engineCh; ++ch)
            channelPtrs[ch] = buffer.getWritePointer (ch);

        engine.process (channelPtrs, engineCh, n);
    }

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* MochiStretchAudioProcessor::createEditor()
{
    return new MochiStretchAudioProcessorEditor (*this);
}

void MochiStretchAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void MochiStretchAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MochiStretchAudioProcessor();
}
