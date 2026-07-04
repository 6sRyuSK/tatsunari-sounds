#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int kOversampleFactorLog2 = 2; // 2^2 = 4x oversampling
}

juce::AudioProcessorValueTreeState::ParameterLayout
SaturatorAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "drive", 1 }, "Drive",
        juce::NormalisableRange<float> { 0.0f, 36.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

SaturatorAudioProcessor::SaturatorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    driveParam  = apvts.getRawParameterValue ("drive");
    mixParam    = apvts.getRawParameterValue ("mix");
    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, saturator_presets::bank,
                        saturator_presets::kExclude, saturator_presets::kNumExclude);
}

factory_core::Waveshaper SaturatorAudioProcessor::makeDisplayShaper() const
{
    factory_core::Waveshaper w;
    w.setDrive  (juce::Decibels::decibelsToGain (driveParam->load()));
    w.setMix    (mixParam->load() * 0.01);
    w.setOutput (juce::Decibels::decibelsToGain (outputParam->load()));
    return w;
}

void SaturatorAudioProcessor::prepareToPlay (double /*sampleRate*/, int samplesPerBlock)
{
    const auto numCh = (size_t) juce::jmax (1, getTotalNumInputChannels());

    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        numCh, kOversampleFactorLog2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true,   // maximum quality
        true);  // integer latency

    oversampling->initProcessing ((size_t) samplesPerBlock);
    oversampling->reset();
    setLatencySamples ((int) oversampling->getLatencyInSamples());
}

bool SaturatorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void SaturatorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Bypass routes an identity shaper through the same oversampling path so the
    // reported latency stays aligned.
    if (bypassParam->load() > 0.5f)
    {
        shaper.setDrive (1.0);
        shaper.setMix (0.0);
        shaper.setOutput (1.0);
    }
    else
    {
        shaper.setDrive  (juce::Decibels::decibelsToGain (driveParam->load()));
        shaper.setMix    (mixParam->load() * 0.01);
        shaper.setOutput (juce::Decibels::decibelsToGain (outputParam->load()));
    }

    if (oversampling == nullptr)
        return;

    const auto numCh = (size_t) juce::jmin (totalIn, totalOut, 2);
    juce::dsp::AudioBlock<float> block (buffer);
    auto sub = block.getSubsetChannelBlock (0, numCh);

    auto osBlock = oversampling->processSamplesUp (sub);
    for (size_t ch = 0; ch < osBlock.getNumChannels(); ++ch)
    {
        auto* d = osBlock.getChannelPointer (ch);
        const int n = (int) osBlock.getNumSamples();
        for (int i = 0; i < n; ++i)
            d[i] = (float) shaper.processSample ((double) d[i]);
    }
    oversampling->processSamplesDown (sub);
}

juce::AudioProcessorEditor* SaturatorAudioProcessor::createEditor()
{
    return new SaturatorAudioProcessorEditor (*this);
}

void SaturatorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        // Append the selected program index (attribute only — existing sessions
        // without it read back as program 0, so state stays compatible).
        programs.writeStateAttribute (*xml);
        copyXmlToBinary (*xml, destData);
    }
}

void SaturatorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            programs.readStateAttribute (*xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SaturatorAudioProcessor();
}
