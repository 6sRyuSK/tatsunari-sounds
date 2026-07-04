#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the resonance suppressor.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's soothe-style de-harsh / mix-taming settings) and are
// NOT final. They must not ship without a human listening sign-off from a
// Standalone build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges,
// application) is verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: depth/sharpness/mix in %, attack/release in ms, band "sens" in dB.
// "mode" is a choice index (0 = Soft / adaptive, 1 = Hard / absolute-level);
// presets pick the mode that suits the material. Band nodes use the b<N>_ ID
// scheme (b2_ = the 5 kHz band, b3_ = the 8 kHz band).
//
namespace resonance_suppressor_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences the plugin; "delta" is a monitoring toggle (plan D4)
    // so a preset never changes what the user is auditioning.
    inline constexpr const char* kExclude[] = { "bypass", "delta" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    // Sibilance / harshness tamer for vocals: fast, adaptive, focused on 5-8 kHz.
    inline constexpr factory_presets::PresetParam kVocalDeHarsh[] = {
        { "mode", 0.0f }, { "depth", 45.0f }, { "sharpness", 70.0f },
        { "attack", 15.0f }, { "release", 50.0f }, { "mix", 100.0f },
        { "b2_sens", 8.0f }, { "b3_sens", 6.0f }
    };
    // Gentle broadband tame on a full mix: hard mode, slower, transparent.
    inline constexpr factory_presets::PresetParam kFullMixTame[] = {
        { "mode", 1.0f }, { "depth", 30.0f }, { "sharpness", 45.0f },
        { "attack", 40.0f }, { "release", 120.0f }, { "mix", 100.0f }
    };
    // Light, slow smoothing for delicate sources (parallel-ish via 80% mix).
    inline constexpr factory_presets::PresetParam kGentleSmooth[] = {
        { "mode", 0.0f }, { "depth", 20.0f }, { "sharpness", 40.0f },
        { "attack", 60.0f }, { "release", 150.0f }, { "mix", 80.0f }
    };
    // Heavy, surgical resonance removal: hard mode, deep, very fast/narrow.
    inline constexpr factory_presets::PresetParam kAggressiveCut[] = {
        { "mode", 1.0f }, { "depth", 70.0f }, { "sharpness", 85.0f },
        { "attack", 8.0f }, { "release", 40.0f }, { "mix", 100.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Vocal De-Harsh", kVocalDeHarsh,  (int) (sizeof (kVocalDeHarsh)  / sizeof (kVocalDeHarsh[0]))  },
        { "Full Mix Tame",  kFullMixTame,   (int) (sizeof (kFullMixTame)   / sizeof (kFullMixTame[0]))   },
        { "Gentle Smooth",  kGentleSmooth,  (int) (sizeof (kGentleSmooth)  / sizeof (kGentleSmooth[0]))  },
        { "Aggressive Cut", kAggressiveCut, (int) (sizeof (kAggressiveCut) / sizeof (kAggressiveCut[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
