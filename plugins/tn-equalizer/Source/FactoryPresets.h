#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the dynamic EQ.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (Pro-Q-style corrective / tone-shaping starting points) and are NOT final.
// They must not ship without a human listening sign-off from a Standalone build
// (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is verified
// by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default: all 24 bands off) and is intentionally NOT listed here. Each preset
// turns on only the bands it needs (on = 1) and sets that band's
// type/freq/gain/q and, where it dynamically reacts, dyn/thr/rng. Every unused
// band is left unlisted, so ProgramAdapter resets it to its Init default (off).
//
// Band IDs follow the b<N>_<suffix> scheme (pid(band, suffix)); global "bypass"
// is the only non-band parameter. Values are real units: freq in Hz, gain / thr
// / rng in dB, q dimensionless. Choice indices — type: 0 Bell, 1 Low Shelf,
// 2 High Shelf, 3 High Pass, 4 Low Pass. "dyn" is a bool (1 = dynamics on).
//
namespace dynamic_eq_presets
{
    // Parameters presets must never touch: the global "bypass" (fleet-wide) and
    // every band's "lsn" (listen/solo) — a monitoring toggle (plan D4). The listen
    // IDs are enumerated in full because ProgramAdapter excludes by exact match.
    inline constexpr const char* kExclude[] = {
        "bypass",
        "b0_lsn",  "b1_lsn",  "b2_lsn",  "b3_lsn",  "b4_lsn",  "b5_lsn",
        "b6_lsn",  "b7_lsn",  "b8_lsn",  "b9_lsn",  "b10_lsn", "b11_lsn",
        "b12_lsn", "b13_lsn", "b14_lsn", "b15_lsn", "b16_lsn", "b17_lsn",
        "b18_lsn", "b19_lsn", "b20_lsn", "b21_lsn", "b22_lsn", "b23_lsn"
    };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    // Vocal De-Harsh: a dynamic bell taming ~3 kHz harshness plus a dynamic
    // de-ess bell at ~7 kHz. Both sit flat until the band crosses threshold.
    inline constexpr factory_presets::PresetParam kVocalDeHarsh[] = {
        { "b0_on", 1.0f }, { "b0_type", 0.0f }, { "b0_freq", 3000.0f },
        { "b0_gain", 0.0f }, { "b0_q", 2.5f },
        { "b0_dyn", 1.0f }, { "b0_thr", -30.0f }, { "b0_rng", -6.0f },
        { "b1_on", 1.0f }, { "b1_type", 0.0f }, { "b1_freq", 7000.0f },
        { "b1_gain", 0.0f }, { "b1_q", 4.0f },
        { "b1_dyn", 1.0f }, { "b1_thr", -28.0f }, { "b1_rng", -8.0f }
    };
    // Kick Punch: a dynamic low bell that lifts ~60 Hz on transients, plus a
    // static bell scooping ~300 Hz box.
    inline constexpr factory_presets::PresetParam kKickPunch[] = {
        { "b0_on", 1.0f }, { "b0_type", 0.0f }, { "b0_freq", 60.0f },
        { "b0_gain", 3.0f }, { "b0_q", 1.2f },
        { "b0_dyn", 1.0f }, { "b0_thr", -20.0f }, { "b0_rng", 3.0f },
        { "b1_on", 1.0f }, { "b1_type", 0.0f }, { "b1_freq", 300.0f },
        { "b1_gain", -2.5f }, { "b1_q", 1.5f }
    };
    // De-Mud: a static low-mid bell cut plus a low shelf trim to clear buildup.
    inline constexpr factory_presets::PresetParam kDeMud[] = {
        { "b0_on", 1.0f }, { "b0_type", 0.0f }, { "b0_freq", 250.0f },
        { "b0_gain", -4.0f }, { "b0_q", 1.0f },
        { "b1_on", 1.0f }, { "b1_type", 1.0f }, { "b1_freq", 120.0f },
        { "b1_gain", -3.0f }, { "b1_q", 0.7f }
    };
    // Air Lift: a gentle high shelf for air plus a dynamic presence bell that
    // opens up ~5 kHz only on louder passages.
    inline constexpr factory_presets::PresetParam kAirLift[] = {
        { "b0_on", 1.0f }, { "b0_type", 2.0f }, { "b0_freq", 10000.0f },
        { "b0_gain", 4.0f }, { "b0_q", 0.7f },
        { "b1_on", 1.0f }, { "b1_type", 0.0f }, { "b1_freq", 5000.0f },
        { "b1_gain", 0.0f }, { "b1_q", 2.0f },
        { "b1_dyn", 1.0f }, { "b1_thr", -30.0f }, { "b1_rng", 3.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Vocal De-Harsh", kVocalDeHarsh, (int) (sizeof (kVocalDeHarsh) / sizeof (kVocalDeHarsh[0])) },
        { "Kick Punch",     kKickPunch,    (int) (sizeof (kKickPunch)    / sizeof (kKickPunch[0]))    },
        { "De-Mud",         kDeMud,        (int) (sizeof (kDeMud)        / sizeof (kDeMud[0]))        },
        { "Air Lift",       kAirLift,      (int) (sizeof (kAirLift)      / sizeof (kAirLift[0]))      }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
