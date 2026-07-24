#pragma once
//
// DeqStateMigration.h — the dynamic-eq state-load policy for the clap-first build.
// JUCE-free (StateCodec's StateModel only), so the CLAP shell Policy shares exactly
// this logic.
//
// CLEAN BREAK (2.0.0 is a MAJOR bump = "breaks state compatibility"): the clap-first
// dynamic-eq does NOT import the legacy JUCE session state. StateCodec is framing-/tag-
// compatible with the old JUCE copyXmlToBinary <PARAMS><PARAM id value/></PARAMS> blob
// (so the codec could be validated against it), which means a JUCE/APVTS blob would
// otherwise DECODE and load. The `stateVersion` stamp tells them apart: StateCodec always
// writes stateVersion >= 1; a JUCE-era (or any foreign PARAMS) blob carries no such
// attribute and reads back as 0. A version-0 state is therefore reset to defaults rather
// than imported; a genuine StateCodec state (stateVersion >= 1) passes through untouched.
// The identity-level half of the break is the new CLAP id + wrapper-generated VST3 UID, so
// a host treats the 2.0.0 plugin as different from the old JUCE VST3/AU; this guard is the
// defensive backstop against a hand-fed / mismatched foreign blob.
//
#include "factory_presets/StateCodec.h"

namespace dynamic_eq_state
{
    inline void cleanBreakMigrate (factory_presets::StateModel& m)
    {
        if (m.stateVersion <= 0)
        {
            m.params.clear();   // -> every store parameter falls back to its default
            m.presetIndex = 0;  // -> program 0 (Init)
        }
    }
}
