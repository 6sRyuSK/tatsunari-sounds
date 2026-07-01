#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::StringArray kPitches { "+12", "+7", "+5", "+19", "-12" };
}

double ShimmerReverbAudioProcessor::pitchSemis (int index) noexcept
{
    switch (index)
    {
        case 0:  return 12.0;
        case 1:  return 7.0;
        case 2:  return 5.0;
        case 3:  return 19.0;
        default: return -12.0;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
ShimmerReverbAudioProcessor::createParameterLayout()
{
    using F = juce::AudioParameterFloat;
    using A = juce::AudioParameterFloatAttributes;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto logRange = [] (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r { lo, hi };
        r.setSkewForCentre (centre);
        return r;
    };

    layout.add (std::make_unique<F> (juce::ParameterID { "size", 1 }, "Size",
        juce::NormalisableRange<float> { 30.0f, 160.0f, 0.1f }, 100.0f, A().withLabel (" %")));
    layout.add (std::make_unique<F> (juce::ParameterID { "decay", 1 }, "Decay",
        logRange (0.2f, 15.0f, 2.5f), 2.5f, A().withLabel (" s")));
    layout.add (std::make_unique<F> (juce::ParameterID { "damping", 1 }, "Damping",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 30.0f, A().withLabel (" %")));
    layout.add (std::make_unique<F> (juce::ParameterID { "predelay", 1 }, "Pre-Delay",
        juce::NormalisableRange<float> { 0.0f, 250.0f, 0.1f }, 0.0f, A().withLabel (" ms")));

    layout.add (std::make_unique<F> (juce::ParameterID { "shimmer", 1 }, "Shimmer",
        juce::NormalisableRange<float> { 0.0f, 95.0f, 0.1f }, 35.0f, A().withLabel (" %")));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "pitcha", 1 }, "Pitch A", kPitches, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "pitchb", 1 }, "Pitch B", kPitches, 1));
    layout.add (std::make_unique<F> (juce::ParameterID { "voicemix", 1 }, "Voice Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f, A().withLabel (" %")));

    layout.add (std::make_unique<F> (juce::ParameterID { "lowcut", 1 }, "Low Cut",
        logRange (20.0f, 1000.0f, 120.0f), 120.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { "highcut", 1 }, "High Cut",
        logRange (1000.0f, 18000.0f, 9000.0f), 9000.0f, A().withLabel (" Hz")));

    layout.add (std::make_unique<F> (juce::ParameterID { "modrate", 1 }, "Mod Rate",
        logRange (0.05f, 5.0f, 0.5f), 0.3f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { "moddepth", 1 }, "Mod Depth",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 20.0f, A().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "freeze", 1 }, "Freeze", false));
    layout.add (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 30.0f, A().withLabel (" %")));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

ShimmerReverbAudioProcessor::ShimmerReverbAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    sizeParam     = apvts.getRawParameterValue ("size");
    decayParam    = apvts.getRawParameterValue ("decay");
    dampParam     = apvts.getRawParameterValue ("damping");
    preDelayParam = apvts.getRawParameterValue ("predelay");
    shimmerParam  = apvts.getRawParameterValue ("shimmer");
    pitchAParam   = apvts.getRawParameterValue ("pitcha");
    pitchBParam   = apvts.getRawParameterValue ("pitchb");
    voiceMixParam = apvts.getRawParameterValue ("voicemix");
    lowCutParam   = apvts.getRawParameterValue ("lowcut");
    highCutParam  = apvts.getRawParameterValue ("highcut");
    modRateParam  = apvts.getRawParameterValue ("modrate");
    modDepthParam = apvts.getRawParameterValue ("moddepth");
    freezeParam   = apvts.getRawParameterValue ("freeze");
    mixParam      = apvts.getRawParameterValue ("mix");
    bypassParam   = apvts.getRawParameterValue ("bypass");
}

void ShimmerReverbAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    engine.prepare (sampleRate);
    outputLevel.store (0.0f);

    // ~40ms ramp keeps Size read-position and Mix moves continuous (issue #40).
    constexpr double kRampSec = 0.04;
    sizeSmoothed.reset (sampleRate, kRampSec);
    mixSmoothed.reset (sampleRate, kRampSec);
    sizeSmoothed.setCurrentAndTargetValue (sizeParam->load() * 0.01);
    mixSmoothed.setCurrentAndTargetValue (mixParam->load() * 0.01);
}

bool ShimmerReverbAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ShimmerReverbAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
    {
        outputLevel.store (0.0f);
        return;
    }

    engine.setDecaySec (decayParam->load());
    engine.setDamping (dampParam->load() * 0.01);
    engine.setPreDelayMs (preDelayParam->load());
    engine.setShimmer (shimmerParam->load() * 0.01);
    engine.setPitchASemis (pitchSemis ((int) pitchAParam->load()));
    engine.setPitchBSemis (pitchSemis ((int) pitchBParam->load()));
    engine.setVoiceBMix (voiceMixParam->load() * 0.01);
    engine.setLowCutHz (lowCutParam->load());
    engine.setHighCutHz (highCutParam->load());
    engine.setModRateHz (modRateParam->load());
    engine.setModDepth (modDepthParam->load() * 0.01);
    engine.setFreeze (freezeParam->load() > 0.5f);

    // Size and Mix are continuous and jump audibly if stepped at block
    // boundaries; ramp them per-sample via SmoothedValue (issue #40).
    sizeSmoothed.setTargetValue (sizeParam->load() * 0.01);
    mixSmoothed.setTargetValue (mixParam->load() * 0.01);

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    double sumSq = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        engine.setSize (sizeSmoothed.getNextValue());
        engine.setMix (mixSmoothed.getNextValue());

        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        engine.processStereo (l, r);
        L[i] = (float) l;
        if (R != nullptr) R[i] = (float) r;
        sumSq += 0.5 * (l * l + r * r);
    }

    outputLevel.store ((float) std::sqrt (sumSq / juce::jmax (1, numSamples)));
}

juce::AudioProcessorEditor* ShimmerReverbAudioProcessor::createEditor()
{
    return new ShimmerReverbAudioProcessorEditor (*this);
}

void ShimmerReverbAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ShimmerReverbAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ShimmerReverbAudioProcessor();
}
