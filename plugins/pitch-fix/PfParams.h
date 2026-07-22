#pragma once
//
// plugins/pitch-fix/PfParams.h — the declarative parameter table of Pitch
// TatFixer (single source of truth for the CLAP surface, the shell state
// codec and the editor). JUCE-free; built on factory_params::ParamDesc.
//
// EXPERIMENTAL-PHASE RANGES: every range is the typical production range
// widened by roughly +50% (per the product brief), so voicing work can explore
// beyond the usual bounds. Tightening later is a state-compatible change
// (values are stored in real units and clamped on load).
//
#include "factory_params/ParamDesc.h"

#include <vector>

namespace pitch_fix_params
{
    inline std::vector<factory_params::ParamDesc> buildPfParams()
    {
        using namespace factory_params;
        std::vector<ParamDesc> p;
        p.reserve (14);

        // -- correction behaviour -------------------------------------------
        p.push_back (floatParam ("amount",     "Correction Amount", 0.0f, 150.0f, 0.0f, 100.0f, " %", 1));
        p.push_back (floatParam ("retune",     "Retune Speed",      0.0f, 600.0f, 0.0f,  80.0f, " ms", 1, 120.0f));
        p.push_back (floatParam ("glide",      "Note Glide",        0.0f, 750.0f, 0.0f,  60.0f, " ms", 1, 150.0f));
        p.push_back (floatParam ("tolerance",  "Tolerance",         0.0f,  75.0f, 0.0f,  12.0f, " ct", 1));
        p.push_back (floatParam ("hysteresis", "Note Hysteresis",   0.0f,  75.0f, 0.0f,  18.0f, " ct", 1));

        // -- detector --------------------------------------------------------
        p.push_back (floatParam ("min_pitch",  "Min Pitch",   25.0f,  500.0f, 0.0f,   75.0f, " Hz", 1, 110.0f));
        p.push_back (floatParam ("max_pitch",  "Max Pitch",  200.0f, 4000.0f, 0.0f, 1300.0f, " Hz", 1, 900.0f));
        p.push_back (floatParam ("threshold",  "Detect Threshold", 50.0f, 99.0f, 0.0f, 86.0f, " %", 1));

        // -- performance / musical context ------------------------------------
        p.push_back (choiceParam ("buffer", "Buffer",
                                  { "Realtime", "Fast", "Normal", "Quality" }, 2, 1));
        p.push_back (choiceParam ("key", "Key",
                                  { "C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B" }, 0, 1));
        p.push_back (choiceParam ("scale", "Scale",
                                  { "Chromatic", "Major", "Minor" }, 0, 1));
        p.push_back (floatParam ("a4", "A4 Reference", 400.0f, 480.0f, 0.0f, 440.0f, " Hz", 1));

        // -- output ------------------------------------------------------------
        p.push_back (floatParam ("mix", "Mix",     0.0f, 100.0f, 0.0f, 100.0f, " %", 1));
        p.push_back (floatParam ("out", "Output", -24.0f, 24.0f, 0.0f,   0.0f, " dB", 1));

        return p;
    }
} // namespace pitch_fix_params
