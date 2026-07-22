#pragma once
//
// plugins/pitch-fix/PfPresets.h — the Pitch TatFixer factory-preset bank.
//
// TWO FAMILIES (per the product brief):
//   * PERFORMANCE presets (Realtime/Fast/Normal/Quality) change ONLY the
//     Buffer mode — sound-neutral latency/quality trade-offs.
//   * SOUND presets shape the correction character (speed, tolerance, glide);
//     they all pin Buffer to Normal so a sound choice never surprises the
//     session's latency budget.
//
// The musical context — key, scale, A4 reference — is NEVER written by any
// preset (including Init): those ids live in kExclude and PresetSession skips
// them, so switching presets keeps the song's key settings intact.
//
// Values are a first tuning draft: sonic fine-tuning is a listening judgement
// (Ask a human) — the wiring and the two-family structure are what tests gate.
//
#include "factory_presets/PresetBank.h"

namespace pitch_fix_presets
{
    using factory_presets::Preset;
    using factory_presets::PresetParam;
    using factory_presets::PresetBank;

    // --- performance family (Buffer only — everything else at defaults) -----
    inline constexpr PresetParam kRealtime[] = { { "buffer", 0.0f } };
    inline constexpr PresetParam kFast[]     = { { "buffer", 1.0f } };
    inline constexpr PresetParam kNormal[]   = { { "buffer", 2.0f } };
    inline constexpr PresetParam kQuality[]  = { { "buffer", 3.0f } };

    // --- sound family (Buffer pinned to Normal) ------------------------------
    inline constexpr PresetParam kNaturalVocal[] = {
        { "buffer", 2.0f }, { "amount", 90.0f },  { "retune", 140.0f },
        { "glide", 90.0f }, { "tolerance", 25.0f }, { "hysteresis", 25.0f },
    };
    inline constexpr PresetParam kTightPop[] = {
        { "buffer", 2.0f }, { "amount", 100.0f }, { "retune", 25.0f },
        { "glide", 30.0f }, { "tolerance", 5.0f },  { "hysteresis", 12.0f },
    };
    inline constexpr PresetParam kHardTune[] = {
        { "buffer", 2.0f }, { "amount", 100.0f }, { "retune", 0.0f },
        { "glide", 0.0f },  { "tolerance", 0.0f },  { "hysteresis", 8.0f },
    };
    inline constexpr PresetParam kGentleTouch[] = {
        { "buffer", 2.0f }, { "amount", 65.0f },  { "retune", 220.0f },
        { "glide", 140.0f }, { "tolerance", 35.0f }, { "hysteresis", 25.0f },
    };
    inline constexpr PresetParam kBalladGlide[] = {
        { "buffer", 2.0f }, { "amount", 95.0f },  { "retune", 110.0f },
        { "glide", 260.0f }, { "tolerance", 18.0f }, { "hysteresis", 20.0f },
    };
    inline constexpr PresetParam kVibratoKeeper[] = {
        { "buffer", 2.0f }, { "amount", 100.0f }, { "retune", 90.0f },
        { "glide", 60.0f },  { "tolerance", 45.0f }, { "hysteresis", 30.0f },
    };

    inline constexpr Preset kPresets[] = {
        { "Realtime",       kRealtime,      1 },
        { "Fast",           kFast,          1 },
        { "Normal",         kNormal,        1 },
        { "Quality",        kQuality,       1 },
        { "Natural Vocal",  kNaturalVocal,  6 },
        { "Tight Pop",      kTightPop,      6 },
        { "Hard Tune",      kHardTune,      6 },
        { "Gentle Touch",   kGentleTouch,   6 },
        { "Ballad Glide",   kBalladGlide,   6 },
        { "Vibrato Keeper", kVibratoKeeper, 6 },
    };

    inline const PresetBank bank { kPresets, 10 };

    // Musical context is the user's, never a preset's.
    inline constexpr const char* kExclude[] = { "key", "scale", "a4" };
    inline constexpr int kNumExclude = 3;
} // namespace pitch_fix_presets
