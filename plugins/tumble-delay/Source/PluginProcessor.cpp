#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <iterator>

namespace
{
    // Tempo-sync choice labels and their note values (beats; a quarter note = 1
    // beat), resolved to seconds in processBlock via factory_core::
    // tempoSyncSeconds. Index 0 of each choice list is "Off" (the free value
    // wins), so the beats tables are indexed by (choiceIndex - 1). Labels and
    // beats live side by side and the static_asserts pin their alignment
    // (state-compatibility-critical: reordering one without the other would
    // silently remap every saved session's sync choice).
    constexpr const char* kBoxSizeSyncChoices[] = { "Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars" };
    constexpr double      kBoxSizeSyncBeats[]   = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };

    constexpr const char* kSpinSyncChoices[] = { "Off", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars" };
    constexpr double      kSpinSyncBeats[]   = { 1.0, 2.0, 4.0, 8.0, 16.0 };   // per revolution

    constexpr const char* kPreDelaySyncChoices[] = { "Off", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2" };
    constexpr double      kPreDelaySyncBeats[]   = { 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0 };

    constexpr const char* kTimeSyncChoices[] = { "Off", "1/32", "1/16", "1/16T", "1/8", "1/8T", "1/8.",
                                                 "1/4", "1/4T", "1/4.", "1/2", "1 Bar" };
    constexpr double      kTimeSyncBeats[]   = { 0.125, 0.25, 0.25 * 2.0 / 3.0, 0.5,
                                                 0.5 * 2.0 / 3.0, 0.5 * 1.5, 1.0,
                                                 1.0 * 2.0 / 3.0, 1.0 * 1.5, 2.0, 4.0 };

    static_assert (std::size (kBoxSizeSyncChoices)  == std::size (kBoxSizeSyncBeats) + 1);
    static_assert (std::size (kSpinSyncChoices)     == std::size (kSpinSyncBeats) + 1);
    static_assert (std::size (kPreDelaySyncChoices) == std::size (kPreDelaySyncBeats) + 1);
    static_assert (std::size (kTimeSyncChoices)     == std::size (kTimeSyncBeats) + 1);

    template <std::size_t N>
    juce::StringArray choiceList (const char* const (& items)[N])
    {
        return juce::StringArray (items, (int) N);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
TumbleDelayAudioProcessor::createParameterLayout()
{
    using SA = juce::StringArray;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // "log" ranges: geometric-mean centre skew (spec §5). Linear ranges are plain.
    auto logRange = [] (float lo, float hi, float step)
    {
        juce::NormalisableRange<float> r { lo, hi, step };
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    };
    auto linRange = [] (float lo, float hi, float step)
    {
        return juce::NormalisableRange<float> { lo, hi, step };
    };
    auto addF = [&layout] (const juce::String& id, const juce::String& name,
                           juce::NormalisableRange<float> range, float def,
                           const juce::String& label)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, range, def,
            juce::AudioParameterFloatAttributes().withLabel (label)));
    };
    auto addC = [&layout] (const juce::String& id, const juce::String& name,
                           SA choices, int def)
    {
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { id, 1 }, name, choices, def));
    };
    auto addB = [&layout] (const juce::String& id, const juce::String& name, bool def)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { id, 1 }, name, def));
    };
    auto addI = [&layout] (const juce::String& id, const juce::String& name,
                           int lo, int hi, int def)
    {
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { id, 1 }, name, lo, hi, def));
    };

    // ---- Output + bypass: kept EXACTLY as the scaffold defines them. ----
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    // ---- Globals (15) ----
    addC ("boxShape", "Shape",
          SA { "Triangle", "Square", "Pentagon", "Hexagon", "Octagon", "Circle" }, 1);
    addF ("boxSize", "Box Size", logRange (0.05f, 4.0f, 0.001f), 0.40f, " s");
    addC ("boxSizeSync", "Size Sync", choiceList (kBoxSizeSyncChoices), 0);
    addF ("spin", "Spin", linRange (-2.0f, 2.0f, 0.01f), 0.20f, " rev/s");
    addC ("spinSync", "Spin Sync", choiceList (kSpinSyncChoices), 0);
    addF ("pivotX", "Pivot X", linRange (-1.0f, 1.0f, 0.01f), 0.0f, "");
    addF ("pivotY", "Pivot Y", linRange (-1.0f, 1.0f, 0.01f), 0.0f, "");
    addF ("gravity", "Gravity", linRange (0.0f, 100.0f, 0.1f), 0.0f, " %");
    addB ("ballCollide", "Ball Collide", false);
    addF ("sense", "Sense", linRange (-60.0f, 0.0f, 0.1f), -30.0f, " dB");
    addF ("retrig", "Retrig", logRange (20.0f, 2000.0f, 1.0f), 150.0f, " ms");
    addF ("spawnSpread", "Spawn Spread", linRange (0.0f, 100.0f, 0.1f), 10.0f, " %");
    addF ("refeed", "Refeed", linRange (0.0f, 95.0f, 0.1f), 0.0f, " %");
    addF ("tone", "Tone", logRange (500.0f, 20000.0f, 1.0f), 12000.0f, " Hz");
    addF ("mix", "Mix", linRange (0.0f, 100.0f, 0.1f), 35.0f, " %");

    // ---- Slots A–D (25 each = 100) ----
    static constexpr const char* kSlotPrefix[4]  = { "a", "b", "c", "d" };
    static constexpr const char* kSlotDisplay[4] = { "A ", "B ", "C ", "D " };
    const juce::String deg (juce::CharPointer_UTF8 ("\xc2\xb0")); // degree sign

    for (int s = 0; s < 4; ++s)
    {
        const juce::String p = kSlotPrefix[s];
        const juce::String d = kSlotDisplay[s];
        const bool defOn = (s == 0); // A on, B–D off

        addB (p + "On",           d + "Enable", defOn);
        addI (p + "Count",        d + "Balls", 1, factory_core::TumbleDelay::kMaxBallsPerSlot, 1);
        addF (p + "BallSize",     d + "Ball Size", linRange (2.0f, 25.0f, 0.1f), 8.0f, " %");
        addF (p + "Speed",        d + "Speed", logRange (0.25f, 4.0f, 0.01f), 1.0f, " x");
        addF (p + "Direction",    d + "Direction", linRange (0.0f, 360.0f, 0.1f), 90.0f, deg);
        addF (p + "DirRandom",    d + "Dir Random", linRange (0.0f, 100.0f, 0.1f), 100.0f, " %");
        addF (p + "PreDelay",     d + "Pre-Delay", linRange (0.0f, 1000.0f, 0.1f), 0.0f, " ms");
        addC (p + "PreDelaySync", d + "PD Sync", choiceList (kPreDelaySyncChoices), 0);
        addF (p + "Time",         d + "Time", logRange (10.0f, 2000.0f, 0.1f), 350.0f, " ms");
        addC (p + "TimeSync",     d + "Time Sync", choiceList (kTimeSyncChoices), 0);
        addF (p + "Bounce",       d + "Bounce", linRange (20.0f, 100.0f, 0.1f), 70.0f, " %");
        addF (p + "Drag",         d + "Drag", linRange (0.0f, 100.0f, 0.1f), 10.0f, " %");
        addF (p + "DecayCurve",   d + "Curve", linRange (-100.0f, 100.0f, 0.1f), 0.0f, "");
        addC (p + "LifeMode",     d + "Life Mode", SA { "Time", "Bounces" }, 0);
        addF (p + "LifeTime",     d + "Life Time", logRange (0.1f, 16.0f, 0.01f), 3.0f, " s");
        addI (p + "LifeBounces",  d + "Life Bounces", 1, 99, 12);
        addF (p + "Pitch",        d + "Pitch", linRange (-24.0f, 24.0f, 0.01f), 0.0f, " st");
        addF (p + "PitchRand",    d + "Pitch Rand", linRange (0.0f, 12.0f, 0.01f), 0.0f, " st");
        addF (p + "Grain",        d + "Grain", logRange (10.0f, 500.0f, 0.1f), 90.0f, " ms");
        addF (p + "Reverse",      d + "Reverse", linRange (0.0f, 100.0f, 0.1f), 0.0f, " %");
        addF (p + "Motion",       d + "Motion", linRange (-100.0f, 100.0f, 0.1f), 0.0f, " %");
        addF (p + "Step",         d + "Step", linRange (-500.0f, 500.0f, 0.1f), 0.0f, " ms");
        addF (p + "Spray",        d + "Spray", linRange (0.0f, 500.0f, 0.1f), 0.0f, " ms");
        addC (p + "PanMode",      d + "Pan Mode", SA { "Physics", "Center", "Random" }, 0);
        addF (p + "Gain",         d + "Slot Gain", linRange (-24.0f, 6.0f, 0.01f), 0.0f, " dB");
    }

    return layout;
}

