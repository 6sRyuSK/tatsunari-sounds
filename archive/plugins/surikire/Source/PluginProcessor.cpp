#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
SurikireAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto percent = [] (const char* id, const char* name, float defaultValue)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name,
            juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, defaultValue,
            juce::AudioParameterFloatAttributes().withLabel ("%"));
    };

    // Log-like knob travel for the tone filters: the geometric midpoint of the
    // range sits at the centre of the knob.
    const auto logHzRange = [] (float minHz, float maxHz)
    {
        juce::NormalisableRange<float> r { minHz, maxHz, 1.0f };
        r.setSkewForCentre (std::sqrt (minHz * maxHz));
        return r;
    };

    layout.add (percent ("wow",      "Wow",        25.0f));
    layout.add (percent ("flutter",  "Flutter",    15.0f));
    layout.add (percent ("gen",      "Generation", 35.0f));
    layout.add (percent ("saturate", "Saturate",   20.0f));
    layout.add (percent ("noise",    "Noise",      10.0f));
    layout.add (percent ("failure",  "Failure",     0.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "hp", 1 }, "HP",
        logHzRange (20.0f, 2000.0f), 20.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lp", 1 }, "LP",
        logHzRange (1000.0f, 20000.0f), 20000.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (percent ("mix", "Mix", 100.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

SurikireAudioProcessor::SurikireAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    wowParam      = apvts.getRawParameterValue ("wow");
    flutterParam  = apvts.getRawParameterValue ("flutter");
    genParam      = apvts.getRawParameterValue ("gen");
    saturateParam = apvts.getRawParameterValue ("saturate");
    noiseParam    = apvts.getRawParameterValue ("noise");
    failureParam  = apvts.getRawParameterValue ("failure");
    hpParam       = apvts.getRawParameterValue ("hp");
    lpParam       = apvts.getRawParameterValue ("lp");
    mixParam      = apvts.getRawParameterValue ("mix");
    outputParam   = apvts.getRawParameterValue ("output");
    bypassParam   = apvts.getRawParameterValue ("bypass");

    programs.configure (apvts, surikire_presets::bank,
                        surikire_presets::kExclude, surikire_presets::kNumExclude);
}

void SurikireAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    // All allocation happens here (inside engine.prepare); processBlock never
    // allocates. prepare() also resets the whole engine state (regression
    // policy: state reset on prepare and bypass transitions). Always prepare
    // the stereo worst case; process() clamps to the actual bus width.
    engine.prepare (sampleRate, 2);

    // prepare()'s internal reset ran before the stored targets were known:
    // push them, then reset again so the engine smoothers snap to the current
    // parameter values and playback starts exactly on the stored state.
    pushParametersToEngine();
    engine.reset();

    const bool bypassed = bypassParam->load() > 0.5f;
    wasBypassed = bypassed;

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));
}

bool SurikireAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void SurikireAudioProcessor::pushParametersToEngine()
{
    // The engine setters clamp and smooth (20 ms) internally; percentages map
    // to the engine's 0..1 domain, the filters take Hz directly.
    engine.setWow01        (wowParam->load()      * 0.01f);
    engine.setFlutter01    (flutterParam->load()  * 0.01f);
    engine.setGeneration01 (genParam->load()      * 0.01f);
    engine.setUserHpHz     (hpParam->load());
    engine.setUserLpHz     (lpParam->load());
    engine.setSaturate01   (saturateParam->load() * 0.01f);
    engine.setNoise01      (noiseParam->load()    * 0.01f);
    engine.setFailure01    (failureParam->load()  * 0.01f);
    engine.setMix01        (mixParam->load()      * 0.01f);
}

void SurikireAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Forward the atomic targets before any transition reset so reset() snaps
    // the engine smoothers to the current values.
    pushParametersToEngine();

    const bool bypassed = bypassParam->load() > 0.5f;
    if (bypassed != wasBypassed)
    {
        // Regression policy: full state reset on bypass transitions, both
        // directions (clears delay line, filters, LCGs, dropout timeline).
        engine.reset();
        wasBypassed = bypassed;
    }

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    const int numCh = juce::jmin (totalIn, totalOut);
    const int n     = buffer.getNumSamples();

    if (! bypassed)
        engine.process (buffer.getArrayOfWritePointers(), numCh, n);

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

juce::AudioProcessorEditor* SurikireAudioProcessor::createEditor()
{
    return new SurikireAudioProcessorEditor (*this);
}

void SurikireAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void SurikireAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SurikireAudioProcessor();
}
