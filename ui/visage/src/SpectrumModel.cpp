#include "factory_ui_visage/SpectrumModel.h"

#include "factory_core/StftResolution.h"

#include <algorithm>
#include <cmath>

namespace factory_ui_visage
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // gain → dB with a floor (mirrors juce::Decibels::gainToDecibels).
        float gainToDb (float gain, float floorDb)
        {
            return gain > 0.0f ? std::max (floorDb, 20.0f * std::log10 (gain)) : floorDb;
        }
    } // namespace

    float LogFreqAxis::freqToX (float f) const
    {
        const float clamped = std::min (kMaxHz, std::max (kMinHz, f));
        const float t = std::log (clamped / kMinHz) / std::log (kSpan);
        return x + t * width;
    }

    float LogFreqAxis::xToFreq (float px) const
    {
        const float t = (px - x) / std::max (1.0f, width);
        return kMinHz * std::pow (kSpan, t);
    }

    float VerticalAxis::toY (float v) const
    {
        const float lo = std::min (topValue, bottomValue);
        const float hi = std::max (topValue, bottomValue);
        const float c  = std::min (hi, std::max (lo, v));
        // jmap(c, topValue, bottomValue, y, y+height)
        const float denom = (bottomValue - topValue);
        const float t = denom != 0.0f ? (c - topValue) / denom : 0.0f;
        return y + t * height;
    }

    void SpectrumModel::setup (int fftOrder)
    {
        if (fftOrder == order_ && size_ == (1 << fftOrder))
            return;

        order_ = fftOrder;
        size_  = 1 << fftOrder;
        fft_.prepare (order_);

        // Periodic Hann: w[n] = 0.5 (1 - cos(2π n / N)). Its window sum is exactly
        // N/2, so the coherent gain is 0.5 (used to reason about the peak level).
        window_.resize ((std::size_t) size_);
        for (int i = 0; i < size_; ++i)
            window_[(std::size_t) i] = 0.5 * (1.0 - std::cos (2.0 * kPi * i / size_));

        scratch_.assign ((std::size_t) size_, std::complex<double> (0.0, 0.0));
        ring_.assign ((std::size_t) size_, 0.0f);
        writePos_ = 0;

        smoothed_.assign ((std::size_t) (size_ / 2), kFloorInit);
        peak_.assign ((std::size_t) (size_ / 2), kFloorInit);
    }

    void SpectrumModel::setOrderForSampleRate (double sampleRate)
    {
        setup (factory_core::fftOrderForSampleRate (sampleRate));
    }

    void SpectrumModel::reset()
    {
        std::fill (ring_.begin(), ring_.end(), 0.0f);
        writePos_ = 0;
        std::fill (smoothed_.begin(), smoothed_.end(), kFloorInit);
        std::fill (peak_.begin(), peak_.end(), kFloorInit);
    }

    void SpectrumModel::writeSamples (const float* data, int numSamples)
    {
        if (size_ == 0 || data == nullptr)
            return;
        for (int i = 0; i < numSamples; ++i)
        {
            ring_[(std::size_t) writePos_] = data[i];
            writePos_ = (writePos_ + 1) % size_;
        }
    }

    void SpectrumModel::update (double sampleRate, const Options& opt)
    {
        if (size_ == 0)
            return;

        const int N = size_;

        // Pull the most recent N samples in chronological order (oldest → newest):
        // the ring's write cursor points at the oldest slot, so read from there.
        for (int i = 0; i < N; ++i)
        {
            const float s = ring_[(std::size_t) ((writePos_ + i) % N)];
            scratch_[(std::size_t) i] = std::complex<double> (s * window_[(std::size_t) i], 0.0);
        }

        fft_.forward (scratch_.data());

        const double norm = 1.0 / (N * 0.5); // JUCE house normalisation (== 1/(N/2))
        for (int bin = 0; bin < N / 2; ++bin)
        {
            const float mag  = (float) (std::abs (scratch_[(std::size_t) bin]) * norm);
            float       inst = gainToDb (mag, opt.floorDb);
            if (opt.tiltDbPerOct != 0.0f)
            {
                const float freq = (float) ((double) bin * sampleRate / N);
                inst += opt.tiltDbPerOct * std::log2 (std::max (1.0f, freq) / 1000.0f);
            }

            float& s = smoothed_[(std::size_t) bin];
            s = (inst > s) ? inst : s + (inst - s) * opt.smoothing; // fast up, slow down
            float& p = peak_[(std::size_t) bin];
            p = (s > p) ? s : std::max (s, p - opt.peakFallDb);     // hold then fall
        }
    }
}