TumbleDelayAudioProcessor::TumbleDelayAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };

    outputParam = raw ("output");
    bypassParam = raw ("bypass");

    boxShapeParam    = raw ("boxShape");
    boxSizeParam     = raw ("boxSize");
    boxSizeSyncParam = raw ("boxSizeSync");
    spinParam        = raw ("spin");
    spinSyncParam    = raw ("spinSync");
    pivotXParam      = raw ("pivotX");
    pivotYParam      = raw ("pivotY");
    gravityParam     = raw ("gravity");
    ballCollideParam = raw ("ballCollide");
    senseParam       = raw ("sense");
    retrigParam      = raw ("retrig");
    spawnSpreadParam = raw ("spawnSpread");
    refeedParam      = raw ("refeed");
    toneParam        = raw ("tone");
    mixParam         = raw ("mix");

    // Slot ID builder — used only here (ctor allocation is fine).
    static constexpr const char* kSlotPrefix[4] = { "a", "b", "c", "d" };
    for (int s = 0; s < 4; ++s)
    {
        const juce::String p = kSlotPrefix[s];
        SlotRefs& ref = slotRefs[(size_t) s];
        ref.on           = raw (p + "On");
        ref.count        = raw (p + "Count");
        ref.ballSize     = raw (p + "BallSize");
        ref.speed        = raw (p + "Speed");
        ref.direction    = raw (p + "Direction");
        ref.dirRandom    = raw (p + "DirRandom");
        ref.preDelay     = raw (p + "PreDelay");
        ref.preDelaySync = raw (p + "PreDelaySync");
        ref.time         = raw (p + "Time");
        ref.timeSync     = raw (p + "TimeSync");
        ref.bounce       = raw (p + "Bounce");
        ref.drag         = raw (p + "Drag");
        ref.decayCurve   = raw (p + "DecayCurve");
        ref.lifeMode     = raw (p + "LifeMode");
        ref.lifeTime     = raw (p + "LifeTime");
        ref.lifeBounces  = raw (p + "LifeBounces");
        ref.pitch        = raw (p + "Pitch");
        ref.pitchRand    = raw (p + "PitchRand");
        ref.grain        = raw (p + "Grain");
        ref.reverse      = raw (p + "Reverse");
        ref.motion       = raw (p + "Motion");
        ref.step         = raw (p + "Step");
        ref.spray        = raw (p + "Spray");
        ref.panMode      = raw (p + "PanMode");
        ref.gain         = raw (p + "Gain");
    }

    programs.configure (apvts, tumble_delay_presets::bank,
                        tumble_delay_presets::kExclude, tumble_delay_presets::kNumExclude);
}

void TumbleDelayAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    engine.prepare (sampleRate); // allocates the ring + resets all state

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));

    wasBypassed = (bypassParam->load() > 0.5f);

    // Push the current parameter state so a host querying getTailLengthSeconds()
    // before the first (non-bypassed) block sees the real tail instead of the
    // engine's all-disabled defaults. getPlayHead() is not valid outside
    // processBlock, so 120 BPM stands in until the first block re-resolves sync.
    pushParametersToEngine (120.0);
}

bool TumbleDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void TumbleDelayAudioProcessor::pushParametersToEngine (double bpm) noexcept
{
    using Engine  = factory_core::TumbleDelay;
    using Shape   = Engine::Shape;
    using PanMode = Engine::PanMode;

    // ---------- Globals ----------
    engine.setBoxShape (static_cast<Shape> (juce::roundToInt (boxShapeParam->load())));

    // Box Size: a non-Off sync value (note value = box traversal time) replaces the
    // free seconds value. The engine clamps to [0.02, 8] s defensively.
    {
        const int sync = juce::roundToInt (boxSizeSyncParam->load());
        const double sec = (sync > 0)
            ? factory_core::tempoSyncSeconds (bpm, kBoxSizeSyncBeats[sync - 1])
            : (double) boxSizeParam->load();
        engine.setBoxSizeSeconds (sec);
        effBoxSizeSec.store (sec, std::memory_order_relaxed);
    }

    // Spin: a non-Off sync value is seconds-per-revolution -> rev/s; its sign is
    // taken from the free Spin knob (spin == 0 -> positive).
    {
        const int sync = juce::roundToInt (spinSyncParam->load());
        double rps;
        if (sync > 0)
        {
            const double secPerRev = factory_core::tempoSyncSeconds (bpm, kSpinSyncBeats[sync - 1]);
            const double mag = (secPerRev > 0.0) ? 1.0 / secPerRev : 0.0;
            rps = (spinParam->load() < 0.0f) ? -mag : mag;
        }
        else
        {
            rps = (double) spinParam->load();
        }
        engine.setSpinRevPerSec (rps);
    }

    engine.setPivot       ((double) pivotXParam->load(), (double) pivotYParam->load());
    engine.setGravity     ((double) gravityParam->load()     / 100.0);
    engine.setBallCollide (ballCollideParam->load() > 0.5f);
    engine.setSenseDb     ((double) senseParam->load());
    engine.setRetrigMs    ((double) retrigParam->load());
    engine.setSpawnSpread ((double) spawnSpreadParam->load() / 100.0);
    engine.setRefeed      ((double) refeedParam->load()      / 100.0);
    engine.setToneHz      ((double) toneParam->load());
    engine.setMix         ((double) mixParam->load()         / 100.0);

    // ---------- Slots A–D ----------
    for (int s = 0; s < Engine::kNumSlots; ++s)
    {
        const SlotRefs& ref = slotRefs[(size_t) s];
        Engine::SlotParams sp;

        sp.enabled      = ref.on->load() > 0.5f;
        sp.count        = juce::roundToInt (ref.count->load());
        sp.ballSize     = (double) ref.ballSize->load() / 100.0;
        sp.speed        = (double) ref.speed->load();
        sp.directionDeg = (double) ref.direction->load();
        sp.dirRandom    = (double) ref.dirRandom->load() / 100.0;

        // Pre-Delay: sync overrides free; wrapper clamps <= 1 s (spec §4.9).
        {
            const int sync = juce::roundToInt (ref.preDelaySync->load());
            sp.preDelaySeconds = (sync > 0)
                ? juce::jmin (1.0, factory_core::tempoSyncSeconds (bpm, kPreDelaySyncBeats[sync - 1]))
                : (double) ref.preDelay->load() / 1000.0;
        }
        // Time: sync overrides free; wrapper clamps <= 2 s (spec §4.9).
        {
            const int sync = juce::roundToInt (ref.timeSync->load());
            sp.timeSeconds = (sync > 0)
                ? juce::jmin (2.0, factory_core::tempoSyncSeconds (bpm, kTimeSyncBeats[sync - 1]))
                : (double) ref.time->load() / 1000.0;
        }

        sp.bounce          = (double) ref.bounce->load()     / 100.0;
        sp.drag            = (double) ref.drag->load()       / 100.0;
        sp.decayCurve      = (double) ref.decayCurve->load() / 100.0;
        sp.lifeIsBounces   = (juce::roundToInt (ref.lifeMode->load()) == 1);
        sp.lifeTimeSeconds = (double) ref.lifeTime->load();
        sp.lifeBounces     = juce::roundToInt (ref.lifeBounces->load());
        sp.pitchSemis      = (double) ref.pitch->load();
        sp.pitchRandSemis  = (double) ref.pitchRand->load();
        sp.grainMs         = (double) ref.grain->load();
        sp.reverseProb     = (double) ref.reverse->load() / 100.0;
        sp.motion          = (double) ref.motion->load()  / 100.0;
        sp.stepSeconds     = (double) ref.step->load()    / 1000.0;
        sp.sprayMs         = (double) ref.spray->load();
        sp.panMode         = static_cast<PanMode> (juce::roundToInt (ref.panMode->load()));
        sp.gainLinear      = juce::Decibels::decibelsToGain ((double) ref.gain->load());

        engine.setSlotParams (s, sp);
    }
}

void TumbleDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;

    // Reset the engine on either bypass transition (regression class E).
    if (bypassed != wasBypassed)
    {
        engine.reset();
        wasBypassed = bypassed;
    }

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    const int numCh = juce::jmin (totalIn, totalOut);
    const int n     = buffer.getNumSamples();

    if (bypassed)
    {
        // Dry passthrough — skip the engine entirely; still ramp the output gain.
        float* chp[2] = { numCh >= 1 ? buffer.getWritePointer (0) : nullptr,
                          numCh >= 2 ? buffer.getWritePointer (1) : nullptr };
        for (int i = 0; i < n; ++i)
        {
            const float g = outputGain.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                chp[ch][i] *= g;
        }
        return;
    }

    // Resolve host tempo (optionals, no allocation). Fallback 120 BPM.
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                if (*b > 0.0)
                    bpm = *b;

    // Read every parameter once at block head and push it to the engine.
    pushParametersToEngine (bpm);

    if (numCh >= 2)
    {
        float* ch0 = buffer.getWritePointer (0);
        float* ch1 = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            double l = ch0[i];
            double r = ch1[i];
            engine.processStereo (l, r);
            const float g = outputGain.getNextValue();
            ch0[i] = (float) l * g;
            ch1[i] = (float) r * g;
        }
    }
    else if (numCh == 1)
    {
        // Mono bus: duplicate the input into l/r, output the average.
        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            double l = ch0[i];
            double r = ch0[i];
            engine.processStereo (l, r);
            const float g = outputGain.getNextValue();
            ch0[i] = (float) (0.5 * (l + r)) * g;
        }
    }
}

juce::AudioProcessorEditor* TumbleDelayAudioProcessor::createEditor()
{
    return new TumbleDelayAudioProcessorEditor (*this);
}

void TumbleDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // stateToXml appends the selected program index (append-only attribute — old
    // sessions without it read back as program 0). copyXmlToBinary is a protected
    // AudioProcessor static, so it must stay here in the member function.
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void TumbleDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TumbleDelayAudioProcessor();
}
