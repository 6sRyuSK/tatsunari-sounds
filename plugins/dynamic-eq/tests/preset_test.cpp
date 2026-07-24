//
// preset_test.cpp — wiring verification for the dynamic EQ's factory presets.
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
//
#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "DeqParams.h"
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
        for (int k = 0; k < dynamic_eq_presets::kNumExclude; ++k)
            if (id == dynamic_eq_presets::kExclude[k])
                return true;
        return false;
    }

    juce::RangedAudioParameter* rangedParam (DynamicEqAudioProcessor& p, const juce::String& id)
    {
        return p.apvts.getParameter (id);
    }

    // Denormalised current value of a parameter (reflects setValueNotifyingHost
    // synchronously via the APVTS parameter's stored value).
    double readReal (juce::RangedAudioParameter* rp)
    {
        return (double) rp->convertFrom0to1 (rp->getValue());
    }

    void check1_names (DynamicEqAudioProcessor& p)
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

    void check2_ids_exist (DynamicEqAudioProcessor& p)
    {
        std::printf ("2. every preset paramID exists in the layout\n");
        const auto& bank = dynamic_eq_presets::bank;
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

    void check3_values_in_range (DynamicEqAudioProcessor& p)
    {
        std::printf ("3. every preset value applies without clamping\n");
        const auto& bank = dynamic_eq_presets::bank;
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

    void check4_init_is_default (DynamicEqAudioProcessor& p)
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

    void check5_index_roundtrip (DynamicEqAudioProcessor& p)
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
        // large inline analyzer rings + 24 bands of state.
        auto restored = std::make_unique<DynamicEqAudioProcessor>();
        restored->setStateInformation (state.getData(), (int) state.getSize());
        if (restored->getCurrentProgram() != idx)
            fail ("presetIndex did not survive state round-trip (got "
                  + std::to_string (restored->getCurrentProgram()) + ", expected "
                  + std::to_string (idx) + ")");

        // A default/legacy state (no presetIndex attribute) must read back as 0.
        auto legacy = std::make_unique<DynamicEqAudioProcessor>();
        juce::MemoryBlock legacyState;
        if (auto xml = legacy->apvts.copyState().createXml())
            legacy->copyXmlToBinary (*xml, legacyState); // deliberately no presetIndex
        auto legacyRestored = std::make_unique<DynamicEqAudioProcessor>();
        legacyRestored->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        if (legacyRestored->getCurrentProgram() != 0)
            fail ("legacy state without presetIndex did not default to program 0");
    }

    void check6_full_state_roundtrip (DynamicEqAudioProcessor& p)
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
        auto restored = std::make_unique<DynamicEqAudioProcessor>();
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
    // check 7 — factory_params ParamDesc table parity with the live APVTS layout.
    // The APVTS layout is now GENERATED from dynamic_eq_params::buildDeqParams() via
    // factory_params::buildApvtsLayout(); this proves the generated layout is
    // BIT-IDENTICAL to what the parameters actually expose, so the migration is a pure
    // refactor with zero behaviour change — table size + host-visible order (positional
    // paramID match), per-param id resolvable / name / label, getDefaultValue() bit-equal
    // to normalizedDefault, Float range conversions (convertFrom0to1 257-pt sweep,
    // convertTo0to1 real-value sweep, snapToLegal for stepped ranges) bit-equal with NO
    // tolerance, Choice count + labels, and uid uniqueness. A failure means fix
    // Range.h/ApvtsAdapter/DeqParams.h — never loosen the comparison (CLAUDE.md hard rule).
    // (Mirrors resonance-suppressor preset_test's check10.)
    void check7_paramdesc_parity (DynamicEqAudioProcessor& p)
    {
        std::printf ("7. factory_params ParamDesc table parity with the live APVTS layout\n");

        const auto table = dynamic_eq_params::buildDeqParams();
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
                continue;
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

                for (int k = 0; k <= 256; ++k)
                {
                    const float pr = (float) k / 256.0f;
                    if (! bitEqual (nr.convertFrom0to1 (pr), factory_params::convertFrom0to1 (spec, pr)))
                    { fail ("convertFrom0to1 not bit-equal for '" + d.id + "' at p=" + std::to_string (pr)); break; }
                }

                std::vector<float> probes { d.minValue, d.maxValue, d.defaultValue };
                if (d.skewCentre > 0.0f) probes.push_back (d.skewCentre);
                for (int k = 0; k <= 64; ++k)
                    probes.push_back (d.minValue + (d.maxValue - d.minValue) * (float) k / 64.0f);
                for (float v : probes)
                    if (! bitEqual (nr.convertTo0to1 (v), factory_params::convertTo0to1 (spec, v)))
                    { fail ("convertTo0to1 not bit-equal for '" + d.id + "' at v=" + std::to_string (v)); break; }

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
    auto processor = std::make_unique<DynamicEqAudioProcessor>();

    std::printf ("dynamic-eq preset wiring (%d programs)\n", processor->getNumPrograms());

    if (processor->getNumPrograms() != 1 + dynamic_eq_presets::bank.numPresets)
        fail ("getNumPrograms() != 1 (Init) + bank size");

    check1_names (*processor);
    check2_ids_exist (*processor);
    check3_values_in_range (*processor);
    check4_init_is_default (*processor);
    check5_index_roundtrip (*processor);
    check6_full_state_roundtrip (*processor);
    check7_paramdesc_parity (*processor);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
