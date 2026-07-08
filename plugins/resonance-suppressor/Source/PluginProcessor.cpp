#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

juce::String ResonanceSuppressorAudioProcessor::cutPid (int which, const char* suffix)
{
    return juce::String (which == 0 ? "lc_" : "hc_") + suffix;
}

juce::String ResonanceSuppressorAudioProcessor::bandPid (int band, const char* suffix)
{
    return "b" + juce::String (band) + "_" + suffix;
}

double ResonanceSuppressorAudioProcessor::slopeValue (int index) noexcept
{
    static constexpr double kSlopes[] = { 6.0, 12.0, 24.0, 48.0 };
    return kSlopes[(size_t) juce::jlimit (0, 3, index)];
}

factory_core::ReductionNodes
ResonanceSuppressorAudioProcessor::readNodes (juce::AudioProcessorValueTreeState& apvts)
{
    auto f = [&apvts] (const juce::String& id) { return apvts.getRawParameterValue (id)->load(); };
    factory_core::ReductionNodes n;

    n.lowCut  = { f (cutPid (0, "on")) > 0.5f, (double) f (cutPid (0, "freq")), slopeValue ((int) f (cutPid (0, "slope"))) };
    n.highCut = { f (cutPid (1, "on")) > 0.5f, (double) f (cutPid (1, "freq")), slopeValue ((int) f (cutPid (1, "slope"))) };

    for (int b = 0; b < kNumBands; ++b)
        n.bands[(size_t) b] = { f (bandPid (b, "on")) > 0.5f,
                                (double) f (bandPid (b, "freq")),
                                (factory_core::ReductionBandType) (int) f (bandPid (b, "type")),
                                (double) f (bandPid (b, "sens")) };
    return n;
}

