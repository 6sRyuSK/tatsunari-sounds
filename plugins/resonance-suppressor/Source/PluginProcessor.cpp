#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

juce::String ResonanceSuppressorAudioProcessor::nodePid (int node, const char* suffix)
{
    return "n" + juce::String (node) + "_" + suffix;
}

juce::AudioProcessorValueTreeState::ParameterLayout
ResonanceSuppressorAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "depth", 1 }, "Depth",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "sharpness", 1 }, "Sharpness",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    NormalisableRange<float> lowR { 20.0f, 2000.0f }; lowR.setSkewForCentre (200.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "lowfreq", 1 }, "Low", lowR, 20.0f,
        AudioParameterFloatAttributes().withLabel (" Hz")));

    NormalisableRange<float> highR { 1000.0f, 20000.0f }; highR.setSkewForCentre (6000.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "highfreq", 1 }, "High", highR, 20000.0f,
        AudioParameterFloatAttributes().withLabel (" Hz")));

    NormalisableRange<float> atkR { 1.0f, 200.0f }; atkR.setSkewForCentre (20.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack", atkR, 15.0f,
        AudioParameterFloatAttributes().withLabel (" ms")));

    NormalisableRange<float> relR { 5.0f, 500.0f }; relR.setSkewForCentre (100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release", relR, 120.0f,
        AudioParameterFloatAttributes().withLabel (" ms")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "delta", 1 },  "Delta",  false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "link", 1 },   "Stereo Link", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    for (int n = 0; n < kNumNodes; ++n)
    {
        const float defF = 80.0f * std::pow (16000.0f / 80.0f, (float) n / (float) (kNumNodes - 1));
        NormalisableRange<float> fR { 20.0f, 20000.0f }; fR.setSkewForCentre (650.0f);
        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { nodePid (n, "on"),   1 }, "Node " + juce::String (n + 1) + " On", false));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { nodePid (n, "freq"), 1 }, "Node " + juce::String (n + 1) + " Freq", fR, defF,
                                                           AudioParameterFloatAttributes().withLabel (" Hz")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { nodePid (n, "amt"),  1 }, "Node " + juce::String (n + 1) + " Amount",
                                                           NormalisableRange<float> { -1.0f, 2.0f, 0.01f }, 1.0f));
    }

    return layout;
}

ResonanceSuppressorAudioProcessor::ResonanceSuppressorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    depthParam  = apvts.getRawParameterValue ("depth");
    sharpParam  = apvts.getRawParameterValue ("sharpness");
    lowParam    = apvts.getRawParameterValue ("lowfreq");
    highParam   = apvts.getRawParameterValue ("highfreq");
    atkParam    = apvts.getRawParameterValue ("attack");
    relParam    = apvts.getRawParameterValue ("release");
    mixParam    = apvts.getRawParameterValue ("mix");
    deltaParam  = apvts.getRawParameterValue ("delta");
    linkParam   = apvts.getRawParameterValue ("link");
    bypassParam = apvts.getRawParameterValue ("bypass");

    for (int n = 0; n < kNumNodes; ++n)
    {
        nodes[(size_t) n].on   = apvts.getRawParameterValue (nodePid (n, "on"));
        nodes[(size_t) n].freq = apvts.getRawParameterValue (nodePid (n, "freq"));
        nodes[(size_t) n].amt  = apvts.getRawParameterValue (nodePid (n, "amt"));
    }
}

void ResonanceSuppressorAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    suppressor.prepare (sampleRate, kFftOrder);
    setLatencySamples (suppressor.latencySamples());
    for (auto& a : pubMag) a.store (-120.0f, std::memory_order_relaxed);
    for (auto& a : pubRed) a.store (0.0f, std::memory_order_relaxed);
}

bool ResonanceSuppressorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ResonanceSuppressorAudioProcessor::rasterizeProfile()
{
    const double sr = currentSampleRate;
    const int N = 1 << kFftOrder;
    constexpr double sigma = 0.30; // gaussian half-width in natural-log frequency

    for (int k = 0; k < kNumBins; ++k) profileBuf[(size_t) k] = 1.0;

    for (int n = 0; n < kNumNodes; ++n)
    {
        if (nodes[(size_t) n].on->load() <= 0.5f) continue;
        const double f0 = nodes[(size_t) n].freq->load();
        const double a  = nodes[(size_t) n].amt->load();
        const double lf0 = std::log (juce::jmax (10.0, f0));
        for (int k = 1; k < kNumBins; ++k)
        {
            const double f = (double) k * sr / N;
            const double d = (std::log (juce::jmax (10.0, f)) - lf0) / sigma;
            profileBuf[(size_t) k] += a * std::exp (-0.5 * d * d);
        }
    }
    for (int k = 0; k < kNumBins; ++k)
        profileBuf[(size_t) k] = juce::jlimit (0.0, 4.0, profileBuf[(size_t) k]);

    suppressor.setProfile (profileBuf.data(), kNumBins);
}

void ResonanceSuppressorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
        return;

    suppressor.setDepth     ((double) depthParam->load() / 100.0 * 1.5);
    suppressor.setSharpness (0.15 + (double) sharpParam->load() / 100.0 * 0.85); // 0.15..1.0 octave
    suppressor.setRange     (lowParam->load(), highParam->load());
    suppressor.setTimes     (atkParam->load(), relParam->load());
    suppressor.setMix       ((double) mixParam->load() / 100.0);
    suppressor.setDelta     (deltaParam->load() > 0.5f);
    suppressor.setStereoLink (linkParam->load() > 0.5f);
    rasterizeProfile();

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        suppressor.process (l, r);
        L[i] = (float) l;
        if (R != nullptr) R[i] = (float) r;
    }

    // Publish the latest display spectra for the editor.
    std::array<double, kNumBins> scratch;
    const double* magDb = suppressor.magnitudeDb (scratch.data());
    const double* redDb = suppressor.reductionDb();
    for (int k = 0; k < kNumBins; ++k)
    {
        pubMag[(size_t) k].store ((float) magDb[k], std::memory_order_relaxed);
        pubRed[(size_t) k].store ((float) redDb[k], std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* ResonanceSuppressorAudioProcessor::createEditor()
{
    return new ResonanceSuppressorAudioProcessorEditor (*this);
}

void ResonanceSuppressorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ResonanceSuppressorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ResonanceSuppressorAudioProcessor();
}
