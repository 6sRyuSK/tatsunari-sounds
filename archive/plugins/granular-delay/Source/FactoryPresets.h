#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the granular cloud delay.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's granular / tempo-synced delay settings) and are NOT
// final. They must not ship without a human listening sign-off from a Standalone
// build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is
// verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: delay/grainsize/jitter in ms, feedback/mix/spread/pitchrand-as-%/
// lfodepth in %, density/lforate in Hz, pitch/pitchrand in semitones. "sync" is
// a bool (0/1); "division" is a choice index (0 = 1/4, 1 = 1/4., 2 = 1/8,
// 3 = 1/8., 4 = 1/8T, 5 = 1/16). Tempo-synced presets set sync = 1 + a division;
// free presets set sync = 0 and use delay (ms).
//
namespace granular_delay_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    // Tempo-synced 1/8 cloud.
    inline constexpr factory_presets::PresetParam kTempoCloud[] = {
        { "sync", 1.0f }, { "division", 2.0f }, { "feedback", 50.0f }, { "mix", 40.0f },
        { "grainsize", 120.0f }, { "density", 30.0f }, { "jitter", 20.0f },
        { "pitch", 0.0f }, { "pitchrand", 0.0f }, { "spread", 60.0f },
        { "lforate", 0.5f }, { "lfodepth", 0.0f }
    };
    // Free-time ambient wash.
    inline constexpr factory_presets::PresetParam kFreeAmbient[] = {
        { "sync", 0.0f }, { "delay", 600.0f }, { "feedback", 55.0f }, { "mix", 45.0f },
        { "grainsize", 200.0f }, { "density", 20.0f }, { "jitter", 40.0f },
        { "pitch", 0.0f }, { "pitchrand", 0.0f }, { "spread", 70.0f },
        { "lforate", 0.3f }, { "lfodepth", 15.0f }
    };
    // Octave-up shimmer trails.
    inline constexpr factory_presets::PresetParam kShimmerOctave[] = {
        { "sync", 0.0f }, { "delay", 400.0f }, { "feedback", 45.0f }, { "mix", 40.0f },
        { "grainsize", 150.0f }, { "density", 30.0f }, { "jitter", 20.0f },
        { "pitch", 12.0f }, { "pitchrand", 0.0f }, { "spread", 65.0f },
        { "lforate", 0.4f }, { "lfodepth", 10.0f }
    };
    // Randomised-pitch cloud.
    inline constexpr factory_presets::PresetParam kPitchChaos[] = {
        { "sync", 0.0f }, { "delay", 350.0f }, { "feedback", 40.0f }, { "mix", 50.0f },
        { "grainsize", 80.0f }, { "density", 45.0f }, { "jitter", 100.0f },
        { "pitch", 0.0f }, { "pitchrand", 7.0f }, { "spread", 80.0f },
        { "lforate", 1.0f }, { "lfodepth", 20.0f }
    };
    // Tempo-synced 1/16 stutter.
    inline constexpr factory_presets::PresetParam kRhythmicStutter[] = {
        { "sync", 1.0f }, { "division", 5.0f }, { "feedback", 30.0f }, { "mix", 45.0f },
        { "grainsize", 60.0f }, { "density", 55.0f }, { "jitter", 10.0f },
        { "pitch", 0.0f }, { "pitchrand", 0.0f }, { "spread", 40.0f },
        { "lforate", 0.5f }, { "lfodepth", 0.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Tempo Cloud",      kTempoCloud,      (int) (sizeof (kTempoCloud)      / sizeof (kTempoCloud[0]))      },
        { "Free Ambient",     kFreeAmbient,     (int) (sizeof (kFreeAmbient)     / sizeof (kFreeAmbient[0]))     },
        { "Shimmer Octave",   kShimmerOctave,   (int) (sizeof (kShimmerOctave)   / sizeof (kShimmerOctave[0]))   },
        { "Pitch Chaos",      kPitchChaos,      (int) (sizeof (kPitchChaos)      / sizeof (kPitchChaos[0]))      },
        { "Rhythmic Stutter", kRhythmicStutter, (int) (sizeof (kRhythmicStutter) / sizeof (kRhythmicStutter[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
