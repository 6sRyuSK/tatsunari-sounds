#pragma once
//
// factory_core/ResamplerLatency.h — deterministic host<->model round-trip latency
// (in host samples) for the NAM section's arbitrary-ratio resampler. Kept as a
// pure function in core/ so the sample-rate matrix can unit-test it without JUCE.
//
// The NAM section runs at a fixed internal rate (modelRate, ~48 kHz). When the host
// runs at a different rate the signal is resampled host->model before the models and
// model->host afterwards, with an interpolator whose group delay is `baseLatency`
// input samples per stage. The down stage adds `baseLatency` host samples; the up
// stage adds `baseLatency` model samples == baseLatency * hostRate / modelRate host
// samples. When hostRate == modelRate the resampler is bypassed (0 latency). The
// wrapper delays the dry path by exactly this integer so wet and dry stay aligned.
//
#include <cmath>

namespace factory_core
{
    inline int resamplerRoundTripLatency (double hostRate, double modelRate, int baseLatency) noexcept
    {
        if (hostRate <= 0.0 || modelRate <= 0.0 || baseLatency <= 0)
            return 0;
        if (hostRate == modelRate)
            return 0;                               // resampler bypassed
        const double up = (double) baseLatency * hostRate / modelRate;
        return (int) std::lround ((double) baseLatency + up);
    }
} // namespace factory_core
