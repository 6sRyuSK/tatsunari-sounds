#pragma once
//
// factory_core/Crossover5.h — a 5-band splitter built from four LR4 crossovers,
// a straightforward extension of Crossover3. Each lower band is passed through
// allpasses matched to the splits above it, so all five bands recombine to a flat
// magnitude (perfect reconstruction up to one overall allpass). Header-only,
// JUCE-independent, allocation-free in process.
//
// Tree (S(f) = an LR4 split at f):
//   [L1,H1]=S(f1)(x)  [L2,H2]=S(f2)(H1)  [L3,H3]=S(f3)(H2)  [L4,H4]=S(f4)(H3)
// Phase-compensated bands (AP(f) = LinkwitzRiley::allpass = its low+high sum, so
// the compensation is bit-for-bit the phase the real split imposes):
//   B1 = AP(f2)AP(f3)AP(f4) L1   B2 = AP(f3)AP(f4) L2
//   B3 = AP(f4) L3               B4 = L4            B5 = H4
// so  sum(B_i) = AP(f1)AP(f2)AP(f3)AP(f4) x, identical to allpass(x) — which is
// what makes a parallel direct path (routed through allpass()) phase-coherent
// with the band sum (no comb filtering when dry and wet are mixed).
//
#include "LinkwitzRiley.h"

#include <algorithm>

namespace factory_core
{
    class Crossover5
    {
    public:
        void prepare (double sampleRate) noexcept { fs = sampleRate; setFrequencies (f[0], f[1], f[2], f[3]); }

        // Clamp to ascending order with a minimum 1/3-octave (ratio 1.26) spacing,
        // then design every split, compensation allpass and the direct allpass at
        // the SAME cutoffs. effectiveCrossoverHz() exposes the clamped values.
        void setFrequencies (double f1, double f2, double f3, double f4) noexcept
        {
            constexpr double kMinRatio = 1.259921; // 2^(1/3), one-third octave
            f[0] = f1;
            f[1] = std::max (f2, f[0] * kMinRatio);
            f[2] = std::max (f3, f[1] * kMinRatio);
            f[3] = std::max (f4, f[2] * kMinRatio);

            split[0].setCutoff (f[0], fs);
            split[1].setCutoff (f[1], fs);
            split[2].setCutoff (f[2], fs);
            split[3].setCutoff (f[3], fs);

            // Band compensation allpasses.
            comp1a.setCutoff (f[1], fs); comp1b.setCutoff (f[2], fs); comp1c.setCutoff (f[3], fs);
            comp2a.setCutoff (f[2], fs); comp2b.setCutoff (f[3], fs);
            comp3a.setCutoff (f[3], fs);

            // Direct-path allpass cascade (matches sum(B_i)).
            dir0.setCutoff (f[0], fs); dir1.setCutoff (f[1], fs);
            dir2.setCutoff (f[2], fs); dir3.setCutoff (f[3], fs);
        }

        void reset() noexcept
        {
            for (auto& s : split) s.reset();
            comp1a.reset(); comp1b.reset(); comp1c.reset();
            comp2a.reset(); comp2b.reset();
            comp3a.reset();
            dir0.reset(); dir1.reset(); dir2.reset(); dir3.reset();
        }

        // Split x into five phase-aligned bands. sum(bands) == allpass(x).
        void process (double x, double (&bands)[5]) noexcept
        {
            double l1, h1, l2, h2, l3, h3, l4, h4;
            split[0].process (x,  l1, h1);
            split[1].process (h1, l2, h2);
            split[2].process (h2, l3, h3);
            split[3].process (h3, l4, h4);

            bands[0] = comp1c.allpass (comp1b.allpass (comp1a.allpass (l1)));
            bands[1] = comp2b.allpass (comp2a.allpass (l2));
            bands[2] = comp3a.allpass (l3);
            bands[3] = l4;
            bands[4] = h4;
        }

        // Direct-path allpass = AP(f1)AP(f2)AP(f3)AP(f4) x (own filter state).
        double allpass (double x) noexcept
        {
            return dir3.allpass (dir2.allpass (dir1.allpass (dir0.allpass (x))));
        }

        double effectiveCrossoverHz (int i) const noexcept { return f[(size_t) std::clamp (i, 0, 3)]; }

    private:
        double fs = 44100.0;
        double f[4] { 130.0, 700.0, 2200.0, 7500.0 };

        LinkwitzRiley split[4];                     // S(f1..f4)
        LinkwitzRiley comp1a, comp1b, comp1c;       // AP(f2),AP(f3),AP(f4) on L1
        LinkwitzRiley comp2a, comp2b;               // AP(f3),AP(f4) on L2
        LinkwitzRiley comp3a;                       // AP(f4) on L3
        LinkwitzRiley dir0, dir1, dir2, dir3;       // direct allpass cascade
    };
} // namespace factory_core
