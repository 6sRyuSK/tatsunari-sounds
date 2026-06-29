#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Sensible defaults spread across the spectrum.
    constexpr float kDefaultFreq[6] = { 80.0f, 250.0f, 800.0f, 2000.0f, 5000.0f, 12000.0f };
    // Band 0 low shelf, band 5 high shelf, the rest bells.
    int defaultType (int band) { return band == 0 ? 1 : (band == 5 ? 2 : 0); }
}

juce::String DynamicEqAudioProcessor::pid (int band, const char* suffix)
{
    return "b" + juce::String (band) + "_" + suffix;
}

juce::AudioProcessorValueTreeState::ParameterLayout
DynamicEqAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    juce::NormalisableRange<float> freqRange { 20.0f, 20000.0f };
    freqRange.setSkewForCentre (632.455f);

    for (int b = 0; b < kNumBands; ++b)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (b, "on"), 1 }, "Band " + juce::String (b + 1) + " On", true));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (b, "type"), 1 }, "Band " + juce::String (b + 1) + " Type",
            juce::StringArray { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass" },
            defaultType (b)));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "freq"), 1 }, "Band " + juce::String (b + 1) + " Freq",
            freqRange, kDefaultFreq[b], juce::AudioParameterFloatAttributes().withLabel (" Hz")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "gain"), 1 }, "Band " + juce::String (b + 1) + " Gain",
            juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel (" dB")));

        juce::NormalisableRange<float> qRange { 0.1f, 18.0f };
        qRange.setSkewForCentre (1.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "q"), 1 }, "Band " + juce::String (b + 1) + " Q",
            qRange, 0.707f));

        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (b, "dyn"), 1 }, "Band " + juce::String (b + 1) + " Dynamics", false));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "thr"), 1 }, "Band " + juce::String (b + 1) + " Threshold",
            juce::NormalisableRange<float> { -60.0f, 0.0f, 0.01f }, -24.0f,
            juce::AudioParameterFloatAttributes().withLabel (" dB")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "rng"), 1 }, "Band " + juce::String (b + 1) + " Range",
            juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel (" dB")));
    }

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

DynamicEqAudioProcessor::DynamicEqAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (int b = 0; b < kNumBands; ++b)
    {
        params[(size_t) b].on   = apvts.getRawParameterValue (pid (b, "on"));
        params[(size_t) b].type = apvts.getRawParameterValue (pid (b, "type"));
        params[(size_t) b].freq = apvts.getRawParameterValue (pid (b, "freq"));
        params[(size_t) b].gain = apvts.getRawParameterValue (pid (b, "gain"));
        params[(size_t) b].q    = apvts.getRawParameterValue (pid (b, "q"));
        params[(size_t) b].dyn  = apvts.getRawParameterValue (pid (b, "dyn"));
        params[(size_t) b].thr  = apvts.getRawParameterValue (pid (b, "thr"));
        params[(size_t) b].rng  = apvts.getRawParameterValue (pid (b, "rng"));
    }
    bypassParam = apvts.getRawParameterValue ("bypass");
}

void DynamicEqAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
    for (auto& band : bands)
        band.prepare (sampleRate);
    analyzerRing.fill (0.0f);
    ringWrite.store (0);
}

bool DynamicEqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void DynamicEqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
        return;

    // Configure bands from parameters (per block).
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bp = params[(size_t) b];
        auto& band = bands[(size_t) b];
        band.setEnabled (bp.on->load() > 0.5f);
        band.setType (static_cast<factory_core::BandType> ((int) bp.type->load()));
        band.setFrequency (bp.freq->load());
        band.setGainDb (bp.gain->load());
        band.setQ (bp.q->load());
        band.setDynamics (bp.dyn->load() > 0.5f, bp.thr->load(), bp.rng->load());
        band.updateCoefficients();
    }

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    int w = ringWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i)
    {
        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;

        analyzerRing[(size_t) (w & kRingMask)] = (float) (0.5 * (l + r));
        ++w;

        for (auto& band : bands)
            band.processStereo (l, r);

        L[i] = (float) l;
        if (R != nullptr) R[i] = (float) r;
    }
    ringWrite.store (w, std::memory_order_release);
}

void DynamicEqAudioProcessor::copyAnalyzerSamples (float* dest, int num) const noexcept
{
    const int w = ringWrite.load (std::memory_order_acquire);
    for (int i = 0; i < num; ++i)
        dest[i] = analyzerRing[(size_t) ((w - num + i) & kRingMask)];
}

juce::AudioProcessorEditor* DynamicEqAudioProcessor::createEditor()
{
    return new DynamicEqAudioProcessorEditor (*this);
}

void DynamicEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DynamicEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DynamicEqAudioProcessor();
}
