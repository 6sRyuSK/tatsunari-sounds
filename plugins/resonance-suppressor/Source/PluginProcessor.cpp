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
                                (double) f (bandPid (b, "sens")),
                                (double) f (bandPid (b, "width")) };
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

    // Phase 6 DEFAULTS DRAFT (pending audition sign-off, CLAUDE.md "Ask a human"
    // #1): attack was 100 ms, tuned for the pre-Phase-1 detector (a coarser,
    // slower-reacting envelope that needed a sluggish attack to avoid chatter).
    // The Phase 1 rework detects per STFT frame (H/fs ~ 5.3 ms hop @ 48 kHz
    // Normal, 8x overlap) with a self-excluding-notch envelope + soft-knee
    // contrast, so it is precise enough per frame that a 100 ms attack just
    // lets a transient resonance (a harsh consonant, a pick attack) ring for
    // ~19 frames before the suppressor catches up -- audibly late for a
    // de-harsh tool. New default 20 ms (the skew centre of this range, so it
    // sits at the dial's natural middle) reacts within ~4 frames while still
    // averaging over enough frames to reject single-frame noise-floor jitter.
    NormalisableRange<float> atkR { 1.0f, 200.0f }; atkR.setSkewForCentre (20.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack", atkR, 20.0f,
        AudioParameterFloatAttributes().withLabel (" ms")));

    // Release nudged 50 -> 65 ms alongside the faster attack: a snappier attack
    // with an unchanged release skewed the ballistics' overall shape toward
    // "grabs fast, lets go fast", which can pump on rhythmic material; a modest
    // release increase keeps recovery still well inside a "fast" setting
    // (release range tops out at 500 ms) while smoothing the gesture back out.
    NormalisableRange<float> relR { 5.0f, 500.0f }; relR.setSkewForCentre (100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release", relR, 65.0f,
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
    // Phase 6 DEFAULTS DRAFT: selectivity/depth/sharpness reviewed and left
    // UNCHANGED (conservative) -- 50% is already the soft-knee law's own
    // documented "nominal" point (ResonanceSuppressor::computeGains: T=3.5dB/
    // W=4dB), and depth/sharpness are audition-first-impression choices better
    // judged by ear against the Phase 6 pack than re-guessed here.
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
    // slope), eight typed bands locally raise/lower the sensitivity over a
    // per-band width (Phase 4). Defaults mirror the reference: low cut 450 Hz,
    // high cut 16 kHz, bands 1-4 flat except a +6 dB emphasis at 5 kHz, so the
    // factory sound is mid-focused, not full-band; bands 5-8 (Phase 4) are off
    // by default so the shipped sound is unchanged until the user enables one.
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

    // b0..b3 shipped pre-Phase-4 (version hint 1) -- defaults/hints unchanged.
    const float bandFreqs[kNumBands] = { 1000.0f, 2500.0f, 5000.0f, 8000.0f, 150.0f, 500.0f, 3000.0f, 12000.0f };
    const float bandSens [kNumBands] = { 0.0f,   0.0f,    6.0f,    0.0f,    0.0f,   0.0f,   0.0f,    0.0f };
    constexpr int kNumBandsV1 = 4;
    for (int b = 0; b < kNumBandsV1; ++b)
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

    // Phase 4: bands 5-8, off by default (version hint 2 -- brand new IDs, never
    // shipped before). Defaults spread across low/low-mid/high-mid/air so
    // enabling one lands somewhere useful before the user retunes freq/type/sens.
    for (int b = kNumBandsV1; b < kNumBands; ++b)
    {
        const juce::String name = "Band " + juce::String (b + 1);
        layout.add (std::make_unique<AudioParameterBool>   (ParameterID { bandPid (b, "on"),   2 }, name + " On", false));
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandPid (b, "freq"), 2 }, name + " Freq", freqRange(), bandFreqs[b],
                                                            AudioParameterFloatAttributes().withLabel (" Hz")));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandPid (b, "type"), 2 }, name + " Type", typeChoices, 0)); // Bell
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandPid (b, "sens"), 2 }, name + " Sens",
                                                            NormalisableRange<float> { -30.0f, 30.0f, 0.1f }, bandSens[b],
                                                            AudioParameterFloatAttributes().withLabel (" dB")));
    }

    // Phase 4: per-band width, ALL 8 bands (version hint 2 -- brand new). Scales
    // each shape's half-width/edge/span in ReductionProfile.h; default 0.50 is
    // the pre-Phase-4 fixed width, so every band reproduces the old curve
    // bit-for-bit until this is moved (see ReductionProfile.h's kWidthRef). The
    // UI knob for this parameter is Phase 5a; only the parameter ships here.
    for (int b = 0; b < kNumBands; ++b)
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { bandPid (b, "width"), 2 }, "Band " + juce::String (b + 1) + " Width",
            NormalisableRange<float> { 0.10f, 2.00f, 0.01f }, 0.50f,
            AudioParameterFloatAttributes().withLabel (" oct")));

    return layout;
}

