#include "PluginProcessor.h"

namespace
{
    constexpr float kFreqDefault = 1000.0f;
    constexpr float kGainDefault = 0.0f;
    constexpr float kQDefault    = 0.707f;
}

juce::AudioProcessorValueTreeState::ParameterLayout
SingleBandEqAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Show at most 2 decimal places (the generic editor otherwise shows many for
    // the continuous, skewed ranges).
    auto twoDp = juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); });

    // frequency: 20 Hz .. 20 kHz, logarithmic, default 1 kHz.
    juce::NormalisableRange<float> freqRange { 20.0f, 20000.0f };
    freqRange.setSkewForCentre (632.455f); // geometric mean of the range
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "frequency", 1 }, "Frequency", freqRange, kFreqDefault, twoDp));

    // gain: -24 .. +24 dB, default 0 dB.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "gain", 1 }, "Gain",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, kGainDefault, twoDp));

    // q: 0.1 .. 18, default 0.707.
    juce::NormalisableRange<float> qRange { 0.1f, 18.0f };
    qRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "q", 1 }, "Q", qRange, kQDefault, twoDp));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

SingleBandEqAudioProcessor::SingleBandEqAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    freqParam   = apvts.getRawParameterValue ("frequency");
    gainParam   = apvts.getRawParameterValue ("gain");
    qParam      = apvts.getRawParameterValue ("q");
    bypassParam = apvts.getRawParameterValue ("bypass");
}

SingleBandEqAudioProcessor::~SingleBandEqAudioProcessor()
{
    stopTimer();
}

factory_core::BiquadCoeffs SingleBandEqAudioProcessor::computeCoeffs() const noexcept
{
    return factory_core::designPeaking (freqParam->load(), gainParam->load(),
                                        qParam->load(), currentSampleRate);
}

void SingleBandEqAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;

    const auto initial = computeCoeffs();
    for (auto& f : filters)
    {
        f.reset();
        f.setCoeffs (initial);
    }

    coeffFifo.reset();
    haveSnapshot = false;

    startTimerHz (60); // control-rate coefficient updates, off the audio thread
}

bool SingleBandEqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void SingleBandEqAudioProcessor::timerCallback()
{
    const float f = freqParam->load();
    const float g = gainParam->load();
    const float q = qParam->load();

    if (haveSnapshot && f == lastFreq && g == lastGain && q == lastQ)
        return;

    lastFreq = f; lastGain = g; lastQ = q; haveSnapshot = true;

    if (coeffFifo.getFreeSpace() < 1)
        return; // audio thread is behind; the next tick will carry the latest

    const auto coeffs = factory_core::designPeaking (f, g, q, currentSampleRate);

    int start1, size1, start2, size2;
    coeffFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)      coeffStore[(size_t) start1] = coeffs;
    else if (size2 > 0) coeffStore[(size_t) start2] = coeffs;
    coeffFifo.finishedWrite (1);
}

void SingleBandEqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Pull the newest coefficients the producer published (lock-free, no alloc).
    if (const int ready = coeffFifo.getNumReady(); ready > 0)
    {
        int start1, size1, start2, size2;
        coeffFifo.prepareToRead (ready, start1, size1, start2, size2);

        factory_core::BiquadCoeffs latest;
        if (size2 > 0)      latest = coeffStore[(size_t) (start2 + size2 - 1)];
        else                latest = coeffStore[(size_t) (start1 + size1 - 1)];

        coeffFifo.finishedRead (ready);

        filters[0].setCoeffs (latest);
        filters[1].setCoeffs (latest);
    }

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
        return;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    for (int ch = 0; ch < numCh; ++ch)
        filters[(size_t) ch].process (buffer.getWritePointer (ch), buffer.getNumSamples());
}

juce::AudioProcessorEditor* SingleBandEqAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void SingleBandEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SingleBandEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// JUCE plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SingleBandEqAudioProcessor();
}
