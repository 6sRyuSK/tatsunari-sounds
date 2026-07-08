//
// preset_test.cpp — wiring verification for the resonance suppressor's factory presets.
//
// Preset typos (a paramID that no parameter owns) and out-of-range values fail
// silently at runtime, so this JUCE-linked console test constructs the processor
// headless and checks the table against the live APVTS layout. The independent
// oracle is the parameter layout declaration itself: a value that has to be
// clamped to apply, or an ID with no matching parameter, is a bug.
//
// This is the "wiring test" category (plan D5) — a complement to the headless
// DSP test (which links only factory_core), NOT a change to it.
//
// Checks (plan D5):
//   1. Program names are non-empty and unique.
//   2. Every preset entry's paramID exists in the APVTS layout.
//   3. Every entry value is in range: apply -> read back matches (clamp = fail).
//   4. Program 0 (Init) sets every non-excluded parameter to its default.
//   5. setCurrentProgram(i) -> getCurrentProgram() == i, and the presetIndex
//      survives a getStateInformation / setStateInformation round-trip.
//   6. Every automatable parameter's raw value round-trips through
//      getStateInformation / setStateInformation on a fresh processor instance.
//   7. A frozen v1.2.0-generation state XML (old params only, all non-default)
//      loads so that (a) every old param takes its fixture value and (b) the
//      params added after v1.2.0 (selectivity/tilt/quality) stay at their
//      defaults — the forward-compat guarantee a minor bump relies on.
//
#include "PluginProcessor.h"
#include "FactoryPresets.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    bool isExcluded (const juce::String& id)
    {
        for (int k = 0; k < resonance_suppressor_presets::kNumExclude; ++k)
            if (id == resonance_suppressor_presets::kExclude[k])
                return true;
        return false;
    }

    juce::RangedAudioParameter* rangedParam (ResonanceSuppressorAudioProcessor& p, const juce::String& id)
    {
        return p.apvts.getParameter (id);
    }

    // Denormalised current value of a parameter (reflects setValueNotifyingHost
    // synchronously via the APVTS parameter's stored value).
    double readReal (juce::RangedAudioParameter* rp)
    {
        return (double) rp->convertFrom0to1 (rp->getValue());
    }

    void check1_names (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("1. program names non-empty + unique\n");
        juce::StringArray seen;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            const auto name = p.getProgramName (i);
            if (name.trim().isEmpty())
                fail ("program " + std::to_string (i) + " has an empty name");
            if (seen.contains (name))
                fail ("duplicate program name '" + name.toStdString() + "'");
            seen.add (name);
        }
    }

    void check2_ids_exist (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("2. every preset paramID exists in the layout\n");
        const auto& bank = resonance_suppressor_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {
                const char* id = bank.presets[pr].params[e].paramID;
                if (rangedParam (p, id) == nullptr)
                    fail (std::string ("preset '") + bank.presets[pr].name + "' references unknown paramID '" + id + "'");
                if (isExcluded (juce::String (id)))
                    fail (std::string ("preset '") + bank.presets[pr].name + "' targets excluded paramID '" + id + "'");
            }
    }

    void check3_values_in_range (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("3. every preset value applies without clamping\n");
        const auto& bank = resonance_suppressor_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
        {
            p.setCurrentProgram (pr + 1); // program 0 is Init; bank is 1-based
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {
                const char* id = bank.presets[pr].params[e].paramID;
                const double intended = (double) bank.presets[pr].params[e].value;
                auto* rp = rangedParam (p, id);
                if (rp == nullptr)
                    continue; // reported by check 2
                const double got = readReal (rp);
                const double span = std::abs ((double) rp->convertFrom0to1 (1.0f)
                                              - (double) rp->convertFrom0to1 (0.0f));
                const double tol = 1.0e-3 + 1.0e-4 * span;
                if (std::abs (got - intended) > tol)
                    fail (std::string ("preset '") + bank.presets[pr].name + "' param '" + id
                          + "' applied as " + std::to_string (got) + " (intended "
                          + std::to_string (intended) + ") — out of range / clamped");
            }
        }
    }

    void check4_init_is_default (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("4. program 0 (Init) == all defaults\n");
        // First push a non-default program so a reset is actually exercised.
        p.setCurrentProgram (p.getNumPrograms() - 1);
        p.setCurrentProgram (0);
        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (isExcluded (rp->getParameterID()))
                    continue;
                if (std::abs (rp->getValue() - rp->getDefaultValue()) > 1.0e-6f)
                    fail ("Init leaves '" + rp->getParameterID().toStdString()
                          + "' off its default");
            }
    }

    void check5_index_roundtrip (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("5. setCurrentProgram/getCurrentProgram + presetIndex persistence\n");
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            if (p.getCurrentProgram() != i)
                fail ("getCurrentProgram() != " + std::to_string (i));
        }

        const int idx = p.getNumPrograms() - 1; // a non-zero program
        p.setCurrentProgram (idx);
        juce::MemoryBlock state;
        p.getStateInformation (state);

        // Heap-allocate the extra instances: several AudioProcessors alive at once
        // on the stack overflow Windows' 1 MB thread stack. This processor carries
        // large inline STFT / display buffers.
        auto restored = std::make_unique<ResonanceSuppressorAudioProcessor>();
        restored->setStateInformation (state.getData(), (int) state.getSize());
        if (restored->getCurrentProgram() != idx)
            fail ("presetIndex did not survive state round-trip (got "
                  + std::to_string (restored->getCurrentProgram()) + ", expected "
                  + std::to_string (idx) + ")");

        // A default/legacy state (no presetIndex attribute) must read back as 0.
        auto legacy = std::make_unique<ResonanceSuppressorAudioProcessor>();
        juce::MemoryBlock legacyState;
        if (auto xml = legacy->apvts.copyState().createXml())
            legacy->copyXmlToBinary (*xml, legacyState); // deliberately no presetIndex
        auto legacyRestored = std::make_unique<ResonanceSuppressorAudioProcessor>();
        legacyRestored->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        if (legacyRestored->getCurrentProgram() != 0)
            fail ("legacy state without presetIndex did not default to program 0");
    }

    void check6_full_state_roundtrip (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("6. every parameter value round-trips through get/setStateInformation\n");

        // Walk every non-excluded parameter to a distinct, deliberately
        // non-default normalised value (a 10-point spread that never lands on
        // 0/0.5/1), so a replaceState wiring regression that drops, clamps, or
        // swaps a parameter shows up here even though check 5 only checks the
        // presetIndex attribute.
        int idx = 0;
        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (isExcluded (rp->getParameterID()))
                    continue;
                const float norm = 0.05f + 0.9f * (float) (idx % 10) / 9.0f;
                rp->setValueNotifyingHost (norm);
                ++idx;
            }

        juce::MemoryBlock state;
        p.getStateInformation (state);

        // Heap-allocate: keeps large processors off the 1 MB Windows main-thread stack.
        auto restored = std::make_unique<ResonanceSuppressorAudioProcessor>();
        restored->setStateInformation (state.getData(), (int) state.getSize());

        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (isExcluded (rp->getParameterID()))
                    continue;
                auto* restoredParam = rangedParam (*restored, rp->getParameterID());
                if (restoredParam == nullptr)
                {
                    fail ("restored processor is missing parameter '"
                          + rp->getParameterID().toStdString() + "'");
                    continue;
                }
                const double original = readReal (rp);
                const double got = readReal (restoredParam);
                const double span = std::abs ((double) rp->convertFrom0to1 (1.0f)
                                              - (double) rp->convertFrom0to1 (0.0f));
                const double tol = 1.0e-3 + 1.0e-4 * span;
                if (std::abs (got - original) > tol)
                    fail ("param '" + rp->getParameterID().toStdString()
                          + "' did not survive state round-trip (got " + std::to_string (got)
                          + ", expected " + std::to_string (original) + ")");
            }
    }

    // ------------------------------------------------------------------------
    // check 7 — v1.2.0 state-compat fixture (state forward-compatibility).
    //
    // A FROZEN state blob captured from the v1.2.0 generation, BEFORE this plugin
    // gained selectivity / tilt / quality (v1.3.0). Loading it must (a) restore
    // every old parameter to the fixture value and (b) leave the three new params
    // at their defaults, because juce::AudioProcessorValueTreeState::replaceState
    // tolerates keys absent from the loaded tree — the exact guarantee that lets a
    // parameter-adding change be a *minor* bump instead of a state break.
    //
    // Shape derivation (factory_presets::stateToXml =
    // apvts.copyState().createXml() + ProgramAdapter's presetIndex attribute):
    //   * root tag == the APVTS valueTreeType, i.e. "PARAMS" (applyStateXml only
    //     applies a tree whose tag matches apvts.state.getType()); a "presetIndex"
    //     attribute rides the root.
    //   * one <PARAM id=".." value=".."/> child per parameter, where "value" is the
    //     *denormalised* (real-unit) value APVTS stores in its tree — verified in
    //     juce_AudioProcessorValueTreeState.{h,cpp}: child type "PARAM", property
    //     "value" = unnormalisedValue, property "id" = the parameter ID.
    // Every listed value is deliberately NON-default, so "applied == default" can
    // never masquerade as a pass. The three v1.3.0 params are intentionally ABSENT
    // — that absence is what makes this a genuine v1.2.0 blob.
    //
    // MAINTENANCE (Phase 3/4): when a later phase adds a parameter, append its ID to
    // kV120NewParams so this check keeps proving that a pre-existing session loads it
    // at its default. Do NOT edit the frozen fixture XML below (it must stay a v1.2.0
    // artefact); only the new-params list grows.
    constexpr const char* kV120Fixture = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<PARAMS presetIndex="0">
  <PARAM id="depth" value="65.0"/>
  <PARAM id="sharpness" value="80.0"/>
  <PARAM id="attack" value="25.0"/>
  <PARAM id="release" value="220.0"/>
  <PARAM id="mix" value="75.0"/>
  <PARAM id="delta" value="1.0"/>
  <PARAM id="link" value="0.0"/>
  <PARAM id="bypass" value="1.0"/>
  <PARAM id="mode" value="1.0"/>
  <PARAM id="lc_on" value="0.0"/>
  <PARAM id="lc_freq" value="300.0"/>
  <PARAM id="lc_slope" value="1.0"/>
  <PARAM id="hc_on" value="0.0"/>
  <PARAM id="hc_freq" value="12000.0"/>
  <PARAM id="hc_slope" value="3.0"/>
  <PARAM id="b0_on" value="0.0"/>
  <PARAM id="b0_freq" value="800.0"/>
  <PARAM id="b0_type" value="2.0"/>
  <PARAM id="b0_sens" value="-12.0"/>
  <PARAM id="b1_on" value="0.0"/>
  <PARAM id="b1_freq" value="3200.0"/>
  <PARAM id="b1_type" value="3.0"/>
  <PARAM id="b1_sens" value="9.0"/>
  <PARAM id="b2_on" value="0.0"/>
  <PARAM id="b2_freq" value="4200.0"/>
  <PARAM id="b2_type" value="1.0"/>
  <PARAM id="b2_sens" value="-6.0"/>
  <PARAM id="b3_on" value="0.0"/>
  <PARAM id="b3_freq" value="9500.0"/>
  <PARAM id="b3_type" value="4.0"/>
  <PARAM id="b3_sens" value="15.0"/>
