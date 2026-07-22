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
// units: per-band enhance / width in %, crossovers in Hz, mix in %, output in dB.
// Per-band "mode1..5" are choice parameters, so the value is the choice index
// (0 = Tube, 1 = Tape, 2 = Bright, 3 = Clean, 4 = Glue).
//
// v1.0.0 model: the old direct/wet dB pair collapsed into a single "mix" (0..100 %)
// under the constant-voltage law, and the old global "mode" became per-band. Each
// legacy preset's (wet, direct) was converted to mix via
//     w = 10^((wet-direct)/20),   mix% = round(100 * w / (1 + w))
// and its global mode replicated onto all five bands:
//     Air           wet -8  dir 0 -> w=0.398 -> mix 28 %,  mode Bright(2)
//     Warm Body     wet -9  dir 0 -> w=0.355 -> mix 26 %,  mode Tube  (0)
//     Full Spectrum wet -10 dir 0 -> w=0.316 -> mix 24 %,  mode Tape  (1)
//     Master Glue   wet -12 dir 0 -> w=0.251 -> mix 20 %,  mode Glue  (4)
//
namespace multiband_enhancer_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide; the
    // "delta" (delta-listen) monitor toggle and the per-band "solo" toggles are
    // transient listening state (plan D4) so a preset never changes what you hear
    // through the monitor path.
    inline constexpr const char* kExclude[] = {
        "bypass", "delta", "solo1", "solo2", "solo3", "solo4", "solo5"
    };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // Band enhance ids: enh1 = LO, enh2 = LO-MID, enh3 = MID, enh4 = HI-MID,
    // enh5 = HI (see PluginProcessor kBandNames). Per-band mode ids mode1..5 match.
    // ---- draft presets (pending audition) ----
    inline constexpr factory_presets::PresetParam kAir[] = {
        { "enh4", 20.0f }, { "enh5", 40.0f }, { "wid5", 120.0f },
        { "mode1", 2.0f }, { "mode2", 2.0f }, { "mode3", 2.0f }, { "mode4", 2.0f }, { "mode5", 2.0f },
        { "mix", 28.0f }
    };
    inline constexpr factory_presets::PresetParam kWarmBody[] = {
        { "enh1", 30.0f }, { "enh2", 35.0f }, { "enh3", 12.0f },
        { "mode1", 0.0f }, { "mode2", 0.0f }, { "mode3", 0.0f }, { "mode4", 0.0f }, { "mode5", 0.0f },
        { "mix", 26.0f }
    };
    inline constexpr factory_presets::PresetParam kFullSpectrum[] = {
        { "enh1", 20.0f }, { "enh2", 20.0f }, { "enh3", 20.0f }, { "enh4", 25.0f }, { "enh5", 30.0f },
        { "mode1", 1.0f }, { "mode2", 1.0f }, { "mode3", 1.0f }, { "mode4", 1.0f }, { "mode5", 1.0f },
        { "mix", 24.0f }
    };
    inline constexpr factory_presets::PresetParam kMasterGlue[] = {
        { "enh1", 10.0f }, { "enh2", 12.0f }, { "enh3", 12.0f }, { "enh4", 15.0f }, { "enh5", 18.0f },
        { "mode1", 4.0f }, { "mode2", 4.0f }, { "mode3", 4.0f }, { "mode4", 4.0f }, { "mode5", 4.0f },
        { "mix", 20.0f }, { "output", 0.0f }
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
