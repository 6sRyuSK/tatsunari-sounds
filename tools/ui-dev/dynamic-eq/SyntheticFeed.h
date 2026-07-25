#pragma once

#include "DeqModels.h"
#include "factory_params/ParamStore.h"

#include <algorithm>
#include <cmath>
#include <string>

class SyntheticDeqFeed final : public deq_ui::DeqFeed
{
public:
    explicit SyntheticDeqFeed (factory_params::ParamStore& store) : store_ (store) {}

    void copyAnalyzerSamples (float* dst, int n, bool post) const override
    {
        if (! dst || n <= 0) return;
        constexpr double sr = 48000.0;
        constexpr double twoPi = 6.28318530717958647692;
        const double control = phaseSamples_ / 4096.0;
        const double midHz = 440.0 + 90.0 * std::sin (control * 1.7);
        const double presenceHz = 2400.0 + 900.0 * (0.5 + 0.5 * std::sin (control));
        for (int i = 0; i < n; ++i)
        {
            const double t = ((double) i + phaseSamples_) / sr;
            float sample = 0.31f * (float) std::sin (twoPi * 110.0 * t)
                         + 0.20f * (float) std::sin (twoPi * midHz * t + 0.3)
                         + 0.13f * (float) std::sin (twoPi * presenceHz * t + 0.8)
                         + 0.08f * (float) std::sin (twoPi * 7200.0 * t + 1.2);
            if (post)
                sample = 0.82f * sample + 0.035f * (float) std::sin (twoPi * 1200.0 * t);
            dst[i] = std::clamp (sample, -0.98f, 0.98f);
        }
    }

    float liveGainDb (int band) const override
    {
        const int gain = store_.indexOf (idFor (band, "gain"));
        const int dyn = store_.indexOf (idFor (band, "dyn"));
        const int range = store_.indexOf (idFor (band, "rng"));
        if (gain < 0) return 0.0f;
        const float base = store_.value (gain);
        if (dyn < 0 || store_.value (dyn) < 0.5f || range < 0) return base;
        const float movement = (0.35f + 0.25f * (float) std::sin (phaseSamples_ * 0.001));
        return base + store_.value (range) * movement;
    }

    double sampleRate() const override { return 48000.0; }
    int numBands() const override { return 24; }

    void setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (frozen_) phaseSamples_ = 4096.0;
    }
    void setPhase (double phase) { phaseSamples_ = phase * 4096.0; }
    void advance() { if (! frozen_) phaseSamples_ += 137.0; }

private:
    static std::string idFor (int band, const char* suffix)
    {
        return "b" + std::to_string (band) + "_" + suffix;
    }

    factory_params::ParamStore& store_;
    double phaseSamples_ = 4096.0;
    bool frozen_ = false;
};
