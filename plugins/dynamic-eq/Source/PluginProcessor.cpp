#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Bands start empty (off); the user adds them by double-clicking the curve,
    // which sets type/freq/gain. These defaults only seed an unused band's
    // parameters: frequencies log-spread across the spectrum, all bells.
    float defaultFreq (int band)
    {
        const float t = (DynamicEqAudioProcessor::kNumBands > 1)
                          ? (float) band / (float) (DynamicEqAudioProcessor::kNumBands - 1) : 0.0f;
        return 20.0f * std::pow (1000.0f, t); // 20 Hz .. 20 kHz
    }
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
            juce::ParameterID { pid (b, "on"), 1 }, "Band " + juce::String (b + 1) + " On", false));

        // Present-but-bypassed: the band stays on the graph but is not processed.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (b, "byp"), 1 }, "Band " + juce::String (b + 1) + " Bypass", false));

        // Solo / listen: audition only this band's range (exclusive, enforced in UI).
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (b, "lsn"), 1 }, "Band " + juce::String (b + 1) + " Listen", false));

        // Channel target.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (b, "chan"), 1 }, "Band " + juce::String (b + 1) + " Channel",
            juce::StringArray { "Stereo", "Left", "Right", "Mid", "Side" }, 0));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (b, "type"), 1 }, "Band " + juce::String (b + 1) + " Type",
            juce::StringArray { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass" },
            0));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "freq"), 1 }, "Band " + juce::String (b + 1) + " Freq",
            freqRange, defaultFreq (b), juce::AudioParameterFloatAttributes().withLabel (" Hz")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "gain"), 1 }, "Band " + juce::String (b + 1) + " Gain",
            juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel (" dB")));

        juce::NormalisableRange<float> qRange { 0.1f, 18.0f };
        qRange.setSkewForCentre (1.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "q"), 1 }, "Band " + juce::String (b + 1) + " Q",
            qRange, 0.707f));

        // High/Low-pass slope: 12..96 dB/oct (Butterworth cascade). Index k => (k+1) sections.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (b, "slope"), 1 }, "Band " + juce::String (b + 1) + " Slope",
            juce::StringArray { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct",
                                "60 dB/oct", "72 dB/oct", "84 dB/oct", "96 dB/oct" }, 0));

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

        juce::NormalisableRange<float> atkRange { 0.05f, 100.0f };
        atkRange.setSkewForCentre (10.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "atk"), 1 }, "Band " + juce::String (b + 1) + " Attack",
            atkRange, 10.0f, juce::AudioParameterFloatAttributes().withLabel (" ms")));

        juce::NormalisableRange<float> relRange { 5.0f, 2000.0f };
        relRange.setSkewForCentre (120.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "rel"), 1 }, "Band " + juce::String (b + 1) + " Release",
            relRange, 120.0f, juce::AudioParameterFloatAttributes().withLabel (" ms")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "knee"), 1 }, "Band " + juce::String (b + 1) + " Knee",
            juce::NormalisableRange<float> { 0.0f, 24.0f, 0.01f }, 6.0f,
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
        params[(size_t) b].byp  = apvts.getRawParameterValue (pid (b, "byp"));
        params[(size_t) b].lsn  = apvts.getRawParameterValue (pid (b, "lsn"));
        params[(size_t) b].chan = apvts.getRawParameterValue (pid (b, "chan"));
        params[(size_t) b].type = apvts.getRawParameterValue (pid (b, "type"));
        params[(size_t) b].freq = apvts.getRawParameterValue (pid (b, "freq"));
        params[(size_t) b].gain = apvts.getRawParameterValue (pid (b, "gain"));
        params[(size_t) b].q     = apvts.getRawParameterValue (pid (b, "q"));
        params[(size_t) b].slope = apvts.getRawParameterValue (pid (b, "slope"));
        params[(size_t) b].dyn   = apvts.getRawParameterValue (pid (b, "dyn"));
        params[(size_t) b].thr   = apvts.getRawParameterValue (pid (b, "thr"));
        params[(size_t) b].rng   = apvts.getRawParameterValue (pid (b, "rng"));
        params[(size_t) b].atk   = apvts.getRawParameterValue (pid (b, "atk"));
        params[(size_t) b].rel   = apvts.getRawParameterValue (pid (b, "rel"));
        params[(size_t) b].knee  = apvts.getRawParameterValue (pid (b, "knee"));
    }
    bypassParam = apvts.getRawParameterValue ("bypass");
    bypassParamPtr = apvts.getParameter ("bypass");

    programs.configure (apvts, dynamic_eq_presets::bank,
                        dynamic_eq_presets::kExclude, dynamic_eq_presets::kNumExclude);
}

void DynamicEqAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
    for (auto& band : bands)
        band.prepare (sampleRate);
    analyzerRing.fill (0.0f);
    analyzerRingPost.fill (0.0f);
    ringWrite.store (0);

    // Ramp Freq / Gain / Q over ~30 ms; reset each smoother to its current
    // parameter value so the first block starts settled (no ramp from zero).
    constexpr double kRampSeconds = 0.03;
    for (int b = 0; b < kNumBands; ++b)
    {
        freqSmooth[(size_t) b].reset (sampleRate, kRampSeconds);
        gainSmooth[(size_t) b].reset (sampleRate, kRampSeconds);
        qSmooth[(size_t) b].reset (sampleRate, kRampSeconds);
        freqSmooth[(size_t) b].setCurrentAndTargetValue (params[(size_t) b].freq->load());
        gainSmooth[(size_t) b].setCurrentAndTargetValue (params[(size_t) b].gain->load());
        qSmooth[(size_t) b].setCurrentAndTargetValue (params[(size_t) b].q->load());
        liveGainDb[(size_t) b].store (0.0f, std::memory_order_relaxed);
    }
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
    {
        // Keep the display honest while bypassed: report the static gains.
        for (int b = 0; b < kNumBands; ++b)
            liveGainDb[(size_t) b].store (params[(size_t) b].gain->load(), std::memory_order_relaxed);
        return;
    }

    // Configure bands from parameters (per block). The continuous Freq / Gain /
    // Q are NOT applied here — they are smoothed and applied in sub-block chunks
    // below (see the chunked loop) to avoid zipper noise on automation. Only the
    // non-continuous settings (type, channel, slope, knee, dynamics) are set
    // per block.
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bp = params[(size_t) b];
        auto& band = bands[(size_t) b];
        const bool present  = bp.on->load() > 0.5f;
        const bool bypassed = bp.byp->load() > 0.5f;
        band.setEnabled (present && ! bypassed);
        if (! present)
            continue; // free slot: skip coefficient work entirely
        // Present (even if bypassed) bands are configured so Listen/solo works.
        band.setType (static_cast<factory_core::BandType> ((int) bp.type->load()));
        band.setChannelMode (static_cast<factory_core::ChannelMode> ((int) bp.chan->load()));
        band.setSlopeStages ((int) bp.slope->load() + 1); // choice index 0..7 -> 1..8 sections
        band.setKnee (bp.knee->load());
        band.setDynamics (bp.dyn->load() > 0.5f, bp.thr->load(), bp.rng->load());
        band.setDynamicsTimes (bp.atk->load(), bp.rel->load());

        // Smooth the continuous Freq / Gain / Q toward the current parameter.
        freqSmooth[(size_t) b].setTargetValue (bp.freq->load());
        gainSmooth[(size_t) b].setTargetValue (bp.gain->load());
        qSmooth[(size_t) b].setTargetValue (bp.q->load());
    }

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // Exclusive Listen/solo: audition the lowest present band that has it on.
    int soloBand = -1;
    for (int b = 0; b < kNumBands; ++b)
        if (params[(size_t) b].on->load() > 0.5f && params[(size_t) b].lsn->load() > 0.5f)
        { soloBand = b; break; }

    int w = ringWrite.load (std::memory_order_relaxed);
    for (int start = 0; start < numSamples; start += kSmoothChunk)
    {
        const int end = juce::jmin (start + kSmoothChunk, numSamples);
        const int chunk = end - start;

        // Advance the smoothers by this chunk and reconfigure each present
        // band's Freq / Gain / Q + coefficients before processing the chunk.
        for (int b = 0; b < kNumBands; ++b)
        {
            if (params[(size_t) b].on->load() <= 0.5f)
            {
                // Keep the smoothers moving so a freed slot doesn't jump later.
                freqSmooth[(size_t) b].skip (chunk);
                gainSmooth[(size_t) b].skip (chunk);
                qSmooth[(size_t) b].skip (chunk);
                continue;
            }
            auto& band = bands[(size_t) b];
            band.setFrequency (freqSmooth[(size_t) b].skip (chunk));
            band.setGainDb    (gainSmooth[(size_t) b].skip (chunk));
            band.setQ         (qSmooth[(size_t) b].skip (chunk));
            band.updateCoefficients();
        }

        for (int i = start; i < end; ++i)
        {
            double l = L[i];
            double r = (R != nullptr) ? R[i] : l;

            analyzerRing[(size_t) (w & kRingMask)] = (float) (0.5 * (l + r)); // pre-EQ

            if (soloBand >= 0)
                bands[(size_t) soloBand].processListen (l, r); // band-pass of the dry input
            else
                for (auto& band : bands)
                    band.processStereo (l, r);

            analyzerRingPost[(size_t) (w & kRingMask)] = (float) (0.5 * (l + r)); // post-EQ
            ++w;

            L[i] = (float) l;
            if (R != nullptr) R[i] = (float) r;
        }
    }
    ringWrite.store (w, std::memory_order_release);

    // Publish each band's effective (post-dynamics) gain for the editor.
    for (int b = 0; b < kNumBands; ++b)
        liveGainDb[(size_t) b].store ((float) bands[(size_t) b].currentGainDb(),
                                      std::memory_order_relaxed);
}

void DynamicEqAudioProcessor::copyAnalyzerSamples (float* dest, int num, bool post) const noexcept
{
    const int w = ringWrite.load (std::memory_order_acquire);
    const auto& ring = post ? analyzerRingPost : analyzerRing;
    for (int i = 0; i < num; ++i)
        dest[i] = ring[(size_t) ((w - num + i) & kRingMask)];
}

juce::AudioProcessorEditor* DynamicEqAudioProcessor::createEditor()
{
    return new DynamicEqAudioProcessorEditor (*this);
}

void DynamicEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void DynamicEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DynamicEqAudioProcessor();
}
