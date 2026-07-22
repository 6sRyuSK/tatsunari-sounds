#pragma once

#include <juce_core/juce_core.h>

//
// Shimmer pitch-shift voice table — the single source of truth for the two
// pitch selectors. Each option's display name and its semitone offset live side
// by side here so the parameter layout (choice items), the semitone lookup
// (ShimmerReverbAudioProcessor::pitchSemis) and the editor's combo boxes cannot
// drift apart — the list used to be hand-duplicated in all three places.
//
// Order and strings are state-visible (they index the "pitcha"/"pitchb" choice
// params), so treat this table as append-only: adding an option is fine, but
// reordering or renaming breaks preset/session compatibility.
//
namespace shimmer_pitch
{
    struct PitchOption { const char* name; double semitones; };

    inline constexpr PitchOption kOptions[] = {
        { "+12",  12.0 },
        { "+7",    7.0 },
        { "+5",    5.0 },
        { "+19",  19.0 },
        { "-12", -12.0 },
    };
    inline constexpr int kNumOptions = (int) (sizeof (kOptions) / sizeof (kOptions[0]));

    // Choice-parameter item names, in table order (for the layout and the editor).
    inline juce::StringArray names()
    {
        juce::StringArray a;
        for (const auto& o : kOptions)
            a.add (o.name);
        return a;
    }

    // Semitone offset for a choice index. Out-of-range indices fall back to the
    // last entry, preserving the historical default (-12).
    inline double semitones (int index) noexcept
    {
        if (index < 0 || index >= kNumOptions)
            index = kNumOptions - 1;
        return kOptions[index].semitones;
    }
}
