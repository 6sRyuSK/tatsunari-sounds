#pragma once
//
// factory_core/DynamicEqBand.h — one band of the parametric EQ: a biquad filter
// (any BandType) whose gain can optionally be modulated by the signal level in
// the band (per-band dynamic EQ). Detection is a band-pass at the band centre
// plus a peak envelope follower. Header-only, headless-testable.
//
// Channel target: a band can act on the full Stereo signal (both channels), a
// single side (Left / Right), or the Mid / Side of an M/S decomposition.
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
    // Which channel(s) a band processes. Stereo filters L and R; Left/Right
    // filter only that side; Mid/Side filter the corresponding M/S component.
    enum class ChannelMode { Stereo = 0, Left, Right, Mid, Side };

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
        void setChannelMode (ChannelMode m) noexcept
        {
            if (m == channelMode) return;
            channelMode = m;
            // A chain that was idle in the old mode carries stale biquad z-state
            // from the last time it ran (e.g. Right->Stereo reactivates filterL).
            // Resuming from that state clicks / bursts a transient, so reset both
            // filter chains and the detectors when the channel target changes.
            for (auto& f : filterL) f.reset();
            for (auto& f : filterR) f.reset();
            detL.reset(); detR.reset();
            env = 0.0;
        }
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
            detL.reset();    detR.reset();
            listenL.reset(); listenR.reset();
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

            // Listen / solo band-pass: at the band's freq and (Q-reflecting) Q.
            const auto bp = designBandpass (freqHz, std::max (0.25, Q), fs);
            listenL.setCoeffs (bp);
            listenR.setCoeffs (bp);

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

            // Route the channel mode onto the two filter chains:
            //   a -> filterL chain (used by Left / Mid / Stereo-left)
            //   b -> filterR chain (used by Right / Side / Stereo-right)
            // A null pointer means that chain is idle for this mode.
            double mid = 0.0, side = 0.0;
            double* a = nullptr;
            double* b = nullptr;
            switch (channelMode)
            {
                case ChannelMode::Stereo: a = &l; b = &r; break;
                case ChannelMode::Left:   a = &l;         break;
                case ChannelMode::Right:            b = &r; break;
                case ChannelMode::Mid:  mid = 0.5 * (l + r); side = 0.5 * (l - r); a = &mid; break;
                case ChannelMode::Side: mid = 0.5 * (l + r); side = 0.5 * (l - r); b = &side; break;
            }

            if (dynamicsOn && isGainBearing (type))
            {
                double d;
                if (a != nullptr && b != nullptr)
                {
                    const double dl = detL.processSample (*a);
                    const double dr = detR.processSample (*b);
                    d = std::max (std::abs (dl), std::abs (dr));
                }
                else if (a != nullptr) d = std::abs (detL.processSample (*a));
                else                   d = std::abs (detR.processSample (*b));

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

                if (a != nullptr) *a = filterL[0].processSample (*a);
                if (b != nullptr) *b = filterR[0].processSample (*b);
            }
            else
            {
                // Static path: gain-bearing static section, or the HP/LP cascade.
                for (int k = 0; k < numStages; ++k)
                {
                    if (a != nullptr) *a = filterL[(size_t) k].processSample (*a);
                    if (b != nullptr) *b = filterR[(size_t) k].processSample (*b);
                }
            }

            if (channelMode == ChannelMode::Mid || channelMode == ChannelMode::Side)
            {
                l = mid + side;
                r = mid - side;
            }
        }

        // Solo audition: replace (l, r) with a band-pass of the *targeted*
        // channel of the (dry) input, so you hear only what this band acts on.
        // The non-targeted channel is zeroed, so Left/Right/Side stay panned and
        // a Side solo on a mono source is (correctly) silent.
        void processListen (double& l, double& r) noexcept
        {
            switch (channelMode)
            {
                case ChannelMode::Stereo:
                    l = listenL.processSample (l);
                    r = listenR.processSample (r);
                    break;
                case ChannelMode::Left:
                    l = listenL.processSample (l); r = 0.0;
                    break;
                case ChannelMode::Right:
                    r = listenR.processSample (r); l = 0.0;
                    break;
                case ChannelMode::Mid:
                {
                    const double m = listenL.processSample (0.5 * (l + r));
                    l = m; r = m;
                    break;
                }
                case ChannelMode::Side:
                {
                    const double s = listenR.processSample (0.5 * (l - r));
                    l = s; r = -s;
                    break;
                }
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
        bool        enabled     { true };
        BandType    type        { BandType::Bell };
        ChannelMode channelMode { ChannelMode::Stereo };
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
        Biquad listenL, listenR; // solo band-pass
        double env { 0.0 };
        double attackCoeff { 0.0 }, releaseCoeff { 0.0 };
        int    decimCounter { 0 };
        double effectiveGainDb { 0.0 };
    };
} // namespace factory_core
