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
//   8. (Phase 5c) factory_presets::UserPresetStore round-trips: save -> list
//      contains -> load -> XML equivalent -> remove -> list empty; illegal
//      filename characters sanitise (still self-consistent load/remove
//      through the same name); overwrite replaces in place (no duplicate row).
//      Runs entirely against a scratch temp directory (the File-override
//      constructor) — never touches the real userAppData.
//   9. (v2.1) "detail" state migration: a v2.0.1-shaped state (legacy
//      sharpness/selectivity, no detail) loads detail at (sharp% + sel%)/2;
//      a new-format state (detail present) is taken verbatim. check 7 also
//      asserts the migration value against its v1.2.0 fixture.
//  10. (Phase P1) factory_params ParamDesc table parity: the APVTS layout is now
//      GENERATED from resonance_suppressor_params::buildRsParams() via
//      factory_params::buildApvtsLayout(). This checks the generated layout is
//      BIT-IDENTICAL to what the parameters expose — table size + host-visible
//      order (positional paramID match), per-param id resolvable/name/label,
//      getDefaultValue() bit-equal to normalizedDefault, Float range conversions
//      (convertFrom0to1 257-pt sweep, convertTo0to1 real-value sweep, snapToLegal
//      for stepped ranges) bit-equal with NO tolerance, Choice count+labels, and
//      uid uniqueness. A failure means fix Range.h/ApvtsAdapter/Params.h — never
//      loosen the comparison (CLAUDE.md hard rule).
//
#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "Params.h"
#include "factory_presets/UserPresetStore.h"
#include "factory_params/ParamDesc.h"
#include "factory_params/Range.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    // Bit-exact float comparison (no tolerance) for the paramdesc parity check.
    bool bitEqual (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; }

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
    // scEnable/scListen. v1.6.0 (Phase 4: 8-band width EQ): b4..b7 on/freq/type/sens
    // (bands 5-8, off by default) and b0..b7 width (all 8 bands, default 0.50).
    // v2.1: "out" (default 0 dB). NOTE: v2.1's "detail" is deliberately NOT in this
    // list -- setStateInformation MIGRATES it from the legacy sharpness/selectivity
    // values when it is absent, so a pre-detail state loads it at (sharp% + sel%)/2
    // rather than its default; check7 asserts that migration value explicitly, and
    // check9 covers the v2.0.1-shaped fixtures.
    const char* const kV120NewParams[] = { "selectivity", "tilt", "quality",
                                           "linkAmt", "channelMode", "scEnable", "scListen",
                                           "b4_on", "b4_freq", "b4_type", "b4_sens",
                                           "b5_on", "b5_freq", "b5_type", "b5_sens",
                                           "b6_on", "b6_freq", "b6_type", "b6_sens",
                                           "b7_on", "b7_freq", "b7_type", "b7_sens",
                                           "b0_width", "b1_width", "b2_width", "b3_width",
                                           "b4_width", "b5_width", "b6_width", "b7_width",
                                           "out" };

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

        // (c) v2.1 "detail" migration: the fixture lists sharpness = 80 and (being
        // v1.2.0) no selectivity, which the migration defaults to 50 -- so detail
        // must load as (80 + 50) / 2 = 65, NOT its default 50. Oracle: the spec
        // formula, restated here by hand.
        if (auto* rp = rangedParam (*p, "detail"))
        {
            const double got = readReal (rp);
            if (std::abs (got - 65.0) > roundTripTol (rp))
                fail ("v2.1 'detail' migrated to " + std::to_string (got)
                      + " from the v1.2.0 fixture (expected (80+50)/2 = 65)");
        }
        else fail ("v2.1 param 'detail' missing from layout");
    }

    // ------------------------------------------------------------------------
    // check 8 — factory_presets::UserPresetStore (Phase 5c shared infra).
    //
    // Pure file-I/O round-trips against a scratch temp directory (the
    // File-override constructor -- see UserPresetStore.h), independent of any
    // processor. The oracle is direct filesystem inspection (existsAsFile() on
    // the path this test computes itself via juce::File::createLegalFileName,
    // the same JUCE primitive the store uses) plus juce::XmlElement's own
    // isEquivalentTo, not a re-derivation of the store's internal logic.
    void check8_user_preset_store()
    {
        std::printf ("8. UserPresetStore round-trip (save/list/load/remove, sanitisation, overwrite)\n");

        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("rs_user_preset_test_" + juce::String ((juce::int64) juce::Time::getHighResolutionTicks()));
        tempDir.deleteRecursively(); // in case a previous crashed run left it behind
        factory_presets::UserPresetStore store (tempDir);

        // Directory doesn't exist yet -- list()/exists() must not throw/assert.
        if (! store.list().isEmpty())
            fail ("fresh store is not empty before anything was saved");
        if (store.exists ("Anything"))
            fail ("exists() true before anything was saved");
        if (store.load ("Anything") != nullptr)
            fail ("load() non-null before anything was saved");

        // save -> list contains -> load -> XML equivalent -> remove -> list empty.
        juce::XmlElement fixtureA ("TestState");
        fixtureA.setAttribute ("foo", 42);
        fixtureA.setAttribute ("bar", "hello");

        if (! store.save ("My Preset", fixtureA))
            fail ("save('My Preset') returned false");
        if (! store.list().contains ("My Preset"))
            fail ("list() does not contain 'My Preset' after save");
        if (! store.exists ("My Preset"))
            fail ("exists('My Preset') false after save");

        auto loadedA = store.load ("My Preset");
        if (loadedA == nullptr)
            fail ("load('My Preset') returned nullptr");
        else if (! loadedA->isEquivalentTo (&fixtureA, false))
            fail ("loaded XML is not equivalent to the saved fixture");

        if (! store.remove ("My Preset"))
            fail ("remove('My Preset') returned false");
        if (store.list().contains ("My Preset"))
            fail ("list() still contains 'My Preset' after remove");
        if (store.exists ("My Preset"))
            fail ("exists('My Preset') true after remove");
        if (store.load ("My Preset") != nullptr)
            fail ("load('My Preset') non-null after remove");
        if (! store.remove ("My Preset"))
            fail ("remove() of an already-absent preset must be a no-op success, not a failure");

        // Illegal filename characters: still self-consistent through save/list/
        // load/remove of the SAME raw name (independent oracle: this test's own
        // call to juce::File::createLegalFileName, checked directly against disk).
        const juce::String illegalName = "weird:name?/here";
        const juce::String expectedLegal = juce::File::createLegalFileName (illegalName);
        if (! store.save (illegalName, fixtureA))
            fail ("save() with illegal filename characters returned false");
        if (! tempDir.getChildFile (expectedLegal + ".xml").existsAsFile())
            fail ("save() with illegal filename characters did not sanitise to the expected path '"
                  + expectedLegal.toStdString() + ".xml'");
        if (! store.list().contains (expectedLegal))
            fail ("list() does not contain the sanitised name '" + expectedLegal.toStdString() + "'");
        auto loadedIllegal = store.load (illegalName); // same raw name re-sanitises identically
        if (loadedIllegal == nullptr)
            fail ("load() of the original illegal-name string returned nullptr");
        else if (! loadedIllegal->isEquivalentTo (&fixtureA, false))
            fail ("loaded XML (illegal-name case) is not equivalent to the saved fixture");
        if (! store.remove (illegalName))
            fail ("remove() of the original illegal-name string returned false");

        // Overwrite: second save() under the same name replaces in place (no
        // duplicate list() entry, and the load reflects the NEW content).
        juce::XmlElement fixtureB ("TestState");
        fixtureB.setAttribute ("foo", 99);
        if (! store.save ("Over", fixtureA))
            fail ("save('Over', fixtureA) returned false");
        if (! store.save ("Over", fixtureB))
            fail ("save('Over', fixtureB) [overwrite] returned false");
        const auto namesAfterOverwrite = store.list();
        if (namesAfterOverwrite.size() != 1 || ! namesAfterOverwrite.contains ("Over"))
            fail ("overwrite produced " + std::to_string (namesAfterOverwrite.size())
                  + " entries named 'Over' (expected exactly 1)");
        auto loadedOver = store.load ("Over");
        if (loadedOver == nullptr || ! loadedOver->isEquivalentTo (&fixtureB, false))
            fail ("load('Over') after overwrite did not reflect the second save");

        tempDir.deleteRecursively(); // leave no artefact behind, pass or fail
    }

    // ------------------------------------------------------------------------
    // check 9 — v2.0.1 -> v2.1 "detail" state migration (setStateInformation).
    //
    // A v2.0.1-generation state carries the legacy sharpness/selectivity pair
    // and no "detail" child; loading it must inject detail = (sharp% + sel%)/2
    // (the migration inverse of the two detail->engine maps) BEFORE
    // applyStateXml, while a new-format state (detail present) must pass
    // through untouched (no double-migration). Minimal PARAMS-shaped fixtures
    // (applyStateXml only needs the matching root tag); the oracle is the spec
    // formula restated by hand. The migration MATH itself is additionally
    // covered headlessly in tests/dsp_test.cpp (detailParamMathTest); this
    // check covers the XML plumbing end to end through the production
    // setStateInformation path.
    void check9_detail_migration()
    {
        std::printf ("9. v2.0.1 state migrates detail = (sharpness + selectivity) / 2\n");

        auto loadFixture = [] (const char* fixtureXml)
        {
            auto xml = juce::parseXML (juce::String (fixtureXml));
            auto p = std::make_unique<ResonanceSuppressorAudioProcessor>();
            if (xml == nullptr) { fail ("check9 fixture XML did not parse"); return p; }
            juce::MemoryBlock blob;
            p->copyXmlToBinary (*xml, blob); // the production binary shape
            p->setStateInformation (blob.getData(), (int) blob.getSize());
            return p;
        };

        // (a) v2.0.1-shaped state: legacy pair present, no detail -> migrated.
        {
            auto p = loadFixture (R"XML(<?xml version="1.0" encoding="UTF-8"?>
<PARAMS presetIndex="0">
  <PARAM id="sharpness" value="70.0"/>
  <PARAM id="selectivity" value="60.0"/>
</PARAMS>)XML");
            auto* rp = rangedParam (*p, "detail");
            if (rp == nullptr) { fail ("check9: 'detail' missing from layout"); return; }
            const double got = readReal (rp);
            if (std::abs (got - 65.0) > roundTripTol (rp))
                fail ("check9: detail migrated to " + std::to_string (got)
                      + " (expected (70+60)/2 = 65)");
        }

        // (b) New-format state: detail present -> taken verbatim, legacy pair
        // ignored by the migration (no double-migration).
        {
            auto p = loadFixture (R"XML(<?xml version="1.0" encoding="UTF-8"?>
<PARAMS presetIndex="0">
  <PARAM id="sharpness" value="70.0"/>
  <PARAM id="selectivity" value="60.0"/>
  <PARAM id="detail" value="33.0"/>
</PARAMS>)XML");
            auto* rp = rangedParam (*p, "detail");
            if (rp == nullptr) { fail ("check9: 'detail' missing from layout"); return; }
            const double got = readReal (rp);
            if (std::abs (got - 33.0) > roundTripTol (rp))
                fail ("check9: new-format detail was re-migrated to " + std::to_string (got)
                      + " (expected the stored 33)");
        }
    }

    // ------------------------------------------------------------------------
    // check 10 — factory_params ParamDesc table parity with the live APVTS layout
    // (Phase P1). The APVTS layout is now GENERATED from resonance_suppressor_params
    // ::buildRsParams() via factory_params::buildApvtsLayout(); this proves the
    // generated layout is BIT-IDENTICAL to what the parameters actually expose, so
    // the migration is a pure refactor with zero behaviour change.
    //
    // The oracle is the live juce parameter objects. Comparisons that must be exact
    // (default, and the Float range conversions) use ==/memcmp with NO tolerance —
    // per CLAUDE.md, if any of these fail the fix is in Range.h/ApvtsAdapter/Params.h,
    // never a loosened comparison here.
    void check10_paramdesc_parity (ResonanceSuppressorAudioProcessor& p)
    {
        std::printf ("10. factory_params ParamDesc table parity with the live APVTS layout\n");

        const auto table = resonance_suppressor_params::buildRsParams();
        const auto& params = p.getParameters();

        if ((int) table.size() != params.size())
            fail ("table size " + std::to_string (table.size()) + " != live parameter count "
                  + std::to_string (params.size()));

        const int n = juce::jmin ((int) table.size(), params.size());
        for (int i = 0; i < n; ++i)
        {
            const auto& d = table[(size_t) i];
            auto* base = params[i];
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base);
            if (rp == nullptr) { fail ("param index " + std::to_string (i) + " is not a RangedAudioParameter"); continue; }

            // Host-visible ORDER: table id == the id at the same positional index.
            if (rp->getParameterID() != juce::String (d.id))
            {
                fail ("order mismatch at index " + std::to_string (i) + ": table '" + d.id
                      + "' vs live '" + rp->getParameterID().toStdString() + "'");
                continue; // positional desync — the remaining per-field checks would be noise
            }

            // Resolvable by id, and it is the same object as the positional one.
            auto* byId = p.apvts.getParameter (juce::String (d.id));
            if (byId == nullptr) { fail ("id '" + d.id + "' not resolvable via apvts.getParameter"); continue; }
            if (byId != base)     fail ("apvts.getParameter('" + d.id + "') != positional parameter");

            // Display name + label (label == desc.unit; empty for bool/choice).
            if (rp->getName (512) != juce::String (d.name))
                fail ("name mismatch for '" + d.id + "': live '" + rp->getName (512).toStdString()
                      + "' vs table '" + d.name + "'");
            if (rp->getLabel() != juce::String (d.unit))
                fail ("label mismatch for '" + d.id + "': live '" + rp->getLabel().toStdString()
                      + "' vs table unit '" + d.unit + "'");

            // Default: BIT-equal to normalizedDefault.
            if (! bitEqual (rp->getDefaultValue(), factory_params::normalizedDefault (d)))
                fail ("getDefaultValue() not bit-equal to normalizedDefault for '" + d.id + "'");

            if (d.type == factory_params::ParamType::Float)
            {
                const auto& nr = rp->getNormalisableRange();
                const auto spec = factory_params::makeRange (d);

                // convertFrom0to1: 257-point normalised sweep, bit-equal.
                for (int k = 0; k <= 256; ++k)
                {
                    const float pr = (float) k / 256.0f;
                    if (! bitEqual (nr.convertFrom0to1 (pr), factory_params::convertFrom0to1 (spec, pr)))
                    { fail ("convertFrom0to1 not bit-equal for '" + d.id + "' at p=" + std::to_string (pr)); break; }
                }

                // convertTo0to1: real-value sweep (min, max, default, skew centre,
                // + 64 interior points), bit-equal.
                std::vector<float> probes { d.minValue, d.maxValue, d.defaultValue };
                if (d.skewCentre > 0.0f) probes.push_back (d.skewCentre);
                for (int k = 0; k <= 64; ++k)
                    probes.push_back (d.minValue + (d.maxValue - d.minValue) * (float) k / 64.0f);
                for (float v : probes)
                    if (! bitEqual (nr.convertTo0to1 (v), factory_params::convertTo0to1 (spec, v)))
                    { fail ("convertTo0to1 not bit-equal for '" + d.id + "' at v=" + std::to_string (v)); break; }

                // snapToLegalValue: bit-equal for stepped ranges on probe values.
                if (d.interval > 0.0f)
                {
                    const float sp[] = { d.minValue, d.maxValue, d.defaultValue,
                                         d.minValue + 0.333f * (d.maxValue - d.minValue),
                                         d.minValue + 0.777f * (d.maxValue - d.minValue) };
                    for (float v : sp)
                        if (! bitEqual (nr.snapToLegalValue (v), factory_params::snapToLegalValue (spec, v)))
                        { fail ("snapToLegalValue not bit-equal for '" + d.id + "' at v=" + std::to_string (v)); break; }
                }
            }
            else if (d.type == factory_params::ParamType::Choice)
            {
                auto* ch = dynamic_cast<juce::AudioParameterChoice*> (base);
                if (ch == nullptr) { fail ("choice '" + d.id + "' is not a juce::AudioParameterChoice"); continue; }
                if (ch->choices.size() != (int) d.choices.size())
                    fail ("choice count mismatch for '" + d.id + "': live " + std::to_string (ch->choices.size())
                          + " vs table " + std::to_string (d.choices.size()));
                else
                    for (int c = 0; c < ch->choices.size(); ++c)
                        if (ch->choices[c] != juce::String (d.choices[(size_t) c]))
                            fail ("choice label mismatch for '" + d.id + "' at index " + std::to_string (c)
                                  + ": live '" + ch->choices[c].toStdString() + "' vs table '"
                                  + d.choices[(size_t) c] + "'");
            }
            // Bool: default / name / label already checked above.
        }

        // uid uniqueness across the whole table.
        for (size_t i = 0; i < table.size(); ++i)
            for (size_t j = i + 1; j < table.size(); ++j)
                if (table[i].uid == table[j].uid)
                    fail ("uid collision between '" + table[i].id + "' and '" + table[j].id + "'");
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
    check8_user_preset_store();
    check9_detail_migration();
    check10_paramdesc_parity (*processor);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
