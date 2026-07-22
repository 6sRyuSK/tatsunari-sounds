#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the saturator.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's classic tape/tube/parallel settings) and are NOT
// final. They must not ship without a human listening sign-off from a Standalone
// build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is
// verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: drive/output in dB, mix in %.
//
namespace saturator_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kTapeWarmth[] = {
        { "drive", 6.0f }, { "mix", 65.0f }, { "output", 0.0f }
    };
    inline constexpr factory_presets::PresetParam kTubeDrive[] = {
        { "drive", 14.0f }, { "mix", 100.0f }, { "output", -2.0f }
    };
    inline constexpr factory_presets::PresetParam kParallelGlue[] = {
        { "drive", 18.0f }, { "mix", 35.0f }, { "output", 0.0f }
    };
    inline constexpr factory_presets::PresetParam kFullCrunch[] = {
        { "drive", 30.0f }, { "mix", 100.0f }, { "output", -4.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Tape Warmth",  kTapeWarmth,   (int) (sizeof (kTapeWarmth)   / sizeof (kTapeWarmth[0]))   },
        { "Tube Drive",   kTubeDrive,    (int) (sizeof (kTubeDrive)    / sizeof (kTubeDrive[0]))    },
        { "Parallel Glue", kParallelGlue, (int) (sizeof (kParallelGlue) / sizeof (kParallelGlue[0])) },
        { "Full Crunch",  kFullCrunch,   (int) (sizeof (kFullCrunch)   / sizeof (kFullCrunch[0]))   }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
