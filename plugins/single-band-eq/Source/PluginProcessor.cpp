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

    // Ramp state (#32): ~8 ms coefficient crossfade, derived from the sample rate.
    currentCoeffs = initial;
    targetCoeffs  = initial;
    lastApplied   = initial;
    rampSamples   = juce::jmax (1, (int) std::round (0.008 * sampleRate));
    rampRemaining = 0;

    wasBypassed = false; // (#41)

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
    // Rather than swapping wholesale (which steps discontinuously against the
    // biquad z-state => clicks, #32), start a ramp from currentCoeffs to the new
    // target; the ramp is applied at sub-block granularity below.
    if (const int ready = coeffFifo.getNumReady(); ready > 0)
    {
        int start1, size1, start2, size2;
        coeffFifo.prepareToRead (ready, start1, size1, start2, size2);

        factory_core::BiquadCoeffs latest;
        if (size2 > 0)      latest = coeffStore[(size_t) (start2 + size2 - 1)];
        else                latest = coeffStore[(size_t) (start1 + size1 - 1)];

        coeffFifo.finishedRead (ready);

        // If a ramp is still in flight, start the new one from where we are now
        // (the last interpolated coeffs applied to the filters) so the crossfade
        // stays continuous instead of jumping back to the last settled point.
        if (rampRemaining > 0)
            currentCoeffs = lastApplied;

        targetCoeffs  = latest;
        rampRemaining = rampSamples; // (re)start the crossfade toward the new target
    }

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Bypass (#41): on the bypass->active transition, clear filter z-state so
    // stale state can't click back in. A hard branch by itself leaves that state.
    if (bypassParam->load() > 0.5f)
    {
        wasBypassed = true;
        return;
    }

    if (wasBypassed)
    {
        filters[0].reset();
        filters[1].reset();
        wasBypassed = false;
    }

    const int numSamples = buffer.getNumSamples();
    const int numCh      = juce::jmin (buffer.getNumChannels(), 2);

    // Process in sub-blocks, interpolating the 5 coefficients across the ramp so
    // the change is applied smoothly. When not ramping this collapses to a single
    // pass with the current coeffs — no per-sample cost.
    int pos = 0;
    while (pos < numSamples)
    {
        const int chunk = rampRemaining > 0
                              ? juce::jmin (kRampUpdateInterval, numSamples - pos)
                              : numSamples - pos;

        if (rampRemaining > 0)
        {
            // Linear interpolation of each coefficient toward the target. `t` is
            // how far along the ramp the end of this chunk sits.
            rampRemaining = juce::jmax (0, rampRemaining - chunk);
            const double t = 1.0 - (double) rampRemaining / (double) rampSamples;

            factory_core::BiquadCoeffs c;
            c.b0 = currentCoeffs.b0 + (targetCoeffs.b0 - currentCoeffs.b0) * t;
            c.b1 = currentCoeffs.b1 + (targetCoeffs.b1 - currentCoeffs.b1) * t;
            c.b2 = currentCoeffs.b2 + (targetCoeffs.b2 - currentCoeffs.b2) * t;
            c.a1 = currentCoeffs.a1 + (targetCoeffs.a1 - currentCoeffs.a1) * t;
            c.a2 = currentCoeffs.a2 + (targetCoeffs.a2 - currentCoeffs.a2) * t;

            lastApplied = c;
            for (int ch = 0; ch < numCh; ++ch)
                filters[(size_t) ch].setCoeffs (c);

            if (rampRemaining == 0)
                currentCoeffs = targetCoeffs; // settle exactly on the target
        }

        for (int ch = 0; ch < numCh; ++ch)
            filters[(size_t) ch].process (buffer.getWritePointer (ch) + pos, chunk);

        pos += chunk;
    }
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
