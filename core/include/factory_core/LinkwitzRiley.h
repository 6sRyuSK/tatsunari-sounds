#pragma once
//
// factory_core/LinkwitzRiley.h — a 4th-order Linkwitz-Riley crossover (a pair of
// cascaded Butterworth biquads for each of the low and high outputs). For LR4
// the low and high bands are in phase and sum to an allpass (flat magnitude),
// which makes phase-aligned multiband reconstruction possible. Header-only,
// JUCE-independent, allocation-free in process.
//
#include "Biquad.h"
#include "Filters.h"

namespace factory_core
{
    class LinkwitzRiley
    {
    public:
        void setCutoff (double freqHz, double sampleRate) noexcept
        {
            const auto lp = designFilter (BandType::LowPass,  freqHz, 0.0, 0.70710678118, sampleRate);
            const auto hp = designFilter (BandType::HighPass, freqHz, 0.0, 0.70710678118, sampleRate);
            lp1.setCoeffs (lp); lp2.setCoeffs (lp);
            hp1.setCoeffs (hp); hp2.setCoeffs (hp);
        }

        void reset() noexcept
        {
            lp1.reset(); lp2.reset();
            hp1.reset(); hp2.reset();
        }

        void process (double x, double& low, double& high) noexcept
        {
            low  = lp2.processSample (lp1.processSample (x));
            high = hp2.processSample (hp1.processSample (x));
        }

        // Allpass = low + high (used to phase-align another band to this split).
        double allpass (double x) noexcept
        {
            double lo, hi;
            process (x, lo, hi);
            return lo + hi;
        }

    private:
        Biquad lp1, lp2, hp1, hp2;
    };
} // namespace factory_core
