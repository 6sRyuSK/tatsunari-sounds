//
// plugins/pitch-fix/tests/preset_test.cpp — headless wiring test for the Pitch
// TatFixer parameter table and factory-preset bank. Links only factory_params
// + factory_presets (no JUCE, no CLAP, no DSP).
//
// Gates:
//   * table sanity: unique ids/uids, sane ranges, defaults inside range;
//   * every preset value targets an EXISTING parameter and lies inside its
//     range (a typo'd id or out-of-range value fails here, not in a DAW);
//   * the two-family structure from the product brief: the four performance
//     presets write ONLY the buffer mode (0..3, in order); every sound preset
//     pins buffer to Normal (2);
//   * the musical context (key / scale / a4) is excluded: no preset table
//     references those ids, and PresetSession leaves them untouched.
//
#include "PfParams.h"
#include "PfPresets.h"

#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;

static void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("FAIL: %s\n", msg.c_str());
}

int main()
{
    const auto table = pitch_fix_params::buildPfParams();

    // --- table sanity ---------------------------------------------------------
    if (table.size() != 14)
        fail ("expected 14 parameters, got " + std::to_string (table.size()));

    std::set<std::string> ids;
    std::set<unsigned>    uids;
    for (const auto& d : table)
    {
        if (! ids.insert (d.id).second)
            fail ("duplicate id " + d.id);
        if (! uids.insert (d.uid).second)
            fail ("uid collision at " + d.id);
        if (d.uid != factory_params::fnv1a32 (d.id))
            fail ("uid not fnv1a32(id) for " + d.id);
        if (! (d.minValue < d.maxValue))
            fail ("empty range on " + d.id);
        if (d.defaultValue < d.minValue || d.defaultValue > d.maxValue)
            fail ("default out of range on " + d.id);
    }

    factory_params::ParamStore store (table);

    // --- every preset value hits an existing, in-range parameter ---------------
    const auto& bank = pitch_fix_presets::bank;
    if (bank.numPresets != 10)
        fail ("expected 10 presets, got " + std::to_string (bank.numPresets));

    std::set<std::string> names;
    for (int p = 0; p < bank.numPresets; ++p)
    {
        const auto& pr = bank.presets[p];
        if (! names.insert (pr.name).second)
            fail (std::string ("duplicate preset name ") + pr.name);
        for (int e = 0; e < pr.numParams; ++e)
        {
            const auto& pp = pr.params[e];
            const int idx = store.indexOf (pp.paramID);
            if (idx < 0)
            {
                fail (std::string (pr.name) + " references unknown id " + pp.paramID);
                continue;
            }
            const auto& d = store.desc (idx);
            if (pp.value < d.minValue || pp.value > d.maxValue)
                fail (std::string (pr.name) + "." + pp.paramID + " value out of range");
            for (int x = 0; x < pitch_fix_presets::kNumExclude; ++x)
                if (std::string (pp.paramID) == pitch_fix_presets::kExclude[x])
                    fail (std::string (pr.name) + " writes excluded id " + pp.paramID);
        }
    }

    // --- two-family structure ---------------------------------------------------
    const char* perfNames[4] = { "Realtime", "Fast", "Normal", "Quality" };
    for (int p = 0; p < 4; ++p)
    {
        const auto& pr = bank.presets[p];
        if (std::string (pr.name) != perfNames[p])
            fail ("performance preset order/name mismatch at " + std::to_string (p));
        if (pr.numParams != 1 || std::string (pr.params[0].paramID) != "buffer"
            || pr.params[0].value != (float) p)
            fail (std::string (pr.name) + " must set ONLY buffer=" + std::to_string (p));
    }
    for (int p = 4; p < bank.numPresets; ++p)
    {
        const auto& pr = bank.presets[p];
        bool bufferNormal = false;
        for (int e = 0; e < pr.numParams; ++e)
            if (std::string (pr.params[e].paramID) == "buffer" && pr.params[e].value == 2.0f)
                bufferNormal = true;
        if (! bufferNormal)
            fail (std::string (pr.name) + " (sound preset) must pin buffer to Normal");
    }

    // --- PresetSession behaviour: musical context untouched, clean applies -----
    std::vector<std::string> excl (pitch_fix_presets::kExclude,
                                   pitch_fix_presets::kExclude + pitch_fix_presets::kNumExclude);
    factory_presets::PresetSession session (store, bank, excl);

    if (session.numPrograms() != 11)
        fail ("numPrograms != 11 (Init + 10)");

    const int iKey = store.indexOf ("key");
    const int iScale = store.indexOf ("scale");
    const int iA4 = store.indexOf ("a4");
    const int iAmount = store.indexOf ("amount");
    const int iBuffer = store.indexOf ("buffer");
    store.setFromHost (iKey, 9.0f);      // A
    store.setFromHost (iScale, 2.0f);    // Minor
    store.setFromHost (iA4, 432.0f);

    for (int prog = 0; prog < session.numPrograms(); ++prog)
    {
        session.applyProgram (prog);
        if (store.value (iKey) != 9.0f || store.value (iScale) != 2.0f
            || store.value (iA4) != 432.0f)
            fail ("program " + std::to_string (prog) + " touched the musical context");
        if (session.isDirty())
            fail ("dirty right after applyProgram " + std::to_string (prog));
    }

    session.applyProgram (3);            // program 3 == bank[2] "Normal" (0 is Init)
    if (store.value (iBuffer) != 2.0f)
        fail ("Normal preset did not set buffer=2");
    session.applyProgram (7);            // program 7 == bank[6] "Hard Tune"
    if (store.value (iAmount) != 100.0f)
        fail ("Hard Tune did not set amount=100");
    session.applyProgram (0);            // Init: managed params back to defaults
    if (store.value (iAmount) != 100.0f || store.value (iBuffer) != 2.0f)
        fail ("Init did not restore defaults");

    if (g_failures > 0)
    {
        std::printf ("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf ("pitch_fix_preset_test OK\n");
    return 0;
}
