#pragma once

//
// factory_presets — the shared, JUCE-independent factory-preset data model.
//
// A preset is a small constexpr table of (parameter ID, real value) pairs. The
// value is the parameter's *real* value in its own units (dB, %, Hz, …), i.e.
// BEFORE normalisation — ProgramAdapter converts it to 0..1 via the parameter's
// own NormalisableRange when applying, so the table stays human-readable and
// diff-reviewable.
//
// Deliberately JUCE-free so a headless test (which does not link JUCE) could
// include and structurally inspect a bank, and so the type carries no runtime
// cost. There is NO generation machinery (JSON -> header): the tables are small,
// live next to each plugin as Source/FactoryPresets.h, and are reviewed by hand.
//
namespace factory_presets
{
    // One parameter target within a preset. `paramID` must match a parameter in
    // the plugin's APVTS layout (verified by the plugin's preset_test); `value`
    // is the real (un-normalised) value, clamped into range on application.
    struct PresetParam
    {
        const char* paramID;
        float       value;
    };

    // A single named factory preset. Parameters not listed here are reset to
    // their default by ProgramAdapter, so a preset never leaves "residue" from
    // the previously selected program.
    struct Preset
    {
        const char*        name;
        const PresetParam* params;
        int                numParams;
    };

    // The whole bank for one plugin. Program 0 ("Init") is synthesised by
    // ProgramAdapter (every parameter to its default) and is NOT stored here, so
    // an empty bank ({ nullptr, 0 }) still yields a working Init-only program list.
    struct PresetBank
    {
        const Preset* presets;
        int           numPresets;
    };
}
