#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

//
// factory_params::ParamDesc — the JUCE-free declarative description of one plugin
// parameter. A plugin declares its whole surface as a std::vector<ParamDesc> (see
// e.g. resonance-suppressor's Source/Params.h); the JUCE APVTS layout is then
// GENERATED from that table by the JUCE-side ApvtsAdapter.h.
//
// Header-only and deliberately JUCE-free (that is the point of this module): the
// table can be built and inspected in a headless test that never links JUCE. Only
// the JUCE-side ApvtsAdapter.h pulls JUCE in — the same split presets/ uses
// (PresetBank.h JUCE-free, ProgramAdapter.h JUCE-side).
//
namespace factory_params
{
    // FNV-1a 32-bit hash. `uid` (below) is the stable, CLAP-era parameter id, so
    // this function's output for a given id string is a STABILITY CONTRACT — it
    // must never change (golden values are pinned in params_test). Standard
    // FNV-1a: offset basis 0x811c9dc5, prime 0x01000193, hashing raw bytes.
    constexpr std::uint32_t fnv1a32 (std::string_view s) noexcept
    {
        std::uint32_t h = 0x811c9dc5u;                                   // FNV offset basis
        for (char c : s)
        {
            h ^= static_cast<std::uint32_t> (static_cast<unsigned char> (c));
            h *= 0x01000193u;                                           // FNV prime
        }
        return h;
    }

    enum class ParamType { Float, Bool, Choice };

    // flags bit constants.
    //   kFlagBypass         — the host bypass parameter (getBypassParameter()).
    //   kFlagLegacyJuceOnly — registered in the JUCE/APVTS build for automation-lane
    //                         + old-session compatibility, but excluded from the
    //                         future CLAP surface (its value is no longer consumed).
    inline constexpr std::uint32_t kFlagBypass         = 1u << 0;
    inline constexpr std::uint32_t kFlagLegacyJuceOnly = 1u << 1;

    struct ParamDesc
    {
        std::string   id;                       // stable, snake/lower English id
        std::uint32_t uid = 0;                  // fnv1a32(id) — CLAP-era stable id
        std::string   name;                     // host display name
        ParamType     type = ParamType::Float;

        // Range (real units). interval 0 == continuous. skewCentre 0 == linear,
        // otherwise the value convertFrom0to1(0.5) maps to (JUCE setSkewForCentre).
        float minValue   = 0.0f;
        float maxValue   = 1.0f;
        float interval   = 0.0f;
        float skewCentre = 0.0f;

        // Default in REAL units (Bool: 0/1; Choice: the index).
        float defaultValue = 0.0f;

        // Verbatim JUCE label (incl. any leading space, e.g. " %"); empty = none.
        std::string unit;

        // Choice labels (empty for Float/Bool).
        std::vector<std::string> choices;

        int           versionHint = 1;          // JUCE ParameterID version hint
        std::uint32_t flags = 0;                 // kFlag* bitset
    };

    // --- terse constructors (the RS table is 64 entries — keep it readable) -----

    // Float. skewCentre 0 = linear; interval 0 = continuous.
    inline ParamDesc floatParam (std::string id, std::string name,
                                 float minValue, float maxValue, float interval,
                                 float defaultValue, std::string unit, int versionHint,
                                 float skewCentre = 0.0f, std::uint32_t flags = 0)
    {
        ParamDesc d;
        d.id         = std::move (id);
        d.name       = std::move (name);
        d.type       = ParamType::Float;
        d.minValue   = minValue;
        d.maxValue   = maxValue;
        d.interval   = interval;
        d.skewCentre = skewCentre;
        d.defaultValue = defaultValue;
        d.unit       = std::move (unit);
        d.versionHint = versionHint;
        d.flags      = flags;
        d.uid        = fnv1a32 (d.id);
        return d;
    }

    inline ParamDesc boolParam (std::string id, std::string name,
                                bool defaultValue, int versionHint, std::uint32_t flags = 0)
    {
        ParamDesc d;
        d.id       = std::move (id);
        d.name     = std::move (name);
        d.type     = ParamType::Bool;
        d.minValue = 0.0f;
        d.maxValue = 1.0f;
        d.interval = 1.0f;
        d.defaultValue = defaultValue ? 1.0f : 0.0f;
        d.versionHint  = versionHint;
        d.flags    = flags;
        d.uid      = fnv1a32 (d.id);
        return d;
    }

    inline ParamDesc choiceParam (std::string id, std::string name,
                                  std::vector<std::string> choices, int defaultIndex,
                                  int versionHint, std::uint32_t flags = 0)
    {
        ParamDesc d;
        d.id       = std::move (id);
        d.name     = std::move (name);
        d.type     = ParamType::Choice;
        d.choices  = std::move (choices);
        d.minValue = 0.0f;
        d.maxValue = static_cast<float> (d.choices.size()) - 1.0f; // mirrors AudioParameterChoice range end
        d.interval = 1.0f;
        d.defaultValue = static_cast<float> (defaultIndex);
        d.versionHint  = versionHint;
        d.flags    = flags;
        d.uid      = fnv1a32 (d.id);
        return d;
    }
}
