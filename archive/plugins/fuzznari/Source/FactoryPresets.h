#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for Fuzznari (continuous-morph fuzz).
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's Fuzz Face / Big Muff / Fuzz Factory settings) and
// are NOT final. They must not ship without a human listening sign-off from a
// Standalone build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges,
// application) is verified by tests/preset_test.cpp; only the *sound* is pending.
//
// SPECIAL NOTE: "Squeal" (osc) arms the self-oscillator and Stab drives it into
// the oscillation region. The presets below that use them (e.g. "Oscillating
// Chaos") do so intentionally as a sonic character, staying within the declared
// ranges — but that oscillating behaviour is exactly what the audition sign-off
// must confirm before shipping.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: drive/level in dB, bias/gate/stab/tone/mix in %, osc as a bool (0/1).
//
namespace fuzznari_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kClassicFuzzFace[] = {
        { "drive", 18.0f }, { "bias", 0.0f }, { "gate", 0.0f }, { "stab", 0.0f },
        { "osc", 0.0f }, { "tone", 20.0f }, { "level", 0.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kBigMuffWall[] = {
        { "drive", 36.0f }, { "bias", 0.0f }, { "gate", 0.0f }, { "stab", 0.0f },
        { "osc", 0.0f }, { "tone", -30.0f }, { "level", 0.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kVelcroGate[] = {
        { "drive", 40.0f }, { "bias", 10.0f }, { "gate", 60.0f }, { "stab", 0.0f },
        { "osc", 0.0f }, { "tone", 10.0f }, { "level", 0.0f }, { "mix", 100.0f }
    };
    inline constexpr factory_presets::PresetParam kBiasStarve[] = {
        { "drive", 30.0f }, { "bias", -80.0f }, { "gate", 30.0f }, { "stab", 0.0f },
        { "osc", 0.0f }, { "tone", -10.0f }, { "level", 0.0f }, { "mix", 100.0f }
    };
    // Uses the oscillation region on purpose (osc on + high Stab) — sign-off item.
    inline constexpr factory_presets::PresetParam kOscillatingChaos[] = {
        { "drive", 44.0f }, { "bias", -40.0f }, { "gate", 20.0f }, { "stab", 70.0f },
        { "osc", 1.0f }, { "tone", 0.0f }, { "level", -3.0f }, { "mix", 100.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Classic Fuzz Face", kClassicFuzzFace,  (int) (sizeof (kClassicFuzzFace)  / sizeof (kClassicFuzzFace[0]))  },
        { "Big Muff Wall",     kBigMuffWall,      (int) (sizeof (kBigMuffWall)      / sizeof (kBigMuffWall[0]))      },
        { "Velcro Gate",       kVelcroGate,       (int) (sizeof (kVelcroGate)       / sizeof (kVelcroGate[0]))       },
        { "Bias Starve",       kBiasStarve,       (int) (sizeof (kBiasStarve)       / sizeof (kBiasStarve[0]))       },
        { "Oscillating Chaos", kOscillatingChaos, (int) (sizeof (kOscillatingChaos) / sizeof (kOscillatingChaos[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