ResonanceSuppressorAudioProcessor::ResonanceSuppressorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)), // optional detector key
      apvts (*this, &undoManager, "PARAMS", createParameterLayout())
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
        bandParams[(size_t) b].on    = apvts.getRawParameterValue (bandPid (b, "on"));
        bandParams[(size_t) b].freq  = apvts.getRawParameterValue (bandPid (b, "freq"));
        bandParams[(size_t) b].type  = apvts.getRawParameterValue (bandPid (b, "type"));
        bandParams[(size_t) b].sens  = apvts.getRawParameterValue (bandPid (b, "sens"));
        bandParams[(size_t) b].width = apvts.getRawParameterValue (bandPid (b, "width"));
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
                                (double) bandParams[(size_t) b].sens->load(),
                                (double) bandParams[(size_t) b].width->load() };
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
    for (auto& a : pubMagPre) a.store (-120.0f, std::memory_order_relaxed);
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

void ResonanceSuppressorAudioProcessor::rasterizeListenProfile (int nodeId)
{
    // Solo one reduction node: copy just that node from the live parameters
    // into an otherwise-default (all off) ReductionNodes, then rasterise it on
    // BOTH engines' grids exactly like rasterizeProfile() -- same mapping, same
    // per-band width -- so only that node's cut/band shapes the profile; every
    // other bin sits at the nominal (no-EQ) 1.0 baseline. id convention: 0 = low
    // cut, 1 = high cut, 2..(1+kNumBands) = bands (see setListenNode).
    const auto nodes = currentNodes();
    factory_core::ReductionNodes solo;
    if (nodeId == 0)      solo.lowCut  = nodes.lowCut;
    else if (nodeId == 1) solo.highCut = nodes.highCut;
    else if (nodeId >= 2 && nodeId - 2 < kNumBands)
        solo.bands[(size_t) (nodeId - 2)] = nodes.bands[(size_t) (nodeId - 2)];

    const int nLow = suppressor.numBins();
    listenProfileLow[0] = 1.0; // DC: nominal, matches rasterizeProfile()
    for (int k = 1; k < nLow; ++k)
        listenProfileLow[(size_t) k] = factory_core::reductionProfileLinearAt (suppressor.binToHz (k), solo);

    const auto& high = suppressor.highEngine();
    const int nHigh = high.numBins();
    listenProfileHigh[0] = 1.0;
    for (int k = 1; k < nHigh; ++k)
        listenProfileHigh[(size_t) k] = factory_core::reductionProfileLinearAt (high.binToHz (k), solo);

    suppressor.setProfile (listenProfileLow.data(), nLow, listenProfileHigh.data(), nHigh);
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

    // Listen (5a-1-B): loaded once per block. >= 0 solos that one reduction
    // node's removed signal (forces delta/mix for THIS block only, below --
    // the APVTS delta/mix params themselves are never written) and outranks SC
    // Listen. < 0 is the normal path, unchanged.
    const int listen = listenNode.load (std::memory_order_relaxed);

    suppressor.setDepth       ((double) depthParam->load() / 100.0 * 1.5);
    suppressor.setSharpness   (0.15 + (double) sharpParam->load() / 100.0 * 0.85); // 0.15..1.0 octave
    suppressor.setSelectivity ((double) selParam->load() / 100.0);  // 0..100 % -> 0..1
    suppressor.setTilt        ((double) tiltParam->load() / 100.0); // -100..+100 % -> -1..+1
    suppressor.setRange       (20.0, 20000.0); // full band; the low/high cut nodes bound processing via the profile
    suppressor.setTimes       (atkParam->load(), relParam->load());
    // Mix/Delta: Listen forces mix=1.0 / delta=true (the node's removed signal,
    // full amount, mix-independent) for this block only; otherwise the normal
    // APVTS-driven values.
    suppressor.setMix         ((listen >= 0) ? 1.0 : (double) mixParam->load() / 100.0);
    suppressor.setDelta       ((listen >= 0) || (deltaParam->load() > 0.5f));
    suppressor.setStereoLink  (linkParam->load() > 0.5f);
    // Continuous link amount (only effective while Stereo Link is on) and channel mode.
    suppressor.setLinkAmount  ((double) linkAmtParam->load() / 100.0);
    suppressor.setChannelMode ((int) channelModeParam->load());
    // Sidechain routing gated on a live connection: with the bus unpatched the engine
    // receives false and safely keys detection off the main signal. SC Listen also
    // requires the sidechain to be enabled (you can only monitor what is routed), and
    // is forced off while Listen is active (Listen outranks SC Listen / Delta).
    const bool scEnable = scEnableParam->load() > 0.5f;
    const bool scListen = (listen < 0) && (scListenParam->load() > 0.5f);
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
    // Listen rasterises a single-node profile instead of the full node graph;
    // parameters (still applied above) keep working while soloed, since the
    // profile is rebuilt from the live parameters every block.
    if (listen >= 0) rasterizeListenProfile (listen);
    else              rasterizeProfile();

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
    const double* magDb    = suppressor.magnitudeDb (magScratch.data());
    const double* redDb    = suppressor.reductionDb (redScratch.data());
    const double* magPreDb = suppressor.magnitudePreDb (preScratch.data()); // 5a-1-C: input spectrum
    for (int k = 0; k < bins; ++k)
    {
        pubMag[(size_t) k].store ((float) magDb[k], std::memory_order_relaxed);
        pubRed[(size_t) k].store ((float) redDb[k], std::memory_order_relaxed);
        pubMagPre[(size_t) k].store ((float) magPreDb[k], std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* ResonanceSuppressorAudioProcessor::createEditor()
{
    return new ResonanceSuppressorAudioProcessorEditor (*this);
}

// Host persistence: intentionally always the CURRENT live state (APVTS +
// program index), same as before A/B existed. A/B slots are a session-only
// scratchpad (see setABSlot below) and never reach this blob -- switching A/B
// does not change what a host save/reload restores.
void ResonanceSuppressorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void ResonanceSuppressorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

// --- A/B compare (Phase 5b-1) ---------------------------------------------
// Same scope as getStateInformation (APVTS + program index via
// stateToXml/applyStateXml) -- listenNode and other transient execution state
// are deliberately excluded, matching the host-save scope exactly.

void ResonanceSuppressorAudioProcessor::copyStateToSlot (int slot)
{
    jassert (slot == 0 || slot == 1);
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, abState[(size_t) slot]);
}

void ResonanceSuppressorAudioProcessor::loadStateFromSlot (int slot)
{
    jassert (slot == 0 || slot == 1);
    if (abState[(size_t) slot].getSize() == 0)
        return; // never written (shouldn't happen -- setABSlot seeds it on first visit)
    factory_presets::applyStateXml (apvts, programs,
        getXmlFromBinary (abState[(size_t) slot].getData(), (int) abState[(size_t) slot].getSize()).get());
    // applyStateXml restored the slot's program index (programs.readStateAttribute)
    // without any program-change notification, so fire one -- the same idiom
    // PresetSelectorController uses after a user pick (setCurrentProgram +
    // updateHostDisplay withProgramChanged). Without it the editor's preset
    // combo and the DAW's program display keep showing the OLD slot's preset
    // name after an A/B switch. The controller marshals its refresh through
    // callAsync, so no synchronous re-entrancy; message-thread only (see the
    // A/B API comment in the header), so no RT impact.
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}.withProgramChanged (true));
}

void ResonanceSuppressorAudioProcessor::setABSlot (int slot)
{
    jassert (slot == 0 || slot == 1);
    if (slot == abActive)
        return; // re-picking the already-active slot is a no-op -- notably it must NOT
                // round-trip through loadStateFromSlot, which (via applyStateXml's
                // apvts.replaceState) clears undo history; a reselect of the current
                // slot is not a state change and must not have that side effect.
    copyStateToSlot (abActive); // stash the state of the slot we are leaving
    abActive = slot;
    if (abState[(size_t) slot].getSize() > 0)
        loadStateFromSlot (slot);
    else
        copyStateToSlot (slot); // first visit to this slot: seed it with the current (still-live) state
}

void ResonanceSuppressorAudioProcessor::copyActiveToOther()
{
    copyStateToSlot (1 - abActive); // the active slot's live state onto the other slot's storage
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ResonanceSuppressorAudioProcessor();
}