juce::AudioProcessorValueTreeState::ParameterLayout
ResonanceSuppressorAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "depth", 1 }, "Depth",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 30.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "sharpness", 1 }, "Sharpness",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    NormalisableRange<float> atkR { 1.0f, 200.0f }; atkR.setSkewForCentre (20.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack", atkR, 100.0f,
        AudioParameterFloatAttributes().withLabel (" ms")));

    NormalisableRange<float> relR { 5.0f, 500.0f }; relR.setSkewForCentre (100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release", relR, 50.0f,
        AudioParameterFloatAttributes().withLabel (" ms")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "delta", 1 },  "Delta",  false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "link", 1 },   "Stereo Link", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    // Detection mode. Soft (default): adaptive threshold, level-independent —
    // reacts to relative tonal change. Hard: absolute-level threshold (Depth sets
    // it), reacts to absolute harmonic level (Soothe2-style). Soft is the current
    // behaviour, so it is the default and existing presets are unchanged.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "mode", 1 }, "Mode", StringArray { "Soft", "Hard" }, 0));

    // --- Phase 1 detector controls (Selectivity / Tilt / Quality) ---
    // Version hint 2: these arrived in v1.3.0, after the original v1.x set, so a
    // v1.2.0 session (which lacks them) still loads — its state simply leaves them
    // at the defaults below (verified by preset_test's v1.2.0 fixture). Applied
    // every block in processBlock like depth/sharpness; the engine epsilon-compares
    // and rebuilds lazily, so no SmoothedValue is needed.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "selectivity", 2 }, "Selectivity",
        NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "tilt", 2 }, "Tilt",
        NormalisableRange<float> { -100.0f, 100.0f, 1.0f }, 0.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    // Quality trades latency for low-frequency time resolution (Fast = half
    // latency, High = double). Excluded from presets (FactoryPresets kExclude): a
    // preset switch must not renegotiate host PDC or override the user's choice.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "quality", 2 }, "Quality", StringArray { "Fast", "Normal", "High" }, 1)); // Normal

    // --- Phase 3 routing controls (Link Amount / Channel mode / Sidechain) ---
    // Version hint 2: added in v1.5.0, after the v1.2.0 set, so an older session still
    // loads and simply leaves these at the defaults below (guarded by preset_test's
    // v1.2.0 fixture). linkAmt scales the continuous stereo-link blend and is only
    // effective while the Stereo Link toggle is on (engine: lambda = link ? amt : 0);
    // channelMode switches Stereo vs Mid/Side; scEnable/scListen are additionally
    // gated on a live sidechain connection in processBlock, so an unpatched sidechain
    // is safe. Applied every block like the detector controls (no SmoothedValue: the
    // engine epsilon-compares and the routing switches ride their own crossfades).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "linkAmt", 2 }, "Link Amount",
        NormalisableRange<float> { 0.0f, 100.0f, 1.0f }, 100.0f,
        AudioParameterFloatAttributes().withLabel (" %")));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "channelMode", 2 }, "Channel Mode", StringArray { "Stereo", "Mid-Side" }, 0));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "scEnable", 2 }, "Sidechain", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "scListen", 2 }, "SC Listen", false));

    // --- Reduction / depth-EQ nodes (soothe-style) ---
    // Two cuts bound where processing acts (rolling the profile off at a chosen
    // slope), four typed bands locally raise/lower the sensitivity. Defaults
    // mirror the reference: low cut 450 Hz, high cut 16 kHz, bands flat except a
    // +6 dB emphasis at 5 kHz, so the factory sound is mid-focused, not full-band.
    const StringArray slopeChoices { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    const StringArray typeChoices  { "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" };

    auto freqRange = [] { NormalisableRange<float> r { 20.0f, 20000.0f }; r.setSkewForCentre (650.0f); return r; };

    struct CutDef  { const char* which; float freq; };
    for (auto [w, cd] : { std::pair<int, CutDef> { 0, { "Low Cut",  450.0f } },
                          std::pair<int, CutDef> { 1, { "High Cut", 16000.0f } } })
    {
        layout.add (std::make_unique<AudioParameterBool>   (ParameterID { cutPid (w, "on"),    1 }, juce::String (cd.which) + " On", true));
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { cutPid (w, "freq"),  1 }, juce::String (cd.which) + " Freq", freqRange(), cd.freq,
                                                            AudioParameterFloatAttributes().withLabel (" Hz")));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { cutPid (w, "slope"), 1 }, juce::String (cd.which) + " Slope", slopeChoices, 2)); // 24 dB/oct
    }

    const float bandFreqs[kNumBands] = { 1000.0f, 2500.0f, 5000.0f, 8000.0f };
    const float bandSens [kNumBands] = { 0.0f,   0.0f,    6.0f,    0.0f };
    for (int b = 0; b < kNumBands; ++b)
    {
        const juce::String name = "Band " + juce::String (b + 1);
        layout.add (std::make_unique<AudioParameterBool>   (ParameterID { bandPid (b, "on"),   1 }, name + " On", true));
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandPid (b, "freq"), 1 }, name + " Freq", freqRange(), bandFreqs[b],
                                                            AudioParameterFloatAttributes().withLabel (" Hz")));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandPid (b, "type"), 1 }, name + " Type", typeChoices, 0)); // Bell
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandPid (b, "sens"), 1 }, name + " Sens",
                                                            NormalisableRange<float> { -30.0f, 30.0f, 0.1f }, bandSens[b],
                                                            AudioParameterFloatAttributes().withLabel (" dB")));
    }

    return layout;
}

ResonanceSuppressorAudioProcessor::ResonanceSuppressorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)), // optional detector key
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    depthParam  = apvts.getRawParameterValue ("depth");
    sharpParam  = apvts.getRawParameterValue ("sharpness");
    atkParam    = apvts.getRawParameterValue ("attack");
    relParam    = apvts.getRawParameterValue ("release");
    mixParam    = apvts.getRawParameterValue ("mix");
    deltaParam  = apvts.getRawParameterValue ("delta");
    linkParam   = apvts.getRawParameterValue ("link");
    bypassParam = apvts.getRawParameterValue ("bypass");
    bypassParamPtr = apvts.getParameter ("bypass");
    modeParam   = apvts.getRawParameterValue ("mode");
    selParam    = apvts.getRawParameterValue ("selectivity");
    tiltParam   = apvts.getRawParameterValue ("tilt");
    qualityParam = apvts.getRawParameterValue ("quality");
    linkAmtParam     = apvts.getRawParameterValue ("linkAmt");
    channelModeParam = apvts.getRawParameterValue ("channelMode");
    scEnableParam    = apvts.getRawParameterValue ("scEnable");
    scListenParam    = apvts.getRawParameterValue ("scListen");

    for (int w = 0; w < 2; ++w)
    {
        auto& c = (w == 0) ? lowCut : highCut;
        c.on    = apvts.getRawParameterValue (cutPid (w, "on"));
        c.freq  = apvts.getRawParameterValue (cutPid (w, "freq"));
        c.slope = apvts.getRawParameterValue (cutPid (w, "slope"));
    }
    for (int b = 0; b < kNumBands; ++b)
    {
        bandParams[(size_t) b].on   = apvts.getRawParameterValue (bandPid (b, "on"));
        bandParams[(size_t) b].freq = apvts.getRawParameterValue (bandPid (b, "freq"));
        bandParams[(size_t) b].type = apvts.getRawParameterValue (bandPid (b, "type"));
        bandParams[(size_t) b].sens = apvts.getRawParameterValue (bandPid (b, "sens"));
    }

    programs.configure (apvts, resonance_suppressor_presets::bank,
                        resonance_suppressor_presets::kExclude,
                        resonance_suppressor_presets::kNumExclude);
}

