#pragma once
//
// factory_core/Compressor.h — shared DSP primitive: a feed-forward, log-domain
// dynamics compressor with a static gain computer (threshold / ratio / optional
// soft knee) and decoupled attack/release ballistics. Stereo detection is
// linked (one gain from max(|L|,|R|)). Header-only, JUCE-independent, headless-
// testable. No allocation in the audio path.
//
#include <algorithm>
#include <cmath>

namespace factory_core
{
    class Compressor
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            fs = sampleRate;
            updateCoeffs();
            reset();
        }

        void reset() noexcept { gainStateDb = 0.0; }

        void setThresholdDb (double t) noexcept { thresholdDb = t; }
        void setRatio       (double r) noexcept { ratio = r; }
        void setKneeDb      (double k) noexcept { kneeDb = k; }
        void setMakeupDb    (double m) noexcept { makeupDb = m; }
        void setAttackMs    (double a) noexcept { attackMs = a; updateCoeffs(); }
        void setReleaseMs   (double r) noexcept { releaseMs = r; updateCoeffs(); }

        // Static gain computer: output level (dB) for a given input level (dB).
        // Pure (no state) — the independent reference for the curve.
        double staticOutputDb (double inDb) const noexcept
        {
            const double half = kneeDb * 0.5;
            if (kneeDb > 0.0 && inDb > thresholdDb - half && inDb < thresholdDb + half)
            {
                // Quadratic soft knee (Reiss/Giannoulis).
                const double x = inDb - thresholdDb + half;
                return inDb + (1.0 / ratio - 1.0) * (x * x) / (2.0 * kneeDb);
            }
            if (inDb <= thresholdDb)
                return inDb;
            return thresholdDb + (inDb - thresholdDb) / ratio;
        }

        // Static gain reduction in dB (<= 0), excluding makeup.
        double staticGainDb (double inDb) const noexcept { return staticOutputDb (inDb) - inDb; }

        // Feed one detector sample (linear); returns the linear gain to apply
        // (including makeup), advancing the attack/release state.
        double processDetector (double detectorLinear) noexcept
        {
            const double inDb     = 20.0 * std::log10 (std::max (detectorLinear, 1.0e-12));
            const double targetGr = staticGainDb (inDb); // <= 0

            // Decoupled: attack when reduction deepens, release when it eases.
            const double coeff = (targetGr <= gainStateDb) ? attackCoeff : releaseCoeff;
            gainStateDb = coeff * gainStateDb + (1.0 - coeff) * targetGr;

            return std::pow (10.0, (gainStateDb + makeupDb) / 20.0);
        }

        // Stereo-linked: detect on max(|l|,|r|), apply one gain to both.
        void processStereoSample (double& l, double& r) noexcept
        {
            const double g = processDetector (std::max (std::abs (l), std::abs (r)));
            l *= g;
            r *= g;
        }

        // Current gain reduction in dB (<= 0), for metering.
        double currentGainReductionDb() const noexcept { return gainStateDb; }

    private:
        void updateCoeffs() noexcept
        {
            attackCoeff  = coeffForMs (attackMs);
            releaseCoeff = coeffForMs (releaseMs);
        }

        double coeffForMs (double ms) const noexcept
        {
            const double t = ms * 0.001;
            if (t <= 0.0 || fs <= 0.0)
                return 0.0;
            return std::exp (-1.0 / (t * fs));
        }

        double fs          { 44100.0 };
        double thresholdDb { 0.0 };
        double ratio       { 4.0 };
        double kneeDb      { 0.0 };
        double makeupDb    { 0.0 };
        double attackMs    { 10.0 };
        double releaseMs   { 100.0 };
        double attackCoeff { 0.0 };
        double releaseCoeff{ 0.0 };
        double gainStateDb { 0.0 };
    };
} // namespace factory_core
