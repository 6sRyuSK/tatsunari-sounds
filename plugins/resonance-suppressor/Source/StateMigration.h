#pragma once
//
// StateMigration.h — the resonance-suppressor state-load policy for the shipping
// CLAP build (3.0.0). JUCE-free (StateCodec's StateModel only), so the CLAP shell
// Policy and a headless test share exactly this logic.
//
// CLEAN BREAK (owner decision, 2026-07-19; 3.0.0 is a MAJOR bump = "breaks state
// compatibility"): RS does NOT import legacy or foreign session state. The state
// wire format (StateCodec) is deliberately framing- and tag-compatible with the
// old JUCE copyXmlToBinary / <PARAMS><PARAM id value/></PARAMS> blob so the codec
// could be validated against it — which means a JUCE/APVTS blob would otherwise
// DECODE and its parameter values would load. The `stateVersion` stamp is what
// tells the two apart: StateCodec always writes stateVersion >= kStateVersionCurrent
// (currently 1); a JUCE-era blob (and any foreign PARAMS blob) carries no such
// attribute and reads back as 0.
//
// So: a version-0 (legacy / pre-versioned / foreign) state is NOT trusted — reset
// the model to defaults (drop every parameter + the program index) so the load
// lands the plugin at its defaults instead of importing an old session. A genuine
// RS state (stateVersion >= 1) passes through untouched and round-trips exactly.
//
// This is the state-level half of the clean break; the identity-level half is the
// new CLAP id + wrapper-generated VST3 UID, so a host treats the 3.0.0 plugin as a
// different plugin from the old JUCE VST3/AU and never even hands it an old session.
// This guard is the defensive backstop that keeps a hand-fed / mismatched foreign
// blob from crashing, hanging, or silently importing.
//
#include "factory_presets/StateCodec.h"

namespace resonance_suppressor_state
{
    inline void cleanBreakMigrate (factory_presets::StateModel& m)
    {
        // stateVersion parses to 0 when the attribute is absent (StateCodec's
        // "legacy / pre-versioned" sentinel); <= 0 also rejects a corrupt negative.
        if (m.stateVersion <= 0)
        {
            m.params.clear();   // -> every store parameter falls back to its default
            m.presetIndex = 0;  // -> program 0 (Init)
        }
    }
}
