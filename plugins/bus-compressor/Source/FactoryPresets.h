#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the SSL-G-style stereo bus compressor.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's classic SSL bus-comp settings) and are NOT final.
// They must not ship without a human listening sign-off from a Standalone build
// (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is verified
// by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: threshold / makeup in dB, attack / release in ms, mix in %. "ratio" is
// a choice parameter, so its value is the choice index (0 = 2:1, 1 = 4:1,
// 2 = 10:1).
//
namespace bus_compressor_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kMixGlue[] = {
        { "threshold", -20.0f }, { "ratio", 0.0f }, { "attack", 30.0f },
        { "release", 300.0f }, { "makeup", 2.5f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kPunchyBus[] = {
        { "threshold", -18.0f }, { "ratio", 1.0f }, { "attack", 30.0f },
        { "release", 100.0f }, { "makeup", 3.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kDrumSmash[] = {
        { "threshold", -24.0f }, { "ratio", 1.0f }, { "attack", 10.0f },
        { "release", 100.0f }, { "makeup", 5.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kGentleMaster[] = {
        { "threshold", -14.0f }, { "ratio", 0.0f }, { "attack", 30.0f },
        { "release", 600.0f }, { "makeup", 1.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kParallelCrush[] = {
        { "threshold", -30.0f }, { "ratio", 2.0f }, { "attack", 1.0f },
        { "release", 100.0f }, { "makeup", 6.0f }, { "mix", 40.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Mix Glue",       kMixGlue,       (int) (sizeof (kMixGlue)       / sizeof (kMixGlue[0]))       },
        { "Punchy Bus",     kPunchyBus,     (int) (sizeof (kPunchyBus)     / sizeof (kPunchyBus[0]))     },
        { "Drum Smash",     kDrumSmash,     (int) (sizeof (kDrumSmash)     / sizeof (kDrumSmash[0]))     },
        { "Gentle Master",  kGentleMaster,  (int) (sizeof (kGentleMaster)  / sizeof (kGentleMaster[0]))  },
        { "Parallel Crush", kParallelCrush, (int) (sizeof (kParallelCrush) / sizeof (kParallelCrush[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
