#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the NAM Player.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (tone/trim/mix starting points for common guitar-amp use cases) and are NOT
// final. They must not ship without a human listening sign-off from a Standalone
// build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is
// verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: in_trim / out_gain in dB, mix in %, tone_locut / tone_hicut in Hz.
//
// Scope (plan D4): presets carry TONE / TRIM / MIX only. They never touch the
// loaded model / IR files (the "files" child tree is not an APVTS parameter, so
// ProgramAdapter never iterates it) and never touch ir_enable / ir_level, which
// are functional state tied to whatever IR the user has loaded.
//
namespace nam_player_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide; the
    // IR enable/level are excluded because they depend on the user-loaded IR
    // (plan D4) — a tone preset must not switch a cabinet on/off or change its
    // level out from under the user.
    inline constexpr const char* kExclude[] = { "bypass", "ir_enable", "ir_level" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    // Full-range clean DI: light band-limiting, unity trim/level.
    inline constexpr factory_presets::PresetParam kCleanDI[] = {
        { "in_trim", 0.0f }, { "out_gain", 0.0f }, { "mix", 100.0f },
        { "tone_locut", 30.0f }, { "tone_hicut", 18000.0f }
    };
    // Tight rhythm: high-pass the flub, tame the fizz, a touch hotter into the amp.
    inline constexpr factory_presets::PresetParam kTightMetal[] = {
        { "in_trim", 3.0f }, { "out_gain", 0.0f }, { "mix", 100.0f },
        { "tone_locut", 90.0f }, { "tone_hicut", 9000.0f }
    };
    // Warm crunch: gentle low-cut, rolled-off top for a rounder break-up.
    inline constexpr factory_presets::PresetParam kWarmCrunch[] = {
        { "in_trim", 2.0f }, { "out_gain", 0.0f }, { "mix", 100.0f },
        { "tone_locut", 60.0f }, { "tone_hicut", 7000.0f }
    };
    // Bright lead: extra drive in, tighter low-cut, open top end, slight makeup.
    inline constexpr factory_presets::PresetParam kBrightLead[] = {
        { "in_trim", 4.0f }, { "out_gain", 1.0f }, { "mix", 100.0f },
        { "tone_locut", 120.0f }, { "tone_hicut", 14000.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Clean DI",    kCleanDI,    (int) (sizeof (kCleanDI)    / sizeof (kCleanDI[0]))    },
        { "Tight Metal", kTightMetal, (int) (sizeof (kTightMetal) / sizeof (kTightMetal[0])) },
        { "Warm Crunch", kWarmCrunch, (int) (sizeof (kWarmCrunch) / sizeof (kWarmCrunch[0])) },
        { "Bright Lead", kBrightLead, (int) (sizeof (kBrightLead) / sizeof (kBrightLead[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