factory_core::ReductionNodes ResonanceSuppressorAudioProcessor::currentNodes() const noexcept
{
    factory_core::ReductionNodes n;
    n.lowCut  = { lowCut.on->load()  > 0.5f, (double) lowCut.freq->load(),  slopeValue ((int) lowCut.slope->load()) };
    n.highCut = { highCut.on->load() > 0.5f, (double) highCut.freq->load(), slopeValue ((int) highCut.slope->load()) };
    for (int b = 0; b < kNumBands; ++b)
        n.bands[(size_t) b] = { bandParams[(size_t) b].on->load() > 0.5f,
                                (double) bandParams[(size_t) b].freq->load(),
                                (factory_core::ReductionBandType) (int) bandParams[(size_t) b].type->load(),
                                (double) bandParams[(size_t) b].sens->load() };
    return n;
}

void ResonanceSuppressorAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    // Normal-quality order for this rate; the engine allocates one order higher for
    // High quality (its maxOrder defaults to order + 1, capped at kMaxFftOrder).
    currentFftOrder   = factory_core::fftOrderForSampleRate (sampleRate, kBaseFftOrder, kRefSampleRate, kMaxFftOrder);
    suppressor.prepare (sampleRate, currentFftOrder);
    setLatencySamples (suppressor.latencySamples());
    // Normal quality is active after prepare; activeBins follows the engine's live
    // window and is renegotiated in processBlock whenever a Quality switch changes it.
    activeBins.store (suppressor.numBins(), std::memory_order_relaxed);
    for (auto& a : pubMag) a.store (-120.0f, std::memory_order_relaxed);
    for (auto& a : pubRed) a.store (0.0f, std::memory_order_relaxed);
}

bool ResonanceSuppressorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Main bus: mono or stereo, in == out (unchanged rule).
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;

    // Sidechain (input bus 1) is optional: allow it disabled, mono, or stereo. When
    // enabled it feeds the detectors; processBlock only keys off it once connected.
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled()
            && sc != juce::AudioChannelSet::mono()
            && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void ResonanceSuppressorAudioProcessor::rasterizeProfile()
{
    const auto nodes = currentNodes();

    // The dual-resolution engine keeps a separate reduction profile per band, so
    // rasterise the same nodes twice — once on each sub-engine's grid. binToHz()
    // reflects that engine's ACTIVE window (Quality changes N: Fast = order-1,
    // High = order+1), so a Quality switch re-bakes on the new grid on the block
    // AFTER it takes effect at a frame boundary — same lazy behaviour the single
    // engine had, now tracked independently per band (each engine switches at its
    // own hop). The low grid is also the display grid; the high grid runs two
    // orders shorter, so its bin count never exceeds the low grid's (kMaxBins).
    const int nLow = suppressor.numBins();
    profileBuf[0] = 1.0; // DC: nominal (the engine leaves the range gate to the profile)
    for (int k = 1; k < nLow; ++k)
        profileBuf[(size_t) k] = factory_core::reductionProfileLinearAt (suppressor.binToHz (k), nodes);

    const auto& high = suppressor.highEngine();
    const int nHigh = high.numBins();
    profileBufHigh[0] = 1.0; // DC: nominal (matches the low grid)
    for (int k = 1; k < nHigh; ++k)
        profileBufHigh[(size_t) k] = factory_core::reductionProfileLinearAt (high.binToHz (k), nodes);

    suppressor.setProfile (profileBuf.data(), nLow, profileBufHigh.data(), nHigh);
}

void ResonanceSuppressorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Sidechain input (bus 1): may be disabled, mono, or stereo. "Connected" means the
    // bus is enabled with channels; the routing params below are gated on it so an
    // unpatched sidechain falls back to internal detection (the engine gets false).
    auto scBus = getBusBuffer (buffer, true, 1);
    const int  scChannels  = scBus.getNumChannels();
    const bool scConnected = scChannels > 0;

    suppressor.setDepth       ((double) depthParam->load() / 100.0 * 1.5);
    suppressor.setSharpness   (0.15 + (double) sharpParam->load() / 100.0 * 0.85); // 0.15..1.0 octave
    suppressor.setSelectivity ((double) selParam->load() / 100.0);  // 0..100 % -> 0..1
    suppressor.setTilt        ((double) tiltParam->load() / 100.0); // -100..+100 % -> -1..+1
    suppressor.setRange       (20.0, 20000.0); // full band; the low/high cut nodes bound processing via the profile
    suppressor.setTimes       (atkParam->load(), relParam->load());
    suppressor.setMix         ((double) mixParam->load() / 100.0);
    suppressor.setDelta       (deltaParam->load() > 0.5f);
    suppressor.setStereoLink  (linkParam->load() > 0.5f);
    // Continuous link amount (only effective while Stereo Link is on) and channel mode.
    suppressor.setLinkAmount  ((double) linkAmtParam->load() / 100.0);
    suppressor.setChannelMode ((int) channelModeParam->load());
    // Sidechain routing gated on a live connection: with the bus unpatched the engine
    // receives false and safely keys detection off the main signal. SC Listen also
    // requires the sidechain to be enabled (you can only monitor what is routed).
    const bool scEnable = scEnableParam->load() > 0.5f;
    const bool scListen = scListenParam->load() > 0.5f;
    suppressor.setSidechain   (scEnable && scConnected);
    suppressor.setScListen    (scListen && scEnable && scConnected);
    suppressor.setMode        ((int) modeParam->load());
    // Quality is latched here; the engine applies the config + latency change at its
    // next frame boundary inside process() below, so PDC is renegotiated after the
    // sample loop (see the setLatencySamples call there).
    suppressor.setQuality     ((int) qualityParam->load());
    // Latency-preserving bypass: the engine runs every block (STFT ring, gains and
    // display stay live and PDC-aligned) and setBypassed only crossfades the output
    // toward the aligned dry. No early return, so the reported latency is honoured
    // in bypass (no PDC shift against other tracks) and the editor keeps updating.
    suppressor.setBypassed    (bypassParam->load() > 0.5f);
    rasterizeProfile();

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // Sidechain read pointers: mono duplicates to both channels; absent -> null, and
    // the loop then falls back to (l, r) so the 4-arg call matches the 2-arg one.
    const float* scL = scConnected ? scBus.getReadPointer (0) : nullptr;
    const float* scR = scConnected ? scBus.getReadPointer (scChannels > 1 ? 1 : 0) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        // Read the sidechain BEFORE writing the main output: the SC bus can alias the
        // main buffer, so sampling it first keeps detection keyed off the true input.
        const double sl = (scL != nullptr) ? (double) scL[i] : l;
        const double sr = (scR != nullptr) ? (double) scR[i] : r;
        suppressor.process (l, r, sl, sr);
        L[i] = (float) l;
        if (R != nullptr) R[i] = (float) r;
    }

    // A Quality switch (applied at a frame boundary in the loop above) changes the
    // engine's window length N, hence its reported latency and bin grid. Renegotiate
    // host PDC and the live bin count when it moves. JUCE 8 accepts setLatencySamples
    // from processBlock (it defers the restart), so this is audio-thread safe.
    const int lat = suppressor.latencySamples();
    if (lat != getLatencySamples())
    {
        setLatencySamples (lat);
        activeBins.store (suppressor.numBins(), std::memory_order_relaxed);
    }

    // Publish the latest display spectra for the editor, on the now-current bin grid
    // (a Quality switch this block may have grown/shrunk it). magScratch is
    // preallocated (sized for the top order) to keep processBlock allocation-free.
    const int bins = suppressor.numBins();
    const double* magDb = suppressor.magnitudeDb (magScratch.data());
    const double* redDb = suppressor.reductionDb (redScratch.data());
    for (int k = 0; k < bins; ++k)
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
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void ResonanceSuppressorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ResonanceSuppressorAudioProcessor();
}
