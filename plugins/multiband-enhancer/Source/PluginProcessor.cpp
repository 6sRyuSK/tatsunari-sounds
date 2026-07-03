#include "PluginProcessor.h"
#include "PluginEditor.h"

using ME = factory_core::MultibandEnhancer;

juce::AudioProcessorValueTreeState::ParameterLayout
MultibandEnhancerAudioProcessor::createParameterLayout()
{
    using F = juce::AudioParameterFloat;
    using C = juce::AudioParameterChoice;
    using B = juce::AudioParameterBool;
    using A = juce::AudioParameterFloatAttributes;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Per-band Enhance (0..100 %, skewed toward the low end) and Width (0..200 %).
    for (int b = 0; b < kBands; ++b)
    {
        juce::NormalisableRange<float> enhR { 0.0f, 100.0f, 0.1f }; enhR.setSkewForCentre (30.0f);
        layout.add (std::make_unique<F> (juce::ParameterID { enhId (b), 1 },
            juce::String ("Enhance ") + kBandNames[b], enhR, 0.0f, A().withLabel (" %")));
        layout.add (std::make_unique<F> (juce::ParameterID { widthId (b), 1 },
            juce::String ("Width ") + kBandNames[b],
            juce::NormalisableRange<float> { 0.0f, 200.0f, 0.1f }, 100.0f, A().withLabel (" %")));
    }

    // Crossovers (log ranges, defaults from the plan).
    auto logRange = [] (float lo, float hi, float centre) {
        juce::NormalisableRange<float> r { lo, hi }; r.setSkewForCentre (centre); return r;
    };
    layout.add (std::make_unique<F> (juce::ParameterID { xoverId (0), 1 }, "Crossover 1",
        logRange (40.0f, 300.0f, 130.0f), 130.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { xoverId (1), 1 }, "Crossover 2",
        logRange (200.0f, 1200.0f, 700.0f), 700.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { xoverId (2), 1 }, "Crossover 3",
        logRange (800.0f, 5000.0f, 2200.0f), 2200.0f, A().withLabel (" Hz")));
    layout.add (std::make_unique<F> (juce::ParameterID { xoverId (3), 1 }, "Crossover 4",
        logRange (3000.0f, 18000.0f, 7500.0f), 7500.0f, A().withLabel (" Hz")));

    layout.add (std::make_unique<C> (juce::ParameterID { "mode", 1 }, "Mode",
        juce::StringArray { "Tube", "Tape", "Bright", "Clean", "Glue" }, 0));

    layout.add (std::make_unique<F> (juce::ParameterID { "direct", 1 }, "Direct",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.01f }, 0.0f, A().withLabel (" dB")));
    layout.add (std::make_unique<F> (juce::ParameterID { "wet", 1 }, "Enhanced",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.01f }, -12.0f, A().withLabel (" dB")));
    layout.add (std::make_unique<F> (juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, A().withLabel (" dB")));

    layout.add (std::make_unique<C> (juce::ParameterID { "quality", 1 }, "Quality",
        juce::StringArray { "HQ", "Zero Latency" }, 0));
    layout.add (std::make_unique<B> (juce::ParameterID { "delta", 1 }, "Delta Listen", false));
    layout.add (std::make_unique<B> (juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

MultibandEnhancerAudioProcessor::MultibandEnhancerAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (int b = 0; b < kBands; ++b)
    {
        enhP[b]   = apvts.getRawParameterValue (enhId (b));
        widthP[b] = apvts.getRawParameterValue (widthId (b));
    }
    for (int i = 0; i < 4; ++i) xovP[i] = apvts.getRawParameterValue (xoverId (i));
    modeP    = apvts.getRawParameterValue ("mode");
    directP  = apvts.getRawParameterValue ("direct");
    wetP     = apvts.getRawParameterValue ("wet");
    outputP  = apvts.getRawParameterValue ("output");
    qualityP = apvts.getRawParameterValue ("quality");
    deltaP   = apvts.getRawParameterValue ("delta");
    bypassP  = apvts.getRawParameterValue ("bypass");
}

void MultibandEnhancerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int maxBlock = juce::jmax (1, samplesPerBlock);

    engine.prepare (sampleRate, maxBlock);

    deltaL.assign ((size_t) maxBlock, 0.0f);
    deltaR.assign ((size_t) maxBlock, 0.0f);
    dryL.assign ((size_t) maxBlock, 0.0f);
    dryR.assign ((size_t) maxBlock, 0.0f);
    rBuf.assign ((size_t) maxBlock, 0.0f);
    dryHistL.fill (0.0f);
    dryHistR.fill (0.0f);

    bypassMix.reset (sampleRate, 0.02);
    bypassMix.setCurrentAndTargetValue (bypassP->load() > 0.5f ? 1.0f : 0.0f);

    ringPre.fill (0.0f); ringPost.fill (0.0f); ringDelta.fill (0.0f);
    ringWrite.store (0, std::memory_order_relaxed);
    for (auto& r : bandRmsDb) r.store (-120.0f, std::memory_order_relaxed);

    reportedLatency.store (engine.latencySamples(), std::memory_order_relaxed);
    setLatencySamples (engine.latencySamples());
    (void) engine.consumeLatencyDirty(); // clear the prepare-time flag
}

