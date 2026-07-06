//
// preset_test.cpp — wiring verification for tumble-delay's factory presets.
//
// Preset typos (a paramID no parameter owns) and out-of-range values fail
// silently at runtime, so this JUCE-linked console test builds the processor
// headless and checks the table against the live APVTS layout. The independent
// oracle is the parameter layout itself: a clamped value, or an ID with no
// matching parameter, is a bug. This is the "wiring test" category — a complement
// to tests/dsp_test.cpp (which links only factory_core), NOT a change to it.
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
        for (int k = 0; k < tumble_delay_presets::kNumExclude; ++k)
            if (id == tumble_delay_presets::kExclude[k])
                return true;
        return false;
    }

    double readReal (juce::RangedAudioParameter* rp)
    {
        return (double) rp->convertFrom0to1 (rp->getValue());
    }

    void check1_names (TumbleDelayAudioProcessor& p)
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

    void check2_ids_exist (TumbleDelayAudioProcessor& p)
    {
        std::printf ("2. every preset paramID exists in the layout\n");
        const auto& bank = tumble_delay_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {
                const char* id = bank.presets[pr].params[e].paramID;
                if (p.apvts.getParameter (id) == nullptr)
                    fail (std::string ("preset '") + bank.presets[pr].name + "' references unknown paramID '" + id + "'");
                if (isExcluded (juce::String (id)))
                    fail (std::string ("preset '") + bank.presets[pr].name + "' targets excluded paramID '" + id + "'");
            }
    }

    void check3_values_in_range (TumbleDelayAudioProcessor& p)
    {
        std::printf ("3. every preset value applies without clamping\n");
        const auto& bank = tumble_delay_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
        {
            p.setCurrentProgram (pr + 1); // program 0 is Init; bank is 1-based
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {
                const char* id = bank.presets[pr].params[e].paramID;
                const double intended = (double) bank.presets[pr].params[e].value;
                auto* rp = p.apvts.getParameter (id);
                if (rp == nullptr) continue; // reported by check 2
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

    void check4_init_is_default (TumbleDelayAudioProcessor& p)
    {
        std::printf ("4. program 0 (Init) == all defaults\n");
        p.setCurrentProgram (p.getNumPrograms() - 1); // exercise a reset
        p.setCurrentProgram (0);
        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (isExcluded (rp->getParameterID())) continue;
                if (std::abs (rp->getValue() - rp->getDefaultValue()) > 1.0e-6f)
                    fail ("Init leaves '" + rp->getParameterID().toStdString() + "' off its default");
            }
    }

    void check5_index_roundtrip (TumbleDelayAudioProcessor& p)
    {
        std::printf ("5. setCurrentProgram/getCurrentProgram + presetIndex persistence\n");
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            if (p.getCurrentProgram() != i)
                fail ("getCurrentProgram() != " + std::to_string (i));
        }

        const int idx = p.getNumPrograms() - 1;
        p.setCurrentProgram (idx);
        juce::MemoryBlock state;
        p.getStateInformation (state);

        // Heap-allocate: an AudioProcessor can carry large inline buffers, so
        // stacking several instances overflows the 1 MB Windows main-thread stack.
        auto restored = std::make_unique<TumbleDelayAudioProcessor>();
        restored->setStateInformation (state.getData(), (int) state.getSize());
        if (restored->getCurrentProgram() != idx)
            fail ("presetIndex did not survive state round-trip");

        // A legacy state without the presetIndex attribute must default to 0.
        auto legacy = std::make_unique<TumbleDelayAudioProcessor>();
        juce::MemoryBlock legacyState;
        if (auto xml = legacy->apvts.copyState().createXml())
            legacy->copyXmlToBinary (*xml, legacyState);
        auto legacyRestored = std::make_unique<TumbleDelayAudioProcessor>();
        legacyRestored->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        if (legacyRestored->getCurrentProgram() != 0)
            fail ("legacy state without presetIndex did not default to program 0");
    }

    void check6_full_state_roundtrip (TumbleDelayAudioProcessor& p)
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
                if (isExcluded (rp->getParameterID())) continue;
                const float norm = 0.05f + 0.9f * (float) (idx % 10) / 9.0f;
                rp->setValueNotifyingHost (norm);
                ++idx;
            }

        juce::MemoryBlock state;
        p.getStateInformation (state);

        // Heap-allocate: keeps large processors off the 1 MB Windows main-thread stack.
        auto restored = std::make_unique<TumbleDelayAudioProcessor>();
        restored->setStateInformation (state.getData(), (int) state.getSize());

        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (isExcluded (rp->getParameterID())) continue;
                auto* restoredParam = restored->apvts.getParameter (rp->getParameterID());
                if (restoredParam == nullptr)
                {
                    fail ("restored processor is missing parameter '" + rp->getParameterID().toStdString() + "'");
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
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager for async param updates

    // Heap-allocate (see check5): keeps large processors off the Windows stack.
    auto processorPtr = std::make_unique<TumbleDelayAudioProcessor>();
    auto& processor = *processorPtr;
    std::printf ("tumble-delay preset wiring (%d programs)\n", processor.getNumPrograms());

    if (processor.getNumPrograms() != 1 + tumble_delay_presets::bank.numPresets)
        fail ("getNumPrograms() != 1 (Init) + bank size");

    check1_names (processor);
    check2_ids_exist (processor);
    check3_values_in_range (processor);
    check4_init_is_default (processor);
    check5_index_roundtrip (processor);
    check6_full_state_roundtrip (processor);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