</PARAMS>)XML";

    // Parameters introduced after v1.2.0 (absent from the fixture above). Each must
    // read back at its OWN default when the fixture loads. Append here in later phases.
    // v1.3.0: selectivity/tilt/quality. v1.5.0 (Pass 3B routing): linkAmt/channelMode/
    // scEnable/scListen.
    const char* const kV120NewParams[] = { "selectivity", "tilt", "quality",
                                           "linkAmt", "channelMode", "scEnable", "scListen" };

    // Tolerance mirrors checks 3/6: an absolute floor plus a range-proportional term
    // (denorm -> norm -> denorm is lossy for skewed ranges within ~this bound).
    double roundTripTol (juce::RangedAudioParameter* rp)
    {
        const double span = std::abs ((double) rp->convertFrom0to1 (1.0f)
                                      - (double) rp->convertFrom0to1 (0.0f));
        return 1.0e-3 + 1.0e-4 * span;
    }

    void check7_v120_state_compat()
    {
        std::printf ("7. v1.2.0 state loads: old params applied, new params default\n");

        auto xml = juce::parseXML (juce::String (kV120Fixture));
        if (xml == nullptr) { fail ("v1.2.0 fixture XML did not parse"); return; }

        // Fresh instance so the new params start at their defaults (an earlier check
        // may have moved them). Heap-allocated to stay off the 1 MB Windows stack.
        auto p = std::make_unique<ResonanceSuppressorAudioProcessor>();
        juce::MemoryBlock blob;
        p->copyXmlToBinary (*xml, blob);                       // the production binary shape
        p->setStateInformation (blob.getData(), (int) blob.getSize());

        // (a) Every parameter the fixture lists took its (non-default) value.
        for (auto* e : xml->getChildIterator())
        {
            if (! e->hasTagName ("PARAM")) continue;
            const juce::String id = e->getStringAttribute ("id");

            for (auto* np : kV120NewParams)
                if (id == np)
                    fail (std::string ("fixture must not list post-v1.2.0 param '") + np
                          + "' (it would defeat the compat check)");

            auto* rp = rangedParam (*p, id);
            if (rp == nullptr) { fail ("fixture references unknown paramID '" + id.toStdString() + "'"); continue; }
            const double intended = e->getDoubleAttribute ("value");
            const double got = readReal (rp);
            if (std::abs (got - intended) > roundTripTol (rp))
                fail ("v1.2.0 param '" + id.toStdString() + "' loaded as " + std::to_string (got)
                      + " (fixture " + std::to_string (intended) + ")");
        }

        // (b) The post-v1.2.0 params, absent from the fixture, stayed at default.
        for (auto* np : kV120NewParams)
        {
            auto* rp = rangedParam (*p, np);
            if (rp == nullptr) { fail (std::string ("post-v1.2.0 param '") + np + "' missing from layout"); continue; }
            const double def = (double) rp->convertFrom0to1 (rp->getDefaultValue());
            const double got = readReal (rp);
            if (std::abs (got - def) > roundTripTol (rp))
                fail (std::string ("new param '") + np + "' is " + std::to_string (got)
                      + " after loading v1.2.0 state (expected default " + std::to_string (def) + ")");
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager for async param updates

    // Heap-allocate: this processor plus the extras in check5 must not all sit
    // on the 1 MB Windows thread stack at once (SEGFAULT otherwise).
    auto processor = std::make_unique<ResonanceSuppressorAudioProcessor>();

    std::printf ("resonance-suppressor preset wiring (%d programs)\n", processor->getNumPrograms());

    if (processor->getNumPrograms() != 1 + resonance_suppressor_presets::bank.numPresets)
        fail ("getNumPrograms() != 1 (Init) + bank size");

    check1_names (*processor);
    check2_ids_exist (*processor);
    check3_values_in_range (*processor);
    check4_init_is_default (*processor);
    check5_index_roundtrip (*processor);
    check6_full_state_roundtrip (*processor);
    check7_v120_state_compat();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