bool MultibandEnhancerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void MultibandEnhancerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int n = buffer.getNumSamples();
    if (n <= 0) return;
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    float* L   = buffer.getWritePointer (0);
    float* Rin = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // Capture the dry input (preserved for the latency-matched bypass reference).
    for (int i = 0; i < n; ++i) { dryL[(size_t) i] = L[i]; dryR[(size_t) i] = Rin ? Rin[i] : L[i]; }

    // Push parameters to the engine (plain, lock-free stores).
    for (int b = 0; b < kBands; ++b)
    {
        engine.setEnhance (b, (double) enhP[b]->load());
        engine.setWidth   (b, (double) widthP[b]->load());
    }
    engine.setCrossovers ((double) xovP[0]->load(), (double) xovP[1]->load(),
                          (double) xovP[2]->load(), (double) xovP[3]->load());
    engine.setMode    ((ME::Mode) (int) modeP->load());
    engine.setDirectDb ((double) directP->load());
    engine.setWetDb    ((double) wetP->load());
    engine.setOutputDb ((double) outputP->load());
    engine.setQuality  (qualityP->load() > 0.5f ? ME::Quality::ZeroLatency : ME::Quality::HQ);
    engine.setDeltaListen (deltaP->load() > 0.5f);

    const int lat = juce::jlimit (0, kMaxLatency - 1, engine.latencySamples());

    // Engine processes in place; mono uses a scratch right channel.
    float* Rwork = Rin ? Rin : rBuf.data();
    if (! Rin) for (int i = 0; i < n; ++i) Rwork[i] = dryL[(size_t) i];
    engine.processBlock (L, Rwork, n, deltaL.data(), deltaR.data());

    bypassMix.setTargetValue (bypassP->load() > 0.5f ? 1.0f : 0.0f);

    int w = ringWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        const float dld = (i >= lat) ? dryL[(size_t) (i - lat)] : dryHistL[(size_t) (kMaxLatency - lat + i)];
        const float drd = (i >= lat) ? dryR[(size_t) (i - lat)] : dryHistR[(size_t) (kMaxLatency - lat + i)];

        const float xf = bypassMix.getNextValue();
        const float outL = L[i]     * (1.0f - xf) + dld * xf;
        const float outR = Rwork[i] * (1.0f - xf) + drd * xf;

        L[i] = outL;
        if (Rin) Rin[i] = outR;

        const int idx = w & kRingMask;
        ringPre[(size_t) idx]   = 0.5f * (dryL[(size_t) i] + dryR[(size_t) i]);
        ringPost[(size_t) idx]  = 0.5f * (outL + outR);
        ringDelta[(size_t) idx] = 0.5f * (deltaL[(size_t) i] + deltaR[(size_t) i]);
        ++w;
    }
    ringWrite.store (w, std::memory_order_release);

    // Slide the dry-reference history (keep the last kMaxLatency dry samples).
    if (n >= kMaxLatency)
    {
        for (int k = 0; k < kMaxLatency; ++k)
        {
            dryHistL[(size_t) k] = dryL[(size_t) (n - kMaxLatency + k)];
            dryHistR[(size_t) k] = dryR[(size_t) (n - kMaxLatency + k)];
        }
    }
    else
    {
        for (int k = 0; k < kMaxLatency - n; ++k) { dryHistL[(size_t) k] = dryHistL[(size_t) (k + n)]; dryHistR[(size_t) k] = dryHistR[(size_t) (k + n)]; }
        for (int k = 0; k < n; ++k) { dryHistL[(size_t) (kMaxLatency - n + k)] = dryL[(size_t) k]; dryHistR[(size_t) (kMaxLatency - n + k)] = dryR[(size_t) k]; }
    }

    // Publish meters / effective crossovers.
    for (int b = 0; b < kBands; ++b)
        bandRmsDb[(size_t) b].store ((float) engine.bandResidualRmsDb (b), std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        effXover[(size_t) i].store (engine.effectiveCrossoverHz (i), std::memory_order_relaxed);

    // A Quality change moves the reported latency: apply it on the message thread.
    if (engine.consumeLatencyDirty())
    {
        reportedLatency.store (engine.latencySamples(), std::memory_order_relaxed);
        triggerAsyncUpdate();
    }
}

void MultibandEnhancerAudioProcessor::handleAsyncUpdate()
{
    setLatencySamples (reportedLatency.load (std::memory_order_relaxed));
    updateHostDisplay();
}

void MultibandEnhancerAudioProcessor::copyRing (float* dest, int num, int which) const noexcept
{
    const std::array<float, kRingSize>* src =
        (which == RingPre) ? &ringPre : (which == RingPost) ? &ringPost : &ringDelta;
    const int w = ringWrite.load (std::memory_order_acquire);
    const int start = w - num;
    for (int i = 0; i < num; ++i)
        dest[i] = (*src)[(size_t) ((start + i) & kRingMask)];
}

juce::AudioProcessorEditor* MultibandEnhancerAudioProcessor::createEditor()
{
    return new MultibandEnhancerAudioProcessorEditor (*this);
}

void MultibandEnhancerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MultibandEnhancerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MultibandEnhancerAudioProcessor();
}
