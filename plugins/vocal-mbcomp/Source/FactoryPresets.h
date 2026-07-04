#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the vocal-tuned 3-band compressor.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on common vocal-processing use cases) and are NOT final. They must not
// ship without a human listening sign-off from a Standalone build (CLAUDE.md
// "Ask a human" #1). The wiring (IDs, ranges, application) is verified by
// tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: compress / band amount / mix in %, output in dB, crossovers in Hz.
//
namespace vocal_mbcomp_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kLeadVocal[] = {
        { "compress", 45.0f }, { "low", 85.0f }, { "mix", 100.0f },
        { "output", 1.0f }, { "lowfreq", 220.0f }, { "highfreq", 5000.0f }
    };
    inline constexpr factory_presets::PresetParam kBackingVocal[] = {
        { "compress", 62.0f }, { "mix", 85.0f }, { "output", 0.0f },
        { "lowfreq", 250.0f }, { "highfreq", 4500.0f }
    };
    inline constexpr factory_presets::PresetParam kPodcast[] = {
        { "compress", 70.0f }, { "high", 70.0f }, { "mix", 100.0f },
        { "output", 2.0f }, { "lowfreq", 180.0f }, { "highfreq", 3500.0f }
    };
    inline constexpr factory_presets::PresetParam kBrightMix[] = {
        { "compress", 35.0f }, { "low", 90.0f }, { "mix", 100.0f },
        { "output", 0.5f }, { "lowfreq", 300.0f }, { "highfreq", 6000.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Lead Vocal",    kLeadVocal,    (int) (sizeof (kLeadVocal)    / sizeof (kLeadVocal[0]))    },
        { "Backing Vocal", kBackingVocal, (int) (sizeof (kBackingVocal) / sizeof (kBackingVocal[0])) },
        { "Podcast",       kPodcast,      (int) (sizeof (kPodcast)      / sizeof (kPodcast[0]))      },
        { "Bright Mix",    kBrightMix,    (int) (sizeof (kBrightMix)    / sizeof (kBrightMix[0]))    }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
