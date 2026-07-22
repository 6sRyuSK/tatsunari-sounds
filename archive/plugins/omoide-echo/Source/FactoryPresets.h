#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for omoide-echo.
//
// SCAFFOLD: this bank starts EMPTY (Init only). Program 0 ("Init") is synthesised
// by ProgramAdapter (every parameter to its default) and is NOT listed here.
//
// To add a preset (see .claude/skills — the add-preset workflow):
//   1. Declare a constexpr factory_presets::PresetParam array of (paramID, value)
//      pairs. `value` is the parameter's REAL value in its own units (dB, %, Hz…),
//      not the normalised 0..1 — ProgramAdapter normalises on apply.
//   2. Add a factory_presets::Preset row to kPresets and grow `bank`.
//   3. tests/preset_test.cpp verifies IDs exist and values are in range.
// Preset VALUES/NAMES are taste — do not ship without a human audition sign-off.
//
namespace omoide_echo_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin. Add monitoring toggles
    // (e.g. "delta", "listen") here as needed.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // TODO(scaffold): declare preset parameter arrays and list them in kPresets.
    inline constexpr factory_presets::Preset* kPresets = nullptr;

    // Init-only bank until curated presets are added.
    inline constexpr factory_presets::PresetBank bank { kPresets, 0 };
}
