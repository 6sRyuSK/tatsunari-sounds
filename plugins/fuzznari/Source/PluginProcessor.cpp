#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
FuzznariAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "drive", 1 }, "Drive",
        juce::NormalisableRange<float> { 0.0f, 48.0f, 0.01f }, 24.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "bias", 1 }, "Bias",
        juce::NormalisableRange<float> { -100.0f, 100.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "gate", 1 }, "Gate",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "stab", 1 }, "Stab",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // Self-oscillation enable. Defaults OFF so a fresh instance can never make
    // sound from silence; with it off the feedback ceiling keeps the loop gain
    // strictly below 1 at every setting.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "osc", 1 }, "Squeal", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone", 1 }, "Tone",
        juce::NormalisableRange<float> { -100.0f, 100.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "level", 1 }, "Level",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

FuzznariAudioProcessor::FuzznariAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    driveParam  = apvts.getRawParameterValue ("drive");
    biasParam   = apvts.getRawParameterValue ("bias");
    gateParam   = apvts.getRawParameterValue ("gate");
    stabParam   = apvts.getRawParameterValue ("stab");
    oscParam    = apvts.getRawParameterValue ("osc");
    toneParam   = apvts.getRawParameterValue ("tone");
    levelParam  = apvts.getRawParameterValue ("level");
    mixParam    = apvts.getRawParameterValue ("mix");
    bypassParam = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, fuzznari_presets::bank,
                        fuzznari_presets::kExclude, fuzznari_presets::kNumExclude);
}

void FuzznariAudioProcessor::pushParamsToEngine() noexcept
{
    engine.setDriveDb ((double) driveParam->load());
    engine.setBias ((double) biasParam->load() * 0.01);
    engine.setGate ((double) gateParam->load() * 0.01);
    engine.setOscEnabled (oscParam->load() > 0.5f);
    engine.setStab ((double) stabParam->load() * 0.01);
    engine.setTone ((double) toneParam->load() * 0.01);
    engine.setLevelDb ((double) levelParam->load());
    engine.setMix ((double) mixParam->load() * 0.01);
    engine.setBypassed (bypassParam->load() > 0.5f);
}

void FuzznariAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    monoScratch.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);

    // Targets first, then prepare: prepare() snaps the engine smoothers to the
    // current targets so playback starts from the saved state without a ramp.
    pushParamsToEngine();
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (engine.latencySamples());
}

bool FuzznariAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void FuzznariAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    const int n        = buffer.getNumSamples();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, n);

    pushParamsToEngine();

    const bool stereo = juce::jmin (totalIn, totalOut) >= 2;
    float* left = buffer.getWritePointer (0);

    if (stereo)
    {
        float* right = buffer.getWritePointer (1);
        engine.process (left, right, left, right, n);
    }
    else
    {
        // The engine is stereo-shaped (RateBracket precedent from nam-player):
        // feed channel 0 to both inputs and discard the scratch right channel.
        int pos = 0;
        while (pos < n)
        {
            const int m = juce::jmin (n - pos, (int) monoScratch.size());
            engine.process (left + pos, left + pos, left + pos, monoScratch.data(), m);
            pos += m;
        }
    }

    // Publish the output to the editor's scope ring (release store pairs with
    // the acquire load in copyScopeSamples).
    int w = ringWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        scopeRing[(size_t) ((w + i) & kRingMask)] = left[i];
    ringWrite.store (w + n, std::memory_order_release);
}

juce::AudioProcessorEditor* FuzznariAudioProcessor::createEditor()
{
    return new FuzznariAudioProcessorEditor (*this);
}

void FuzznariAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        // Append the selected program index (attribute only — existing sessions
        // without it read back as program 0, so state stays compatible).
        programs.writeStateAttribute (*xml);
        copyXmlToBinary (*xml, destData);
    }
}

void FuzznariAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
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
    return new FuzznariAudioProcessor();
}
