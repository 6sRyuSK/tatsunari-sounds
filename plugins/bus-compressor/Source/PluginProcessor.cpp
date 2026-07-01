#include "PluginProcessor.h"
#include "PluginEditor.h"

double BusCompressorAudioProcessor::ratioFromIndex (int index) noexcept
{
    switch (index)
    {
        case 0:  return 2.0;
        case 1:  return 4.0;
        default: return 10.0;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
BusCompressorAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "threshold", 1 }, "Threshold",
        juce::NormalisableRange<float> { -40.0f, 0.0f, 0.01f }, -16.0f,
        juce::AudioParameterFloatAttributes().withLabel (" dB")));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "ratio", 1 }, "Ratio",
        juce::StringArray { "2:1", "4:1", "10:1" }, 1));

    auto msRange = [] (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r { lo, hi };
        r.setSkewForCentre (centre);
        return r;
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "attack", 1 }, "Attack",
        msRange (0.1f, 100.0f, 10.0f), 10.0f,
        juce::AudioParameterFloatAttributes().withLabel (" ms")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "release", 1 }, "Release",
        msRange (10.0f, 1200.0f, 200.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel (" ms")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "makeup", 1 }, "Makeup",
        juce::NormalisableRange<float> { 0.0f, 30.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel (" dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

BusCompressorAudioProcessor::BusCompressorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue ("threshold");
    ratioParam     = apvts.getRawParameterValue ("ratio");
    attackParam    = apvts.getRawParameterValue ("attack");
    releaseParam   = apvts.getRawParameterValue ("release");
    makeupParam    = apvts.getRawParameterValue ("makeup");
    mixParam       = apvts.getRawParameterValue ("mix");
    bypassParam    = apvts.getRawParameterValue ("bypass");
}

void BusCompressorAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    compressor.prepare (sampleRate);
    gainReductionDb.store (0.0f);
    wasBypassed = false;

    constexpr double rampSeconds = 0.04; // ~40 ms zipper-free ramp
    makeupSmoothed.reset (sampleRate, rampSeconds);
    mixSmoothed.reset (sampleRate, rampSeconds);
    makeupSmoothed.setCurrentAndTargetValue (makeupParam != nullptr ? (double) makeupParam->load() : 0.0);
    mixSmoothed.setCurrentAndTargetValue (mixParam != nullptr ? (double) mixParam->load() * 0.01 : 1.0);
}

bool BusCompressorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void BusCompressorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
    {
        gainReductionDb.store (0.0f);
        wasBypassed = true;
        return;
    }

    // Bypass -> active transition: clear stale ballistics (gainStateDb) so the
    // first active block does not click, and jump the smoothers to the current
    // targets so they don't ramp from a bypass-era value.
    if (wasBypassed)
    {
        compressor.reset();
        makeupSmoothed.setCurrentAndTargetValue ((double) makeupParam->load());
        mixSmoothed.setCurrentAndTargetValue ((double) mixParam->load() * 0.01);
        wasBypassed = false;
    }

    compressor.setThresholdDb (thresholdParam->load());
    compressor.setRatio (ratioFromIndex ((int) ratioParam->load()));
    compressor.setAttackMs (attackParam->load());
    compressor.setReleaseMs (releaseParam->load());

    // Smoothed parameter delivery (zipper-free). Makeup is part of the core's
    // gain law, so deliver the smoothed dB to the core per sub-block; mix is a
    // dry/wet blend applied per sample from the smoothed value.
    makeupSmoothed.setTargetValue ((double) makeupParam->load());
    mixSmoothed.setTargetValue ((double) mixParam->load() * 0.01);

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    constexpr int subBlock = 32; // makeup update granularity

    float minGr = 0.0f; // most negative GR over the block, for the meter

    if (numCh >= 2)
    {
        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            if ((i % subBlock) == 0)
                compressor.setMakeupDb (makeupSmoothed.skip (juce::jmin (subBlock, numSamples - i)));

            const double mix = mixSmoothed.getNextValue();
            const double dl = L[i], dr = R[i];
            double cl = dl, cr = dr;
            compressor.processStereoSample (cl, cr);
            L[i] = (float) ((1.0 - mix) * dl + mix * cl);
            R[i] = (float) ((1.0 - mix) * dr + mix * cr);
            minGr = juce::jmin (minGr, (float) compressor.currentGainReductionDb());
        }
    }
    else if (numCh == 1)
    {
        auto* x = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
        {
            if ((i % subBlock) == 0)
                compressor.setMakeupDb (makeupSmoothed.skip (juce::jmin (subBlock, numSamples - i)));

            const double mix = mixSmoothed.getNextValue();
            const double dx = x[i];
            const double g = compressor.processDetector (std::abs (dx));
            x[i] = (float) ((1.0 - mix) * dx + mix * (dx * g));
            minGr = juce::jmin (minGr, (float) compressor.currentGainReductionDb());
        }
    }

    gainReductionDb.store (minGr);
}

juce::AudioProcessorEditor* BusCompressorAudioProcessor::createEditor()
{
    return new BusCompressorAudioProcessorEditor (*this);
}

void BusCompressorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BusCompressorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BusCompressorAudioProcessor();
}
