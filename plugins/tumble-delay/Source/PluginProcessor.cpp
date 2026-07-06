#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
TumbleDelayAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // TODO(scaffold): add the plugin's real parameters.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

TumbleDelayAudioProcessor::TumbleDelayAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, tumble_delay_presets::bank,
                        tumble_delay_presets::kExclude, tumble_delay_presets::kNumExclude);
}

void TumbleDelayAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    // TODO(scaffold): prepare() the factory_core engine here (preallocate all
    // buffers; processBlock must never allocate). Reset all state (regression
    // policy: state reset on prepare and bypass transitions).
    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));
}

bool TumbleDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void TumbleDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;

    // TODO(scaffold): run the factory_core engine here (per sample or per
    // block). On bypass, keep latency-aligned and reset/crossfade state.

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    const int numCh = juce::jmin (totalIn, totalOut);
    const int n     = buffer.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* TumbleDelayAudioProcessor::createEditor()
{
    return new TumbleDelayAudioProcessorEditor (*this);
}

void TumbleDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void TumbleDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TumbleDelayAudioProcessor();
}
