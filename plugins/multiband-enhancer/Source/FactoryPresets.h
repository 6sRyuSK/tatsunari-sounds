#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the Tatsumin Enhancer (5-band parallel harmonic enhancer).
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// (based on the reference's Waves-Vitamin-style use cases) and are NOT final.
// They must not ship without a human listening sign-off from a Standalone build
// (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges, application) is verified
// by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: per-band enhance / width in %, crossovers in Hz, direct / wet / output
// in dB. "mode" is a choice parameter, so its value is the choice index (0 = Tube,
// 1 = Tape, 2 = Bright, 3 = Clean, 4 = Glue).
//
namespace multiband_enhancer_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide; the
    // "delta" (delta-listen) monitor toggle is excluded per plan D4 so a preset
    // never changes the monitoring state.
    inline constexpr const char* kExclude[] = { "bypass", "delta" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // Band enhance ids: enh1 = LO, enh2 = LO-MID, enh3 = MID, enh4 = HI-MID,
    // enh5 = HI (see PluginProcessor kBandNames).
    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kAir[] = {
        { "enh4", 20.0f }, { "enh5", 40.0f }, { "wid5", 120.0f },
        { "mode", 2.0f }, { "wet", -8.0f }, { "direct", 0.0f }
    };
    inline constexpr factory_presets::PresetParam kWarmBody[] = {
        { "enh1", 30.0f }, { "enh2", 35.0f }, { "enh3", 12.0f },
        { "mode", 0.0f }, { "wet", -9.0f }, { "direct", 0.0f }
    };
    inline constexpr factory_presets::PresetParam kFullSpectrum[] = {
        { "enh1", 20.0f }, { "enh2", 20.0f }, { "enh3", 20.0f },
        { "enh4", 25.0f }, { "enh5", 30.0f }, { "mode", 1.0f }, { "wet", -10.0f }
    };
    inline constexpr factory_presets::PresetParam kMasterGlue[] = {
        { "enh1", 10.0f }, { "enh2", 12.0f }, { "enh3", 12.0f },
        { "enh4", 15.0f }, { "enh5", 18.0f }, { "mode", 4.0f },
        { "wet", -12.0f }, { "output", 0.0f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Air",           kAir,          (int) (sizeof (kAir)          / sizeof (kAir[0]))          },
        { "Warm Body",     kWarmBody,     (int) (sizeof (kWarmBody)     / sizeof (kWarmBody[0]))     },
        { "Full Spectrum", kFullSpectrum, (int) (sizeof (kFullSpectrum) / sizeof (kFullSpectrum[0])) },
        { "Master Glue",   kMasterGlue,   (int) (sizeof (kMasterGlue)   / sizeof (kMasterGlue[0]))   }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
