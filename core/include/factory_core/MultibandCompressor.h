#pragma once
//
// factory_core/MultibandCompressor.h — a 3-band compressor: each channel is
// split by a Crossover3, the matching bands are compressed stereo-linked by a
// factory_core::Compressor, then recombined, with a dry/wet mix. Header-only,
// JUCE-independent, allocation-free in process.
//
// At ratio 1 the bands recombine to a flat magnitude (the crossover is
// reconstruction-flat), so the processor is transparent when not compressing.
//
#include "Compressor.h"
#include "Crossover3.h"

#include <array>

namespace factory_core
{
    class MultibandCompressor
    {
    public:
        static constexpr int kBands = 3;

        void prepare (double sampleRate) noexcept
        {
            xoverL.prepare (sampleRate);
            xoverR.prepare (sampleRate);
            for (auto& c : comps) c.prepare (sampleRate);
        }

        void reset() noexcept
        {
            xoverL.reset();
            xoverR.reset();
            for (auto& c : comps) c.reset();
        }

        void setCrossover (double lowMidHz, double midHighHz) noexcept
        {
            xoverL.setFrequencies (lowMidHz, midHighHz);
            xoverR.setFrequencies (lowMidHz, midHighHz);
        }

        Compressor& band (int i) noexcept { return comps[(size_t) i]; }
        void setMix (double m) noexcept { mix = std::clamp (m, 0.0, 1.0); }

        void processStereo (double& l, double& r) noexcept
        {
            const double dryL = l, dryR = r;

            std::array<double, kBands> bl {}, br {};
            xoverL.process (l, bl[0], bl[1], bl[2]);
            xoverR.process (r, br[0], br[1], br[2]);

            for (int i = 0; i < kBands; ++i)
                comps[(size_t) i].processStereoSample (bl[(size_t) i], br[(size_t) i]);

            const double wetL = bl[0] + bl[1] + bl[2];
            const double wetR = br[0] + br[1] + br[2];

            l = (1.0 - mix) * dryL + mix * wetL;
            r = (1.0 - mix) * dryR + mix * wetR;
        }

        double bandGainReductionDb (int i) const noexcept { return comps[(size_t) i].currentGainReductionDb(); }

    private:
        Crossover3 xoverL, xoverR;
        std::array<Compressor, kBands> comps;
        double mix = 1.0;
    };
} // namespace factory_core
