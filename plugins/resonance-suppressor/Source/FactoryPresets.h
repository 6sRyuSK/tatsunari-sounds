#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for the resonance suppressor (v2, Phase 6 -- reworked for the
// 2.0 feature set: 8 bands + per-band width, Selectivity, Tilt, Channel Mode
// (M/S), Link Amount).
//
// DRAFT VALUES — the parameter values and names below are a *taste* proposal
// and are NOT final. They must not ship without a human listening sign-off from
// a Standalone build (CLAUDE.md "Ask a human" #1). The wiring (IDs, ranges,
// application) is verified by tests/preset_test.cpp; only the *sound* is pending.
//
// Program 0 ("Init") is synthesised by ProgramAdapter (every parameter to its
// default) and is intentionally NOT listed here. Values are the parameter's real
// units: depth/detail/mix/linkAmt in %, tilt in % (-100..100), attack/release
// in ms, band "sens" in dB, band "width" in octaves. "mode" and "channelMode"
// are choice indices (mode: 0 = Soft/adaptive, 1 = Hard/absolute; channelMode:
// 0 = Stereo, 1 = Mid-Side). Band nodes use the b<N>_ ID scheme (b0..b3 = the
// original four bands -- 1k/2.5k/5k/8k by default; b4..b7 = the Phase 4 bands,
// off/150-12k by default until a preset turns one on).
//
// v2.1: each preset's legacy sharpness/selectivity pair is replaced by the
// single "detail" macro at the migration value (their mean -- the same inverse
// setStateInformation applies to pre-detail sessions), so every preset keeps
// its v2.0.1 detection shape through the new macro. The legacy params are now
// unlisted (ProgramAdapter resets unlisted params to their defaults; the DSP
// no longer reads them). The new "out" trim is likewise unlisted -- every
// preset wants the 0 dB default, and the house style only lists non-default
// values (same as link / quality today; kExclude is unchanged).
//
// The four original presets are kept (same names/count baseline) with values
// re-tuned for the new detector (Selectivity/Tilt added, per-band width
// narrowed where the preset wants a more surgical cut) and two new presets are
// added to demonstrate genuinely new 2.0 capability: Channel Mode (M/S) and a
// 5th reduction band. Program count therefore grows 1+4 -> 1+6 (see the version
// bump / catalog step's report note on DAW program-index implications).
//
namespace resonance_suppressor_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences the plugin; "delta" is a monitoring toggle (plan D4)
    // so a preset never changes what the user is auditioning; "quality" is a
    // latency/CPU trade-off the user owns — a preset switch must not renegotiate
    // host PDC (latency) nor override the user's Fast/Normal/High choice. "scEnable"
    // and "scListen" own the user's sidechain routing and monitor state — a preset
    // must not re-key detection onto (or start monitoring) a sidechain the user has
    // wired, so both are excluded too. (linkAmt / channelMode are ordinary tone
    // controls and remain preset-managed.)
    inline constexpr const char* kExclude[] = { "bypass", "delta", "quality", "scEnable", "scListen" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // ---- draft presets (pending audition) ----

    // Sibilance / harshness tamer for vocals: fast, adaptive, focused on 5-8 kHz.
    // v2: a slight positive Tilt (+20%, highs react faster -- matches the new
    // frame-rate detector's ability to catch a fast "s" onset) and the two
    // harshness bands narrowed (0.35 oct, down from the 0.50 default) for a
    // more precise cut. v2.1: detail 65 == the old sharpness 70 / selectivity
    // 60 pair's migration value (surgical side of centre).
    inline constexpr factory_presets::PresetParam kVocalDeHarsh[] = {
        { "mode", 0.0f }, { "depth", 45.0f }, { "detail", 65.0f },
        { "attack", 10.0f }, { "release", 45.0f }, { "mix", 100.0f },
        { "tilt", 20.0f },
        { "b2_sens", 9.0f }, { "b2_width", 0.35f },
        { "b3_sens", 7.0f }, { "b3_width", 0.35f }
    };
    // Gentle broadband tame on a full mix: hard mode, slower, transparent.
    // v2: Link Amount pulled back to 70% (a partial per-channel <-> linked
    // blend, gentler on a wide stereo mix than the fully-linked default).
    // v2.1: detail 35 == the old sharpness 45 / selectivity 25 pair (broad,
    // soft-knee side -- touches subtle excess everywhere, wider smoothing).
    inline constexpr factory_presets::PresetParam kFullMixTame[] = {
        { "mode", 1.0f }, { "depth", 28.0f }, { "detail", 35.0f },
        { "attack", 35.0f }, { "release", 130.0f }, { "mix", 100.0f },
        { "linkAmt", 70.0f }
    };
    // Light, slow smoothing for delicate sources (parallel-ish via 80% mix).
    // v2: a small negative Tilt (-10%, highs react a touch slower) so the air
    // band stays untouched by anything but a sustained resonance. v2.1:
    // detail 40 == the old sharpness 35 / selectivity 45 pair (gentle side).
    inline constexpr factory_presets::PresetParam kGentleSmooth[] = {
        { "mode", 0.0f }, { "depth", 18.0f }, { "detail", 40.0f },
        { "attack", 55.0f }, { "release", 160.0f }, { "mix", 80.0f },
        { "tilt", -10.0f }
    };
    // Heavy, surgical resonance removal: hard mode, deep, very fast/narrow.
    // v2: a stronger positive Tilt (+30%) and every original band narrowed
    // (0.30 oct) so the "surgical" character actually comes from narrower
    // per-band width, not just higher depth. v2.1: detail 82.5 == the old
    // sharpness 85 / selectivity 80 pair (the most surgical in the bank).
    inline constexpr factory_presets::PresetParam kAggressiveCut[] = {
        { "mode", 1.0f }, { "depth", 70.0f }, { "detail", 82.5f },
        { "attack", 6.0f }, { "release", 35.0f }, { "mix", 100.0f },
        { "tilt", 30.0f },
        { "b0_width", 0.30f }, { "b1_width", 0.30f },
        { "b2_sens", 10.0f }, { "b2_width", 0.30f }, { "b3_width", 0.30f }
    };

    // ---- new presets (2.0 capability demonstrations) ----

    // De-Harsh M/S: runs detection/suppression in Mid-Side (Channel Mode 1) so
    // a de-harsh pass targets the mono-compatible centre (where most vocal/lead
    // harshness lives) while the stereo side content (room, reverb tails, wide
    // synths) is de-harshed through the SAME topology but is naturally less
    // affected by a centre-focused band (b1 at 2.5 kHz, boosted). Demonstrates
    // Channel Mode, a routing control the pre-2.0 bank never touched.
    // v2.1: detail 57.5 == the old sharpness 60 / selectivity 55 pair.
    inline constexpr factory_presets::PresetParam kDeHarshMS[] = {
        { "mode", 0.0f }, { "depth", 40.0f }, { "detail", 57.5f },
        { "attack", 15.0f }, { "release", 60.0f }, { "mix", 100.0f },
        { "tilt", 10.0f },
        { "channelMode", 1.0f },
        { "b1_sens", 8.0f }, { "b1_width", 0.40f },
        { "b2_sens", 6.0f }
    };
    // Sibilance Tamer: extends coverage across the full 5-9 kHz sibilance zone
    // with THREE narrow bands (b2 @ 5k, a newly-enabled b4 @ 6.5k filling the
    // gap, b3 @ 8k), each narrowed to 0.25 oct. Demonstrates enabling one of
    // the Phase 4 bands (b4..b7, off by default) that the pre-2.0 bank never
    // used, plus per-band width as a deliberate shaping tool rather than left
    // at its 0.50 default.
    // v2.1: detail 70 == the old sharpness 75 / selectivity 65 pair.
    inline constexpr factory_presets::PresetParam kSibilanceTamer[] = {
        { "mode", 0.0f }, { "depth", 50.0f }, { "detail", 70.0f },
        { "attack", 8.0f }, { "release", 40.0f }, { "mix", 100.0f },
        { "tilt", 25.0f },
        { "b2_sens", 10.0f }, { "b2_width", 0.25f },
        { "b3_sens", 9.0f },  { "b3_width", 0.25f },
        { "b4_on", 1.0f }, { "b4_freq", 6500.0f }, { "b4_type", 0.0f },
        { "b4_sens", 8.0f }, { "b4_width", 0.25f }
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "Vocal De-Harsh", kVocalDeHarsh,   (int) (sizeof (kVocalDeHarsh)   / sizeof (kVocalDeHarsh[0]))   },
        { "Full Mix Tame",  kFullMixTame,    (int) (sizeof (kFullMixTame)    / sizeof (kFullMixTame[0]))    },
        { "Gentle Smooth",  kGentleSmooth,   (int) (sizeof (kGentleSmooth)   / sizeof (kGentleSmooth[0]))   },
        { "Aggressive Cut", kAggressiveCut,  (int) (sizeof (kAggressiveCut)  / sizeof (kAggressiveCut[0]))  },
        { "De-Harsh M/S",   kDeHarshMS,      (int) (sizeof (kDeHarshMS)      / sizeof (kDeHarshMS[0]))      },
        { "Sibilance Tamer",kSibilanceTamer, (int) (sizeof (kSibilanceTamer) / sizeof (kSibilanceTamer[0])) }
    };

    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0]))
    };
}
