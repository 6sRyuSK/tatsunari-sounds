#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the shimmer reverb.
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's octave-shimmer / ambient-reverb settings) and are
// NOT final. They must not ship without a human listening sign-off from a
// Standalone build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges,
// application) is verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: size/damping/shimmer/voicemix/moddepth/mix in %, decay in s, predelay
// in ms, lowcut/highcut/modrate in Hz. "pitcha"/"pitchb" are choice indices
// (0 = +12, 1 = +7, 2 = +5, 3 = +19, 4 = -12 semitones).
//
// EXCLUSIONS: "bypass" (fleet-wide) AND "freeze" — freeze is a monitoring-style
// hold that presets must not engage (plan D4). "Frozen Pad" achieves a frozen
// feel with a very long decay instead of the freeze switch.
//
namespace shimmer_reverb_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin; "freeze" is a monitoring-
    // style hold that presets must not engage (plan D4).
    inline constexpr const char* kExclude[] = { "bypass", "freeze" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kOctaveShimmer[] = {
        { "size", 110.0f }, { "decay", 4.0f }, { "damping", 25.0f }, { "predelay", 20.0f },
        { "shimmer", 50.0f }, { "pitcha", 0.0f }, { "pitchb", 1.0f }, { "voicemix", 30.0f },
        { "lowcut", 150.0f }, { "highcut", 10000.0f }, { "modrate", 0.3f }, { "moddepth", 20.0f },
        { "mix", 35.0f }
    };
    inline constexpr factory_presets::PresetParam kCathedral[] = {
        { "size", 140.0f }, { "decay", 8.0f }, { "damping", 40.0f }, { "predelay", 40.0f },
        { "shimmer", 20.0f }, { "pitcha", 0.0f }, { "pitchb", 1.0f }, { "voicemix", 10.0f },
        { "lowcut", 100.0f }, { "highcut", 8000.0f }, { "modrate", 0.2f }, { "moddepth", 15.0f },
        { "mix", 30.0f }
    };
    inline constexpr factory_presets::PresetParam kAmbientWash[] = {
        { "size", 120.0f }, { "decay", 6.0f }, { "damping", 30.0f }, { "predelay", 10.0f },
        { "shimmer", 40.0f }, { "pitcha", 0.0f }, { "pitchb", 1.0f }, { "voicemix", 20.0f },
        { "lowcut", 200.0f }, { "highcut", 9000.0f }, { "modrate", 0.4f }, { "moddepth", 30.0f },
        { "mix", 50.0f }
    };
    // Frozen feel via a very long decay rather than the (excluded) freeze switch.
    inline constexpr factory_presets::PresetParam kFrozenPad[] = {
        { "size", 150.0f }, { "decay", 15.0f }, { "damping", 20.0f }, { "predelay", 0.0f },
        { "shimmer", 60.0f }, { "pitcha", 0.0f }, { "pitchb", 1.0f }, { "voicemix", 40.0f },
        { "lowcut", 120.0f }, { "highcut", 11000.0f }, { "modrate", 0.15f }, { "moddepth", 25.0f },
        { "mix", 45.0f }
    };
    inline constexpr factory_presets::PresetParam kModulatedSpace[] = {
        { "size", 100.0f }, { "decay", 3.5f }, { "damping", 35.0f }, { "predelay", 15.0f },
        { "shimmer", 30.0f }, { "pitcha", 2.0f }, { "pitchb", 1.0f }, { "voicemix", 15.0f },
        { "lowcut", 150.0f }, { "highcut", 9000.0f }, { "modrate", 1.5f }, { "moddepth", 60.0f },
        { "mix", 35.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Octave Shimmer",  kOctaveShimmer,  (int) (sizeof (kOctaveShimmer)  / sizeof (kOctaveShimmer[0]))  },
        { "Cathedral",       kCathedral,      (int) (sizeof (kCathedral)      / sizeof (kCathedral[0]))      },
        { "Ambient Wash",    kAmbientWash,    (int) (sizeof (kAmbientWash)    / sizeof (kAmbientWash[0]))    },
        { "Frozen Pad",      kFrozenPad,      (int) (sizeof (kFrozenPad)      / sizeof (kFrozenPad[0]))      },
        { "Modulated Space", kModulatedSpace, (int) (sizeof (kModulatedSpace) / sizeof (kModulatedSpace[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
