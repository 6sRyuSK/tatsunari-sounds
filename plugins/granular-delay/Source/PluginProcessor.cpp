#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::StringArray kDivisions { "1/4", "1/4.", "1/8", "1/8.", "1/8T", "1/16" };
}

double GranularDelayAudioProcessor::divisionBeats (int index) noexcept
{
    switch (index)
    {
        case 0:  return 1.0;        // 1/4
        case 1:  return 1.5;        // dotted 1/4
        case 2:  return 0.5;        // 1/8
        case 3:  return 0.75;       // dotted 1/8
        case 4:  return 1.0 / 3.0;  // 1/8 triplet
        default: return 0.25;       // 1/16
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
GranularDelayAudioProcessor::createParameterLayout()
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

    layout.add (std::make_unique<F> (juce::ParameterID { "delay", 1 }, "Delay",
        logRange (1.0f, 2000.0f, 300.0f), 300.0f, A().withLabel (" ms")));
    layout.add (std::make_unique<F> (juce::ParameterID { "feedback", 1 }, "Feedback",
        juce::NormalisableRange<float> { 0.0f, 95.0f, 0.1f }, 40.0f, A().withLabel (" %")));
    layout.add (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 35.0f, A().withLabel (" %")));

    layout.add (std::make_unique<F> (juce::ParameterID { "grainsize", 1 }, "Grain Size",
        logRange (10.0f, 400.0f, 120.0f), 120.0f, A().withLabel (" ms")));
    layout.add (std::make_unique<F> (juce::ParameterID { "density", 1 }, "Density",
        logRange (1.0f, 100.0f, 25.0f), 25.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { "jitter", 1 }, "Position Jitter",
        juce::NormalisableRange<float> { 0.0f, 200.0f, 0.1f }, 20.0f, A().withLabel (" ms")));

    layout.add (std::make_unique<F> (juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 1.0f }, 0.0f, A().withLabel (" st")));
    layout.add (std::make_unique<F> (juce::ParameterID { "pitchrand", 1 }, "Pitch Random",
        juce::NormalisableRange<float> { 0.0f, 12.0f, 0.1f }, 0.0f, A().withLabel (" st")));
    layout.add (std::make_unique<F> (juce::ParameterID { "spread", 1 }, "Spread",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f, A().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "sync", 1 }, "Tempo Sync", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "division", 1 }, "Division", kDivisions, 2));

    layout.add (std::make_unique<F> (juce::ParameterID { "lforate", 1 }, "LFO Rate",
        logRange (0.01f, 10.0f, 0.5f), 0.5f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { "lfodepth", 1 }, "LFO Depth",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f, A().withLabel (" %")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

GranularDelayAudioProcessor::GranularDelayAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    delayParam    = apvts.getRawParameterValue ("delay");
    feedbackParam = apvts.getRawParameterValue ("feedback");
    mixParam      = apvts.getRawParameterValue ("mix");
    sizeParam     = apvts.getRawParameterValue ("grainsize");
    densityParam  = apvts.getRawParameterValue ("density");
    jitterParam   = apvts.getRawParameterValue ("jitter");
    pitchParam    = apvts.getRawParameterValue ("pitch");
    pitchRndParam = apvts.getRawParameterValue ("pitchrand");
    spreadParam   = apvts.getRawParameterValue ("spread");
    syncParam     = apvts.getRawParameterValue ("sync");
    divisionParam = apvts.getRawParameterValue ("division");
    lfoRateParam  = apvts.getRawParameterValue ("lforate");
    lfoDepthParam = apvts.getRawParameterValue ("lfodepth");
    bypassParam   = apvts.getRawParameterValue ("bypass");
}

void GranularDelayAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate, 2.0);
    lfoPhase = 0.0;
    outputLevel.store (0.0f);
}

bool GranularDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void GranularDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

    // Base delay: tempo-synced or free.
    double baseDelaySec = delayParam->load() * 1.0e-3;
    if (syncParam->load() > 0.5f)
    {
        double bpm = 120.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm())
                    bpm = *b;
        baseDelaySec = factory_core::tempoSyncSeconds (bpm, divisionBeats ((int) divisionParam->load()));
    }
    const double baseDelaySamples = baseDelaySec * currentSampleRate;

    engine.setFeedback (feedbackParam->load() * 0.01);
    engine.setMix (mixParam->load() * 0.01);
    engine.setGrainSizeMs (sizeParam->load());
    engine.setDensityHz (densityParam->load());
    engine.setPositionJitterMs (jitterParam->load());
    engine.setPitchSemitones (pitchParam->load());
    engine.setPitchRandomSemis (pitchRndParam->load());
    engine.setSpread (spreadParam->load() * 0.01);

    const double lfoInc = lfoRateParam->load() / currentSampleRate;
    const double lfoDepth = lfoDepthParam->load() * 0.01;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    double sumSq = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double lfo = std::sin (2.0 * juce::MathConstants<double>::pi * lfoPhase);
        lfoPhase += lfoInc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;

        engine.setDelaySamples (baseDelaySamples * (1.0 + 0.25 * lfoDepth * lfo));

        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        engine.processStereo (l, r);
        L[i] = (float) l;
        if (R != nullptr) R[i] = (float) r;
        sumSq += 0.5 * (l * l + r * r);
    }

    outputLevel.store ((float) std::sqrt (sumSq / juce::jmax (1, numSamples)));
}

juce::AudioProcessorEditor* GranularDelayAudioProcessor::createEditor()
{
    return new GranularDelayAudioProcessorEditor (*this);
}

void GranularDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void GranularDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GranularDelayAudioProcessor();
}
