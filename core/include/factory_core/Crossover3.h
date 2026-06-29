#pragma once
//
// factory_core/Crossover3.h — a 3-band splitter built from two LR4 crossovers.
// The low band is passed through an allpass matched to the upper split so all
// three bands recombine to a flat magnitude (perfect reconstruction up to an
// overall allpass). Header-only, JUCE-independent, allocation-free.
//
#include "LinkwitzRiley.h"

namespace factory_core
{
    class Crossover3
    {
    public:
        void prepare (double sampleRate) noexcept { fs = sampleRate; }

        void setFrequencies (double lowMidHz, double midHighHz) noexcept
        {
            splitLow.setCutoff (lowMidHz, fs);
            splitHigh.setCutoff (midHighHz, fs);
            alignLow.setCutoff (midHighHz, fs);
        }

        void reset() noexcept
        {
            splitLow.reset();
            splitHigh.reset();
            alignLow.reset();
        }

        void process (double x, double& low, double& mid, double& high) noexcept
        {
            double lo, hi;
            splitLow.process (x, lo, hi);
            splitHigh.process (hi, mid, high);
            low = alignLow.allpass (lo); // phase-align the low band to the high split
        }

    private:
        double fs = 44100.0;
        LinkwitzRiley splitLow;   // low | (mid+high)
        LinkwitzRiley splitHigh;  // mid | high
        LinkwitzRiley alignLow;   // allpass at the mid/high split, applied to low
    };
} // namespace factory_core
