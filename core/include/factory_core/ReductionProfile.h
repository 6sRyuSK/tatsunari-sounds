#pragma once
//
// factory_core/ReductionProfile.h — the soothe-style "reduction / depth EQ"
// curve for the resonance suppressor. It is NOT an audio filter: it is a
// per-frequency *multiplier on the suppression amount* (the profile fed to
// ResonanceSuppressor::setProfile). A value of 1.0 (0 dB) is the nominal
// reduction; >1 suppresses that region harder, <1 (down to 0) backs off.
//
// The curve is built from a fixed node set that mirrors the reference (soothe):
//   - a low cut and a high cut, each rolling the profile off beyond its corner
//     at a chosen slope (dB/oct) — this is what bounds *where* processing acts;
//   - four "bands", each a typed shape (bell / shelves / band shelf / band
//     reject / tilt) that locally raises or lowers the sensitivity by `sensDb`.
//
// Header-only, JUCE-independent, allocation-free: the plugin's rasteriser and
// the editor's curve both evaluate the SAME functions here (single source of
// truth), and the headless DSP test asserts the shapes directly.
//
#include <algorithm>
#include <array>
#include <cmath>

namespace factory_core
{
    enum class ReductionBandType { Bell, LowShelf, HighShelf, BandShelf, BandReject, Tilt };

    struct ReductionNodes
    {
        struct Cut  { bool on = false; double freqHz = 1000.0; double slopeDbPerOct = 24.0; };
        struct Band { bool on = false; double freqHz = 1000.0;
                      ReductionBandType type = ReductionBandType::Bell; double sensDb = 0.0; };

        Cut lowCut, highCut;
        std::array<Band, 4> bands {};
    };

    namespace detail
    {
        constexpr double kLn2 = 0.69314718055994530942;

        // Bell half-width and shelf/band edge softness, in natural-log frequency.
        // ~0.35 ≈ half an octave; kept here so the plugin and editor agree.
        constexpr double kBellSigma  = 0.35;
        constexpr double kShelfWidth = 0.50;
        constexpr double kBandHalf   = 0.50; // band-shelf/reject plateau half-width (ln)
        constexpr double kBandEdge   = 0.35; // band-shelf/reject edge softness (ln)
        constexpr double kTiltSpan   = 2.0 * kLn2; // ±2 octaves reaches ±sens

        // Flat-topped bump in [0,1]: ~1 across [-kBandHalf, +kBandHalf], smooth to 0.
        inline double bandTop (double x) noexcept
        {
            const double num = std::tanh ((x + kBandHalf) / kBandEdge)
                             - std::tanh ((x - kBandHalf) / kBandEdge);
            const double den = 2.0 * std::tanh (kBandHalf / kBandEdge);
            return num / den;
        }

        inline double bandDb (ReductionBandType type, double x, double sensDb) noexcept
        {
            switch (type)
            {
                case ReductionBandType::Bell:
                {
                    const double t = x / kBellSigma;
                    return sensDb * std::exp (-0.5 * t * t);
                }
                case ReductionBandType::LowShelf:
                    return sensDb * 0.5 * (1.0 - std::tanh (x / kShelfWidth));
                case ReductionBandType::HighShelf:
                    return sensDb * 0.5 * (1.0 + std::tanh (x / kShelfWidth));
                case ReductionBandType::BandShelf:
                    return sensDb * bandTop (x);
                case ReductionBandType::BandReject:
                    return -sensDb * bandTop (x);
                case ReductionBandType::Tilt:
                    return sensDb * std::clamp (x / kTiltSpan, -1.0, 1.0);
            }
            return 0.0;
        }

        // Rounded (Butterworth-magnitude) cut: ~unity in the pass-band, a smooth
        // −3 dB knee AT the corner, asymptoting to slopeDbPerOct in the stop-band.
        // Order N = slope/6; |H|^2 = 1/(1 + ratio^(2N)), ratio = fc/f (low cut) or
        // f/fc (high cut). This gives the filter-like roundness a hard piecewise
        // ramp lacks, while keeping the same far-field slope.
        inline double cutDb (double f, double fc, double slopeDbPerOct, bool lowCut) noexcept
        {
            const double order = std::max (0.5, slopeDbPerOct / 6.0);
            const double ratio = lowCut ? (fc / std::max (1.0e-6, f))
                                        : (f  / std::max (1.0e-6, fc));
            return -10.0 * std::log10 (1.0 + std::pow (ratio, 2.0 * order));
        }
    } // namespace detail

    // Total profile deviation at frequency f (Hz), in dB (0 dB = nominal).
    inline double reductionProfileDbAt (double f, const ReductionNodes& n) noexcept
    {
        const double lf = std::log (std::max (1.0e-6, f));
        double db = 0.0;

        // Cuts: a rounded Butterworth roll-off (−3 dB at the corner) — applies
        // near/beyond the corner, ~0 deep in the pass-band.
        if (n.lowCut.on)  db += detail::cutDb (f, n.lowCut.freqHz,  n.lowCut.slopeDbPerOct,  true);
        if (n.highCut.on) db += detail::cutDb (f, n.highCut.freqHz, n.highCut.slopeDbPerOct, false);

        for (const auto& b : n.bands)
            if (b.on)
                db += detail::bandDb (b.type, lf - std::log (std::max (1.0e-6, b.freqHz)), b.sensDb);

        return db;
    }

    // The same, converted to the linear multiplier fed to setProfile and clamped
    // to the suppressor's accepted range (default [0, 4], i.e. up to +12 dB).
    inline double reductionProfileLinearAt (double f, const ReductionNodes& n,
                                            double lo = 0.0, double hi = 4.0) noexcept
    {
        return std::clamp (std::pow (10.0, reductionProfileDbAt (f, n) / 20.0), lo, hi);
    }
} // namespace factory_core
