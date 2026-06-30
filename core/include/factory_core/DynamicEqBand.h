#pragma once
//
// factory_core/DynamicEqBand.h — one band of the parametric EQ: a biquad filter
// (any BandType) whose gain can optionally be modulated by the signal level in
// the band (per-band dynamic EQ). Detection is a band-pass at the band centre
// plus a peak envelope follower, stereo-linked. Header-only, headless-testable.
//
// High/Low-pass bands support a selectable slope (12..96 dB/oct) implemented as
// a cascade of up to kMaxStages Butterworth sections. Gain-bearing bands
// (bell / shelf) stay a single section and are the only ones whose gain the
// dynamics path modulates.
//
// Coefficient recomputation for the dynamic path is decimated to a control rate;
// nothing here allocates, so it is safe on the audio thread.
//
#include "Biquad.h"
#include "Filters.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace factory_core
{
    class DynamicEqBand
    {
    public:
        static constexpr int kMaxStages = 8; // 8 * 12 = 96 dB/oct

        // --- configuration (set by the host, then call updateCoefficients) ---
        void setEnabled    (bool e)        noexcept { enabled = e; }
        void setType       (BandType t)    noexcept { type = t; }
        void setFrequency  (double f)      noexcept { freqHz = f; }
        void setGainDb     (double g)      noexcept { staticGainDb = g; }
        void setQ          (double q)      noexcept { Q = q; }
        // Slope for HP/LP only: 12 * numStages dB/oct, numStages in [1, kMaxStages].
        void setSlopeStages (int n)        noexcept { slopeStages = std::clamp (n, 1, kMaxStages); }
        void setKnee       (double kDb)    noexcept { kneeDb = std::max (0.0, kDb); }
        void setDynamics   (bool on, double thrDb, double rangeDb) noexcept
        {
            dynamicsOn = on; thresholdDb = thrDb; dynRangeDb = rangeDb;
        }
        void setDynamicsTimes (double atkMs, double relMs) noexcept
        {
            attackMs = atkMs; releaseMs = relMs;
        }

        void prepare (double sampleRate) noexcept
        {
            fs = sampleRate;
            updateCoefficients();
            reset();
        }

        void reset() noexcept
        {
            for (auto& f : filterL) f.reset();
            for (auto& f : filterR) f.reset();
            detL.reset(); detR.reset();
            env = 0.0;
            decimCounter = 0;
            effectiveGainDb = staticGainDb;
        }

        static bool isGainBearing (BandType t) noexcept
        {
            return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf;
        }

        // Recompute static filter + detector coefficients from the current
        // config. Call once per block after setting parameters.
        void updateCoefficients() noexcept
        {
            if (type == BandType::HighPass || type == BandType::LowPass)
            {
                numStages = slopeStages;
                for (int k = 0; k < numStages; ++k)
                {
                    const auto cc = designHpLpStage (type, freqHz, Q, k, numStages, fs);
                    filterL[(size_t) k].setCoeffs (cc);
                    filterR[(size_t) k].setCoeffs (cc);
                }
                effectiveGainDb = 0.0; // gain is meaningless for cut filters
            }
            else
            {
                numStages = 1;
                staticCoeffs = designFilter (type, freqHz, staticGainDb, Q, fs);
                if (! dynamicsOn)
                {
                    filterL[0].setCoeffs (staticCoeffs);
                    filterR[0].setCoeffs (staticCoeffs);
                    effectiveGainDb = staticGainDb;
                }
            }

            // Detector (only the dynamic, gain-bearing path uses it).
            detectorCoeffs = designBandpass (freqHz, std::max (0.5, Q), fs);
            detL.setCoeffs (detectorCoeffs);
            detR.setCoeffs (detectorCoeffs);

            attackCoeff  = coeffForMs (attackMs);
            releaseCoeff = coeffForMs (releaseMs);
        }

        // Dynamic gain offset (dB) added to the static gain: zero below the
        // threshold, ramping to dynRangeDb over a fixed span above it. A soft
        // knee of width kneeDb (dB) rounds the lower corner; kneeDb == 0 is the
        // hard-knee piecewise-linear ramp. Pure.
        static double dynamicOffsetDb (double levelDb, double thresholdDb, double dynRangeDb,
                                       double kneeDb = 0.0) noexcept
        {
            constexpr double spanDb = 24.0;
            const double slope = dynRangeDb / spanDb;
            const double d = levelDb - thresholdDb;

            double lin;
            if (kneeDb > 1.0e-9 && d > -0.5 * kneeDb && d < 0.5 * kneeDb)
            {
                const double e = d + 0.5 * kneeDb;       // 0 .. kneeDb
                lin = slope * (e * e) / (2.0 * kneeDb);  // C1 join to the line
            }
            else
            {
                lin = slope * std::max (0.0, d);
            }

            // Top corner stays hard: clamp to the full range (sign-aware).
            if (dynRangeDb >= 0.0) return std::clamp (lin, 0.0, dynRangeDb);
            return std::clamp (lin, dynRangeDb, 0.0);
        }

        void processStereo (double& l, double& r) noexcept
        {
            if (! enabled)
                return;

            if (dynamicsOn && isGainBearing (type))
            {
                const double dl = detL.processSample (l);
                const double dr = detR.processSample (r);
                const double d  = std::max (std::abs (dl), std::abs (dr));

                const double c = (d > env) ? attackCoeff : releaseCoeff;
                env = c * env + (1.0 - c) * d;

                if (--decimCounter <= 0)
                {
                    decimCounter = kDecimation;
                    const double levelDb = 20.0 * std::log10 (std::max (env, 1.0e-12));
                    effectiveGainDb = staticGainDb + dynamicOffsetDb (levelDb, thresholdDb, dynRangeDb, kneeDb);
                    const auto cc = designFilter (type, freqHz, effectiveGainDb, Q, fs);
                    filterL[0].setCoeffs (cc);
                    filterR[0].setCoeffs (cc);
                }

                l = filterL[0].processSample (l);
                r = filterR[0].processSample (r);
                return;
            }

            // Static path: gain-bearing static section, or the HP/LP cascade.
            for (int k = 0; k < numStages; ++k)
            {
                l = filterL[(size_t) k].processSample (l);
                r = filterR[(size_t) k].processSample (r);
            }
        }

        double currentGainDb() const noexcept { return effectiveGainDb; }
        bool   isEnabled()     const noexcept { return enabled; }

    private:
        static BiquadCoeffs designBandpass (double f0, double Q, double Fs) noexcept
        {
            constexpr double pi = 3.14159265358979323846;
            const double w0 = 2.0 * pi * f0 / Fs;
            const double cw = std::cos (w0);
            const double alpha = std::sin (w0) / (2.0 * Q);
            const double a0 = 1.0 + alpha;
            BiquadCoeffs c;
            c.b0 =  alpha / a0;
            c.b1 =  0.0;
            c.b2 = -alpha / a0;
            c.a1 = (-2.0 * cw) / a0;
            c.a2 = (1.0 - alpha) / a0;
            return c;
        }

        double coeffForMs (double ms) const noexcept
        {
            const double t = ms * 0.001;
            if (t <= 0.0 || fs <= 0.0) return 0.0;
            return std::exp (-1.0 / (t * fs));
        }

        static constexpr int kDecimation = 32; // control-rate coeff updates

        // config
        bool     enabled     { true };
        BandType type        { BandType::Bell };
        double   freqHz      { 1000.0 };
        double   staticGainDb{ 0.0 };
        double   Q           { 0.707 };
        int      slopeStages { 1 };
        double   kneeDb      { 0.0 };
        bool     dynamicsOn  { false };
        double   thresholdDb { -24.0 };
        double   dynRangeDb  { 0.0 };
        double   attackMs    { 10.0 };
        double   releaseMs   { 120.0 };

        // runtime
        double fs { 44100.0 };
        BiquadCoeffs staticCoeffs {};
        BiquadCoeffs detectorCoeffs {};
        std::array<Biquad, kMaxStages> filterL, filterR;
        int    numStages { 1 };
        Biquad detL, detR;
        double env { 0.0 };
        double attackCoeff { 0.0 }, releaseCoeff { 0.0 };
        int    decimCounter { 0 };
        double effectiveGainDb { 0.0 };
    };
} // namespace factory_core
