#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for mochi-stretch.
//
// v1 SPEC: Init only (see mochi-spec.md's APVTS section) -- no curated presets
// are defined yet. Program 0 ("Init") is synthesised by ProgramAdapter (every
// parameter to its default) and is NOT listed here.
//
// To add a preset later (see .claude/skills — the add-preset workflow):
//   1. Declare a constexpr factory_presets::PresetParam array of (paramID, value)
//      pairs. `value` is the parameter's REAL value in its own units (dB, %, Hz…),
//      not the normalised 0..1 — ProgramAdapter normalises on apply.
//   2. Add a factory_presets::Preset row to kPresets and grow `bank`.
//   3. tests/preset_test.cpp verifies IDs exist and values are in range.
// Preset VALUES/NAMES are taste — do not ship without a human audition sign-off.
//
namespace mochi_stretch_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin. "hold" is deliberately
    // NOT excluded: it is a latching state param (like a loop-freeze) that a
    // preset is allowed to restore, per the v1 spec.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // Init-only bank for v1 (spec-scoped: no curated presets yet).
    inline constexpr factory_presets::Preset* kPresets = nullptr;
    inline constexpr factory_presets::PresetBank bank { kPresets, 0 };
}
