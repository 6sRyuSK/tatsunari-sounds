#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for tumble-delay.
//
// DRAFT VALUES — pending a human listening/audition sign-off (CLAUDE.md
// "Ask a human" #1). This table only wires parameter targets; tests/preset_test.cpp
// verifies every paramID exists and every value applies unclamped, but NOT that the
// preset sounds right. Program 0 ("Init") is synthesised by ProgramAdapter (every
// parameter to its default) and is deliberately NOT listed here. Concepts/rationale
// per preset: docs/plans/physics-granular-delay.md §8.
//
// Each preset lists only the parameters that deviate from default plus the
// signature params that define its character (see comments per preset below);
// every unlisted parameter falls back to its default when the program is applied.
//
namespace tumble_delay_presets
{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin. Add monitoring toggles
    // (e.g. "delta", "listen") here as needed.
    inline constexpr const char* kExclude[] = { "bypass" };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // 1. First Bounce — the default character, pinned (square box, gentle spin,
    //    single billiard-like ball). Values intentionally mirror the parameter
    //    defaults; pinned as a named, discoverable preset distinct from Init.
    inline constexpr factory_presets::PresetParam kFirstBounce[] = {
        { "boxShape", 1.0f }, { "boxSize", 0.40f }, { "spin", 0.20f },
        { "gravity", 0.0f }, { "refeed", 0.0f }, { "tone", 12000.0f }, { "mix", 35.0f },
        { "aOn", 1.0f }, { "aCount", 1.0f }, { "aBounce", 70.0f }, { "aTime", 350.0f },
        { "aGrain", 90.0f }, { "aMotion", 0.0f },
    };

    // 2. Tumbler — hexagon box that spins hard and keeps rolling (low drag,
    //    time-based life so it rings on).
    inline constexpr factory_presets::PresetParam kTumbler[] = {
        { "boxShape", 3.0f }, { "spin", 0.8f }, { "gravity", 40.0f },
        { "spawnSpread", 15.0f }, { "mix", 40.0f },
        { "aOn", 1.0f }, { "aCount", 3.0f }, { "aTime", 250.0f }, { "aBounce", 80.0f },
        { "aDrag", 5.0f }, { "aLifeTime", 6.0f }, { "aGrain", 120.0f },
    };

    // 3. Ball Drop — pure accelerando, no spin, high bounce.
    inline constexpr factory_presets::PresetParam kBallDrop[] = {
        { "boxShape", 1.0f }, { "spin", 0.0f }, { "gravity", 85.0f }, { "mix", 45.0f },
        { "aOn", 1.0f }, { "aCount", 1.0f }, { "aSpeed", 0.6f }, { "aDirection", 270.0f },
        { "aDirRandom", 10.0f }, { "aBounce", 85.0f }, { "aDrag", 0.0f },
        { "aLifeTime", 8.0f }, { "aGrain", 60.0f },
    };

    // 4. Pinwheel — triangle box, eccentric pivot, spin synced to 1 bar/rev.
    inline constexpr factory_presets::PresetParam kPinwheel[] = {
        { "boxShape", 0.0f }, { "spin", 0.5f }, { "spinSync", 3.0f },
        { "pivotX", 0.55f }, { "pivotY", 0.35f }, { "gravity", 20.0f }, { "mix", 40.0f },
        { "aOn", 1.0f }, { "aCount", 3.0f }, { "aTime", 333.0f }, { "aBounce", 75.0f },
        { "aGrain", 80.0f },
    };

    // 5. Shimmer Fall — generation shimmer via Refeed + pitched grains, life
    //    capped by bounce count rather than time.
    inline constexpr factory_presets::PresetParam kShimmerFall[] = {
        { "spin", 0.15f }, { "gravity", 30.0f }, { "refeed", 60.0f }, { "tone", 9000.0f },
        { "mix", 45.0f },
        { "aOn", 1.0f }, { "aCount", 2.0f }, { "aTime", 500.0f }, { "aBounce", 80.0f },
        { "aLifeMode", 1.0f }, { "aLifeBounces", 6.0f }, { "aPitch", 12.0f }, { "aGrain", 150.0f },
    };

    // 6. Marble Duet — slow big ball on Slot A + fast small high-pitched ball
    //    on Slot B.
    inline constexpr factory_presets::PresetParam kMarbleDuet[] = {
        { "spin", 0.25f }, { "gravity", 15.0f }, { "mix", 45.0f },
        { "aOn", 1.0f }, { "aCount", 2.0f }, { "aBallSize", 18.0f }, { "aSpeed", 0.5f },
        { "aBounce", 75.0f }, { "aPitch", -5.0f }, { "aGrain", 200.0f },
        { "bOn", 1.0f }, { "bCount", 4.0f }, { "bTime", 180.0f }, { "bBallSize", 4.0f },
        { "bSpeed", 2.0f }, { "bBounce", 85.0f }, { "bPitch", 7.0f }, { "bGrain", 40.0f },
    };

    // 7. Phrase Scanner — high gravity + positive Step reads further into the
    //    source on every bounce.
    inline constexpr factory_presets::PresetParam kPhraseScanner[] = {
        { "gravity", 75.0f }, { "retrig", 400.0f }, { "mix", 50.0f },
        { "aOn", 1.0f }, { "aCount", 1.0f }, { "aSpeed", 0.8f }, { "aDirection", 270.0f },
        { "aDirRandom", 15.0f }, { "aBounce", 88.0f }, { "aLifeTime", 10.0f },
        { "aGrain", 110.0f }, { "aStep", 100.0f },
    };

    // 8. Fog Chamber — classic granular fog: circle box, wide spawn spread,
    //    Motion + Spray smearing the read head.
    inline constexpr factory_presets::PresetParam kFogChamber[] = {
        { "boxShape", 5.0f }, { "spin", 0.1f }, { "spawnSpread", 40.0f }, { "tone", 8000.0f },
        { "mix", 55.0f },
        { "aOn", 1.0f }, { "aCount", 4.0f }, { "aTime", 120.0f }, { "aBounce", 90.0f },
        { "aLifeTime", 8.0f }, { "aGrain", 180.0f }, { "aReverse", 20.0f },
        { "aMotion", 100.0f }, { "aSpray", 250.0f },
    };

    inline constexpr factory_presets::Preset kPresets[] = {
        { "First Bounce",   kFirstBounce,   (int) (sizeof (kFirstBounce)   / sizeof (kFirstBounce[0])) },
        { "Tumbler",        kTumbler,       (int) (sizeof (kTumbler)       / sizeof (kTumbler[0])) },
        { "Ball Drop",      kBallDrop,      (int) (sizeof (kBallDrop)      / sizeof (kBallDrop[0])) },
        { "Pinwheel",       kPinwheel,      (int) (sizeof (kPinwheel)      / sizeof (kPinwheel[0])) },
        { "Shimmer Fall",   kShimmerFall,   (int) (sizeof (kShimmerFall)   / sizeof (kShimmerFall[0])) },
        { "Marble Duet",    kMarbleDuet,    (int) (sizeof (kMarbleDuet)    / sizeof (kMarbleDuet[0])) },
        { "Phrase Scanner", kPhraseScanner, (int) (sizeof (kPhraseScanner) / sizeof (kPhraseScanner[0])) },
        { "Fog Chamber",    kFogChamber,    (int) (sizeof (kFogChamber)    / sizeof (kFogChamber[0])) },
    };
    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0])) };
}
