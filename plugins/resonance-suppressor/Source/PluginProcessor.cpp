#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DetailParam.h"
#include "Params.h"

#include "factory_params/juce/ApvtsAdapter.h"

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
    // Phase P1 (params-model migration): this plugin's parameters are declared
    // as a single DECLARATIVE table in Source/Params.h (a factory_params::ParamDesc
    // vector, JUCE-free), and the APVTS layout is GENERATED from it here by
    // factory_params::buildApvtsLayout(). The generator reproduces the former
    // hand-written juce::AudioParameter* objects bit-for-bit and in the same
    // host-visible order (verified by preset_test's "paramdesc parity" check). To
    // add or change a parameter, edit the table in Params.h -- do NOT hand-write
    // juce::AudioParameter* objects here. The load-bearing rationale that used to
    // live in this function (the v2.1 legacy note, the Phase 6 attack/release
    // defaults draft, the Detail macro description, the Quality/PDC note, the
    // Phase 3 routing note, and the reduction-node/band section comments) moved to
    // Params.h alongside the corresponding entries.
    return factory_params::buildApvtsLayout (resonance_suppressor_params::buildRsParams());
}

ResonanceSuppressorAudioProcessor::ResonanceSuppressorAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)), // optional detector key
      apvts (*this, &undoManager, "PARAMS", createParameterLayout())
{
    depthParam  = apvts.getRawParameterValue ("depth");
    detailParam = apvts.getRawParameterValue ("detail");
    outParam    = apvts.getRawParameterValue ("out");
    atkParam    = apvts.getRawParameterValue ("attack");
    relParam    = apvts.getRawParameterValue ("release");
    mixParam    = apvts.getRawParameterValue ("mix");
    deltaParam  = apvts.getRawParameterValue ("delta");
    linkParam   = apvts.getRawParameterValue ("link");
    bypassParam = apvts.getRawParameterValue ("bypass");
    bypassParamPtr = apvts.getParameter ("bypass");
    modeParam   = apvts.getRawParameterValue ("mode");
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
    // Output trim smoother (~20 ms, continuous-param rule): seed at the CURRENT
    // effective target -- unity while bypassed, the dB value otherwise -- so
    // playback never starts with a trim ramp.
    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (bypassParam->load() > 0.5f
                                          ? 1.0
                                          : std::pow (10.0, (double) outParam->load() / 20.0));
    // Display-only spectral smoothing (~50 ms): restores the v1 analyser's smooth
    // feel on top of v2's dual-resolution engine (whose airband window is only
    // ~10.7 ms). Opt-in and display-ONLY -- it low-passes what the analyser draws
    // and never touches the suppression/detection DSP or the output audio. Value
    // chosen by listening sign-off (2026-07-09).
    suppressor.setDisplaySmoothingMs (50.0);
    setLatencySamples (suppressor.latencySamples());
    // Normal quality is active after prepare; activeBins follows the engine's live
    // window and is renegotiated in processBlock whenever a Quality switch changes it.
    activeBins.store (suppressor.numBins(), std::memory_order_relaxed);
    for (auto& a : pubMag) a.store (-120.0f, std::memory_order_relaxed);
    for (auto& a : pubRed) a.store (0.0f, std::memory_order_relaxed);
    for (auto& a : pubMagPre) a.store (-120.0f, std::memory_order_relaxed);
    // Invalidate the profile-rasterisation cache: the engine's profile is all-1.0
    // after prepare, and the grid may have changed, so the first block must bake.
    profileCacheValid = false;
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

void ResonanceSuppressorAudioProcessor::updateProfile (int listen)
{
    // Rebake the reduction profile ONLY when its inputs changed since the last
    // block. The rasterised profile is a pure function of (listen node, reduction
    // nodes, both engines' bin grids); when none moved, the engine still holds the
    // profile from the previous block (setProfile copies, so it persists) and the
    // ~per-block transcendental sweep is skipped entirely. Comparing the actual
    // rasterise inputs (rather than trusting a set-hook) means the cache can never
    // go stale. Byte-identical to re-rasterising every block. The grid bin counts
    // capture Quality switches (they change the active window N, hence numBins);
    // a Quality switch takes effect at a frame boundary inside the sample loop, so
    // the new grid is picked up on the FOLLOWING block -- exactly the pre-cache
    // lazy behaviour.
    const auto nodes  = currentNodes();
    const int  nLow   = suppressor.numBins();
    const int  nHigh  = suppressor.highEngine().numBins();

    if (profileCacheValid
        && listen == cachedListenNode
        && nLow == cachedLowBins && nHigh == cachedHighBins
        && factory_core::reductionNodesIdentical (nodes, cachedProfileNodes))
        return;

    if (listen >= 0) rasterizeListenProfile (listen, nodes);
    else             rasterizeProfile (nodes);

    cachedListenNode  = listen;
    cachedLowBins     = nLow;
    cachedHighBins    = nHigh;
    cachedProfileNodes = nodes;
    profileCacheValid = true;
}

void ResonanceSuppressorAudioProcessor::rasterizeProfile (const factory_core::ReductionNodes& nodes)
{
    // The dual-resolution engine keeps a separate reduction profile per band, so
    // rasterise the same nodes twice — once on each sub-engine's grid. binToHz()
    // reflects that engine's ACTIVE window (Quality changes N: Fast = order-1,
    // High = order+1), so a Quality switch re-bakes on the new grid on the block
    // AFTER it takes effect at a frame boundary — same lazy behaviour the single
    // engine had, now tracked independently per band (each engine switches at its
    // own hop). The low grid is also the display grid; the high grid runs two
    // orders shorter, so its bin count never exceeds the low grid's (kMaxBins).
    // The per-band centre log-frequency is hoisted out of the bin sweep
    // (prepareBandLogF): loop-invariant, bit-identical to the per-call form.
    factory_core::BandLogF bandLogF;
    factory_core::prepareBandLogF (nodes, bandLogF);

    const int nLow = suppressor.numBins();
    profileBuf[0] = 1.0; // DC: nominal (the engine leaves the range gate to the profile)
    for (int k = 1; k < nLow; ++k)
        profileBuf[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (suppressor.binToHz (k), nodes, bandLogF);

    const auto& high = suppressor.highEngine();
    const int nHigh = high.numBins();
    profileBufHigh[0] = 1.0; // DC: nominal (matches the low grid)
    for (int k = 1; k < nHigh; ++k)
        profileBufHigh[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (high.binToHz (k), nodes, bandLogF);

    suppressor.setProfile (profileBuf.data(), nLow, profileBufHigh.data(), nHigh);
}

void ResonanceSuppressorAudioProcessor::rasterizeListenProfile (int nodeId, const factory_core::ReductionNodes& nodes)
{
    // Solo one reduction node: copy just that node from `nodes` into an otherwise-
    // default (all off) ReductionNodes, then rasterise it on BOTH engines' grids
    // exactly like rasterizeProfile() -- same mapping, same per-band width -- so
    // only that node's cut/band shapes the profile; every other bin sits at the
    // nominal (no-EQ) 1.0 baseline. id convention: 0 = low cut, 1 = high cut,
    // 2..(1+kNumBands) = bands (see setListenNode).
    factory_core::ReductionNodes solo;
    if (nodeId == 0)      solo.lowCut  = nodes.lowCut;
    else if (nodeId == 1) solo.highCut = nodes.highCut;
    else if (nodeId >= 2 && nodeId - 2 < kNumBands)
        solo.bands[(size_t) (nodeId - 2)] = nodes.bands[(size_t) (nodeId - 2)];

    factory_core::BandLogF bandLogF;
    factory_core::prepareBandLogF (solo, bandLogF);

    const int nLow = suppressor.numBins();
    listenProfileLow[0] = 1.0; // DC: nominal, matches rasterizeProfile()
    for (int k = 1; k < nLow; ++k)
        listenProfileLow[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (suppressor.binToHz (k), solo, bandLogF);

    const auto& high = suppressor.highEngine();
    const int nHigh = high.numBins();
    listenProfileHigh[0] = 1.0;
    for (int k = 1; k < nHigh; ++k)
        listenProfileHigh[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (high.binToHz (k), solo, bandLogF);

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

    // Editor-attached gate (perf): the analyser publish + display-time smoothing
    // are GUI-only. With no editor attached (plugin window closed), both are
    // skipped -- the DSP / output audio are untouched. Read once per block.
    const bool showDisplay = editorActive.load (std::memory_order_relaxed);

    // Analyzer DEV mode (P3b): live display-smoothing override. atomic load only
    // -- no alloc/lock/syscall. prepareToPlay still seeds the core at 50 ms; this
    // re-applies every block so the DEV panel can change it live. Forced to 0 (the
    // engine's cheap unsmoothed display path) while no editor is attached.
    suppressor.setDisplaySmoothingMs (showDisplay ? (double) displaySmoothMsUi.load (std::memory_order_relaxed) : 0.0);

    // Mode read ONCE per block: it steers both the Depth mapping (F2) and
    // setMode below, so the engine always sees a consistent mode+depth pair.
    const bool hardMode = ((int) modeParam->load()) == 1;
    // F2: Soft depth tops out at 1.0 (100 % = flatten-to-envelope, never below
    // at profile 1); Hard keeps the historical 1.5 span (Depth doubles as its
    // absolute-threshold sweep). Mapping lives in DetailParam.h.
    suppressor.setDepth       (resonance_suppressor_detail::engineDepthForPct ((double) depthParam->load(), hardMode));
    // v2.1 Detail macro drives all three detection shape controls (the legacy
    // sharpness/selectivity params are registered but no longer read); d = 50
    // reproduces the v2.0.1 defaults bit-exactly (DetailParam.h).
    const double detail = (double) detailParam->load();
    suppressor.setSharpness      (resonance_suppressor_detail::sharpOctForDetail (detail));
    suppressor.setSelectivity    (resonance_suppressor_detail::selectivityForDetail (detail));
    suppressor.setSmoothingWidth (resonance_suppressor_detail::gainSmoothOctForDetail (detail));
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
    suppressor.setMode        (hardMode ? 1 : 0); // same block-consistent read as the Depth mapping above
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
    // parameters (still applied above) keep working while soloed. updateProfile
    // rebakes only when the profile's inputs changed (perf) -- byte-identical to
    // rebuilding it every block, just skipping the redundant transcendental sweep
    // when nothing moved (the common steady-state case).
    updateProfile (listen);

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // Sidechain read pointers: mono duplicates to both channels; absent -> null, and
    // the loop then falls back to (l, r) so the 4-arg call matches the 2-arg one.
    const float* scL = scConnected ? scBus.getReadPointer (0) : nullptr;
    const float* scR = scConnected ? scBus.getReadPointer (scChannels > 1 ? 1 : 0) : nullptr;

    // Output trim (v2.1 "out"): applied AFTER the suppressor -- post Mix, and
    // in Delta too (it trims whatever the plugin emits). While the internal
    // bypass is active the target is unity so bypass stays a bit-transparent
    // passthrough once the ~20 ms ramp lands (matching the engine's own 10 ms
    // bypass crossfade in spirit; hard-switching would click mid-ramp). One
    // shared per-sample gain for both channels; no allocation or locks.
    outGain.setTargetValue (bypassParam->load() > 0.5f
                                ? 1.0
                                : std::pow (10.0, (double) outParam->load() / 20.0));

    for (int i = 0; i < numSamples; ++i)
    {
        double l = L[i];
        double r = (R != nullptr) ? R[i] : l;
        // Read the sidechain BEFORE writing the main output: the SC bus can alias the
        // main buffer, so sampling it first keeps detection keyed off the true input.
        const double sl = (scL != nullptr) ? (double) scL[i] : l;
        const double sr = (scR != nullptr) ? (double) scR[i] : r;
        suppressor.process (l, r, sl, sr);
        const double og = outGain.getNextValue();
        L[i] = (float) (l * og);
        if (R != nullptr) R[i] = (float) (r * og);
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
    // Skipped entirely when no editor is attached (perf): the three merged-dB reads
    // + per-bin atomic stores are pure display work nobody reads with the window
    // closed. Resumes on the first block after an editor attaches (the snapshots are
    // just one block stale then, imperceptible). PDC / activeBins above stays live.
    if (showDisplay)
    {
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
    auto xml = getXmlFromBinary (data, sizeInBytes);

    // v2.0.x -> v2.1 migration: a state saved before the "detail" parameter
    // existed (no PARAM child with id="detail") carries the legacy
    // sharpness/selectivity pair instead; inject the equivalent detail (their
    // mean, DetailParam.h) BEFORE applyStateXml so the old session reproduces
    // its old detection shape through the new macro. New-format states pass
    // through untouched, and everything else (including the "out" parameter,
    // absent from old states) keeps the exact pre-v2.1 applyStateXml
    // semantics -- the shared helper (ProgramAdapter.h) is deliberately not
    // forked. A/B slots are seeded in-session (always new-format), so
    // loadStateFromSlot needs no migration.
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        bool   hasDetail = false;
        double sharpPct = 50.0, selPct = 50.0; // layout defaults if absent
        for (auto* e : xml->getChildIterator())
        {
            if (! e->hasTagName ("PARAM")) continue;
            const auto id = e->getStringAttribute ("id");
            if      (id == "detail")      hasDetail = true;
            else if (id == "sharpness")   sharpPct = e->getDoubleAttribute ("value", 50.0);
            else if (id == "selectivity") selPct   = e->getDoubleAttribute ("value", 50.0);
        }
        if (! hasDetail)
        {
            auto* p = xml->createNewChildElement ("PARAM");
            p->setAttribute ("id", "detail");
            p->setAttribute ("value", resonance_suppressor_detail::detailFromLegacy (sharpPct, selPct));
        }
    }

    factory_presets::applyStateXml (apvts, programs, xml.get());
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
