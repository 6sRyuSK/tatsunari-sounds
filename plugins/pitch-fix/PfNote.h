#pragma once
//
// plugins/pitch-fix/PfNote.h — pure note-quantiser math for Pitch TatFixer
// (P4 stage separation: detection / note / correction). Framework-free,
// header-only, real-time safe. PfCore owns the hysteresis state machine and
// calls these helpers.
//
#include <cmath>

namespace pf_core
{
    // Cents of a MIDI note relative to A4 (note 69).
    inline double noteCents (int note) noexcept { return (note - 69) * 100.0; }

    // Is `note` (MIDI number) in the Key/Scale mask? Chromatic (scale 0) allows
    // every note. Major / Minor use the degree relative to `key` (0=C .. 11=B).
    inline bool noteAllowed (int note, int key, int scale) noexcept
    {
        static constexpr int kMajor[12] = { 1,0,1,0,1,1,0,1,0,1,0,1 };
        static constexpr int kMinor[12] = { 1,0,1,1,0,1,0,1,1,0,1,0 };
        const int pc  = ((note % 12) + 12) % 12;
        const int deg = ((pc - key) % 12 + 12) % 12;
        if (scale == 1) return kMajor[deg] != 0;
        if (scale == 2) return kMinor[deg] != 0;
        return true;
    }

    // Nearest MIDI note whose pitch class is allowed by the key/scale mask.
    inline int nearestAllowedNote (double detCents, int key, int scale) noexcept
    {
        const double noteF = detCents / 100.0 + 69.0;
        const int nn = (int) std::lround (noteF);
        int    best  = nn;
        double bestD = 1.0e9;
        for (int cand = nn - 12; cand <= nn + 12; ++cand)
        {
            if (! noteAllowed (cand, key, scale)) continue;
            const double d = std::abs (noteF - (double) cand);
            if (d < bestD)
            {
                bestD = d;
                best  = cand;
            }
        }
        return best;
    }
} // namespace pf_core
