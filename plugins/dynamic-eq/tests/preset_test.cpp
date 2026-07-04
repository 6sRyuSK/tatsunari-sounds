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

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
