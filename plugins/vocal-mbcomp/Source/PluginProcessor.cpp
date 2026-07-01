#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
VocalMbCompAudioProcessor::createParameterLayout()
{
    using F = juce::AudioParameterFloat;
    using A = juce::AudioParameterFloatAttributes;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<F> (juce::ParameterID { "compress", 1 }, "Compress",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 40.0f, A().withLabel (" %")));

    const char* names[kBands] = { "Low", "Mid", "High" };
    const char* ids[kBands]   = { "low", "mid", "high" };
    for (int b = 0; b < kBands; ++b)
        layout.add (std::make_unique<F> (juce::ParameterID { ids[b], 1 }, names[b],
            juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f, A().withLabel (" %")));

    layout.add (std::make_unique<F> (juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -12.0f, 12.0f, 0.01f }, 0.0f, A().withLabel (" dB")));
    layout.add (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f, A().withLabel (" %")));

    auto logRange = [] (float lo, float hi, float centre) {
        juce::NormalisableRange<float> r { lo, hi }; r.setSkewForCentre (centre); return r;
    };
    layout.add (std::make_unique<F> (juce::ParameterID { "lowfreq", 1 }, "Low/Mid",
        logRange (80.0f, 600.0f, 250.0f), 250.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { "highfreq", 1 }, "Mid/High",
        logRange (1500.0f, 9000.0f, 4000.0f), 4000.0f, A().withLabel (" Hz")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

VocalMbCompAudioProcessor::VocalMbCompAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    compressParam = apvts.getRawParameterValue ("compress");
    bandTrim[0]   = apvts.getRawParameterValue ("low");
    bandTrim[1]   = apvts.getRawParameterValue ("mid");
    bandTrim[2]   = apvts.getRawParameterValue ("high");
    outputParam   = apvts.getRawParameterValue ("output");
    mixParam      = apvts.getRawParameterValue ("mix");
    lowFreqParam  = apvts.getRawParameterValue ("lowfreq");
    highFreqParam = apvts.getRawParameterValue ("highfreq");
    bypassParam   = apvts.getRawParameterValue ("bypass");
}

void VocalMbCompAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    mb.prepare (sampleRate);
    for (int b = 0; b < kBands; ++b)
    {
        mb.band (b).setAttackMs (kAttackMs[b]);
        mb.band (b).setReleaseMs (kReleaseMs[b]);
        mb.band (b).prepare (sampleRate);
        bandGr[(size_t) b].store (0.0f);
    }

    // Log-domain smoothing of the crossover frequencies (~40 ms ramp) so
    // automation of Low/Mid and Mid/High moves the LR4 cutoffs continuously
    // instead of overwriting the biquad coeffs discontinuously each block.
    constexpr double kRampSeconds = 0.04;
    const double low  = (double) lowFreqParam->load();
    const double high = (double) highFreqParam->load();
    lowFreqSmoothed.reset (sampleRate, kRampSeconds);
    highFreqSmoothed.reset (sampleRate, kRampSeconds);
    lowFreqSmoothed.setCurrentAndTargetValue (std::log (low));
    highFreqSmoothed.setCurrentAndTargetValue (std::log (high));

    // Seed the crossover with the current frequencies and prime the cache so
    // processBlock only recomputes coeffs when the smoothed value moves.
    mb.setCrossover (low, high);
    lastLowFreq  = low;
    lastHighFreq = high;
}

bool VocalMbCompAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void VocalMbCompAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
    {
        for (auto& g : bandGr) g.store (0.0f);
        return;
    }

    // Move the smoothing targets toward the current parameter values (in the
    // log domain). The actual coeff updates happen at sub-block granularity in
    // the sample loop below, only when the smoothed frequency actually moves.
    lowFreqSmoothed.setTargetValue (std::log ((double) lowFreqParam->load()));
    highFreqSmoothed.setTargetValue (std::log ((double) highFreqParam->load()));

    mb.setMix (mixParam->load() * 0.01);

    // Map the macro controls onto each band's threshold/ratio + auto makeup.
    const double compress = compressParam->load() * 0.01;
    constexpr double refDb = -3.0; // level the auto makeup restores
    for (int b = 0; b < kBands; ++b)
    {
        const double a = compress * (bandTrim[(size_t) b]->load() * 0.01);
        const double ratio = juce::jmap (a, 0.0, 1.0, 1.0, 4.0);
        const double thr   = juce::jmap (a, 0.0, 1.0, -8.0, -32.0);
        auto& c = mb.band (b);
        c.setThresholdDb (thr);
        c.setRatio (ratio);
        c.setMakeupDb (-c.staticGainDb (refDb)); // auto makeup
    }

    const double outGain = juce::Decibels::decibelsToGain (outputParam->load());

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    std::array<float, kBands> minGr { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < numSamples; ++i)
    {
        // Advance the smoothed crossover frequencies and push new coeffs to the
        // crossover at sub-block granularity, only when they actually changed.
        if ((i % kXoverUpdateSamples) == 0)
        {
            const double low  = std::exp (lowFreqSmoothed.skip (i == 0 ? 0 : kXoverUpdateSamples));
            const double high = std::exp (highFreqSmoothed.skip (i == 0 ? 0 : kXoverUpdateSamples));
            if (low != lastLowFreq || high != lastHighFreq)
            {
                mb.setCrossover (low, high);
                lastLowFreq  = low;
                lastHighFreq = high;
            }
        }

        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        mb.processStereo (l, r);
        L[i] = (float) (l * outGain);
        if (R != nullptr) R[i] = (float) (r * outGain);
        for (int b = 0; b < kBands; ++b)
            minGr[(size_t) b] = juce::jmin (minGr[(size_t) b], (float) mb.bandGainReductionDb (b));
    }
    for (int b = 0; b < kBands; ++b)
        bandGr[(size_t) b].store (minGr[(size_t) b]);
}

juce::AudioProcessorEditor* VocalMbCompAudioProcessor::createEditor()
{
    return new VocalMbCompAudioProcessorEditor (*this);
}

void VocalMbCompAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalMbCompAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalMbCompAudioProcessor();
}
