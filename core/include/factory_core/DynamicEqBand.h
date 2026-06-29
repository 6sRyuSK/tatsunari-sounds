#pragma once
//
// factory_core/DynamicEqBand.h — one band of the parametric EQ: a biquad filter
// (any BandType) whose gain can optionally be modulated by the signal level in
// the band (per-band dynamic EQ). Detection is a band-pass at the band centre
// plus a peak envelope follower, stereo-linked. Header-only, headless-testable.
//
// Coefficient recomputation for the dynamic path is decimated to a control rate;
// nothing here allocates, so it is safe on the audio thread.
//
#include "Biquad.h"
#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace factory_core
{
    class DynamicEqBand
    {
    public:
        // --- configuration (set by the host, then call updateCoefficients) ---
        void setEnabled   (bool e)        noexcept { enabled = e; }
        void setType      (BandType t)    noexcept { type = t; }
        void setFrequency (double f)      noexcept { freqHz = f; }
        void setGainDb    (double g)      noexcept { staticGainDb = g; }
        void setQ         (double q)      noexcept { Q = q; }
        void setDynamics  (bool on, double thrDb, double rangeDb) noexcept
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
            filterL.reset(); filterR.reset();
            detL.reset();     detR.reset();
            env = 0.0;
            decimCounter = 0;
            effectiveGainDb = staticGainDb;
        }

        // Recompute static filter + detector coefficients from the current
        // config. Call once per block after setting parameters.
        void updateCoefficients() noexcept
        {
            staticCoeffs = designFilter (type, freqHz, staticGainDb, Q, fs);
            if (! dynamicsOn)
            {
                filterL.setCoeffs (staticCoeffs);
                filterR.setCoeffs (staticCoeffs);
                effectiveGainDb = staticGainDb;
            }
            detectorCoeffs = designBandpass (freqHz, std::max (0.5, Q), fs);
            detL.setCoeffs (detectorCoeffs);
            detR.setCoeffs (detectorCoeffs);

            const double atkC = coeffForMs (attackMs);
            const double relC = coeffForMs (releaseMs);
            attackCoeff = atkC; releaseCoeff = relC;
        }

        // Dynamic gain offset (dB) added to the static gain: zero below the
        // threshold, ramping to dynRangeDb over a fixed span above it. Pure.
        static double dynamicOffsetDb (double levelDb, double thresholdDb, double dynRangeDb) noexcept
        {
            constexpr double spanDb = 24.0;
            const double t = std::clamp ((levelDb - thresholdDb) / spanDb, 0.0, 1.0);
            return dynRangeDb * t;
        }

        void processStereo (double& l, double& r) noexcept
        {
            if (! enabled)
                return;

            if (dynamicsOn)
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
                    effectiveGainDb = staticGainDb + dynamicOffsetDb (levelDb, thresholdDb, dynRangeDb);
                    const auto cc = designFilter (type, freqHz, effectiveGainDb, Q, fs);
                    filterL.setCoeffs (cc);
                    filterR.setCoeffs (cc);
                }
            }

            l = filterL.processSample (l);
            r = filterR.processSample (r);
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
        bool     dynamicsOn  { false };
        double   thresholdDb { -24.0 };
        double   dynRangeDb  { 0.0 };
        double   attackMs    { 10.0 };
        double   releaseMs   { 120.0 };

        // runtime
        double fs { 44100.0 };
        BiquadCoeffs staticCoeffs {};
        BiquadCoeffs detectorCoeffs {};
        Biquad filterL, filterR;
        Biquad detL, detR;
        double env { 0.0 };
        double attackCoeff { 0.0 }, releaseCoeff { 0.0 };
        int    decimCounter { 0 };
        double effectiveGainDb { 0.0 };
    };
} // namespace factory_core
