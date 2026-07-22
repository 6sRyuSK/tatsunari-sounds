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
//   - eight "bands", each a typed shape (bell / shelves / band shelf / band
//     reject / tilt) that locally raises or lowers the sensitivity by `sensDb`,
//     over a width set by `widthOct` (Phase 4: independently variable per band).
//
// Each shape's width-bearing constants (bell sigma, shelf/band-edge softness,
// tilt span) scale by widthOct / kWidthRef, so widthOct == kWidthRef (0.50, the
// default for every band) reproduces the pre-Phase-4 fixed-width curve exactly
// (bit-identical: the scale factor is IEEE-exactly 1.0) — see the DSP test's
// default-identity gate against an independent v1 oracle.
//
// Header-only, JUCE-independent, allocation-free: the plugin's rasteriser and
// the editor's curve both evaluate the SAME functions here (single source of
// truth), and the headless DSP test asserts the shapes directly.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace factory_core
{
    enum class ReductionBandType { Bell, LowShelf, HighShelf, BandShelf, BandReject, Tilt };

    struct ReductionNodes
    {
        struct Cut  { bool on = false; double freqHz = 1000.0; double slopeDbPerOct = 24.0; };
        struct Band { bool on = false; double freqHz = 1000.0;
                      ReductionBandType type = ReductionBandType::Bell; double sensDb = 0.0;
                      double widthOct = 0.50; }; // shape width, in octaves (see kWidthRef)

        Cut lowCut, highCut;
        std::array<Band, 8> bands {};
    };

    namespace detail
    {
        constexpr double kLn2 = 0.69314718055994530942;

        // Bell half-width and shelf/band edge softness, in natural-log frequency,
        // AT THE REFERENCE WIDTH (widthOct == kWidthRef). ~0.35 ≈ half an octave;
        // kept here so the plugin and editor agree. Every band's actual width
        // scales these by w = widthOct / kWidthRef (see bandDb), so at the
        // reference width w == 1.0 exactly (IEEE) and nothing changes.
        constexpr double kWidthRef   = 0.50; // widthOct at which w == 1.0 (bit-identical to pre-Phase-4)
        constexpr double kBellSigma  = 0.35;
        constexpr double kShelfWidth = 0.50;
        constexpr double kBandHalf   = 0.50; // band-shelf/reject plateau half-width (ln)
        constexpr double kBandEdge   = 0.35; // band-shelf/reject edge softness (ln)
        constexpr double kTiltSpan   = 2.0 * kLn2; // ±2 octaves reaches ±sens (at kWidthRef)

        // Flat-topped bump in [0,1]: ~1 across [-half, +half], smooth to 0 over edge.
        inline double bandTop (double x, double half, double edge) noexcept
        {
            const double num = std::tanh ((x + half) / edge)
                             - std::tanh ((x - half) / edge);
            const double den = 2.0 * std::tanh (half / edge);
            return num / den;
        }

        // widthOct scales every shape's width-bearing constant by w = widthOct /
        // kWidthRef. At widthOct == kWidthRef, w == 1.0 exactly (IEEE division of
        // two equal finite values), and x * 1.0 == x exactly, so every case below
        // reproduces the pre-Phase-4 fixed-width formula bit-for-bit.
        inline double bandDb (ReductionBandType type, double x, double sensDb, double widthOct) noexcept
        {
            const double w = widthOct / kWidthRef;
            switch (type)
            {
                case ReductionBandType::Bell:
                {
                    const double t = x / (kBellSigma * w);
                    return sensDb * std::exp (-0.5 * t * t);
                }
                case ReductionBandType::LowShelf:
                    return sensDb * 0.5 * (1.0 - std::tanh (x / (kShelfWidth * w)));
                case ReductionBandType::HighShelf:
                    return sensDb * 0.5 * (1.0 + std::tanh (x / (kShelfWidth * w)));
                case ReductionBandType::BandShelf:
                    return sensDb * bandTop (x, kBandHalf * w, kBandEdge * w);
                case ReductionBandType::BandReject:
                    return -sensDb * bandTop (x, kBandHalf * w, kBandEdge * w);
                case ReductionBandType::Tilt:
                    return sensDb * std::clamp (x / (kTiltSpan * w), -1.0, 1.0);
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
                db += detail::bandDb (b.type, lf - std::log (std::max (1.0e-6, b.freqHz)), b.sensDb, b.widthOct);

        return db;
    }

    // The same, converted to the linear multiplier fed to setProfile and clamped
    // to the suppressor's accepted range (default [0, 4], i.e. up to +12 dB).
    inline double reductionProfileLinearAt (double f, const ReductionNodes& n,
                                            double lo = 0.0, double hi = 4.0) noexcept
    {
        return std::clamp (std::pow (10.0, reductionProfileDbAt (f, n) / 20.0), lo, hi);
    }

    // ---- prepared-nodes fast path (rasteriser hoist) ------------------------
    // A rasteriser evaluates the profile at MANY frequencies (every FFT bin)
    // against the SAME node set. reductionProfileDbAt recomputes each band's
    // centre log-frequency, std::log(freqHz), on every call -- loop-invariant
    // across the sweep. prepareBandLogF computes those eight logs ONCE; the
    // *Prepared variants below then reuse them. The result is BIT-IDENTICAL to
    // the per-call form (same inputs, same deterministic std::log, same band
    // iteration/accumulation order), just fewer transcendental calls -- proven by
    // the DSP test's profilePreparedHoistIdentityTest (tolerance 0). Single-
    // frequency callers keep using reductionProfileDbAt / reductionProfileLinearAt.
    using BandLogF = std::array<double, 8>;

    inline void prepareBandLogF (const ReductionNodes& n, BandLogF& out) noexcept
    {
        for (int b = 0; b < 8; ++b)
            out[(size_t) b] = std::log (std::max (1.0e-6, n.bands[(size_t) b].freqHz));
    }

    inline double reductionProfileDbAtPrepared (double f, const ReductionNodes& n,
                                                const BandLogF& bandLogF) noexcept
    {
        const double lf = std::log (std::max (1.0e-6, f));
        double db = 0.0;

        if (n.lowCut.on)  db += detail::cutDb (f, n.lowCut.freqHz,  n.lowCut.slopeDbPerOct,  true);
        if (n.highCut.on) db += detail::cutDb (f, n.highCut.freqHz, n.highCut.slopeDbPerOct, false);

        for (int b = 0; b < 8; ++b)
        {
            const auto& bn = n.bands[(size_t) b];
            if (bn.on)
                db += detail::bandDb (bn.type, lf - bandLogF[(size_t) b], bn.sensDb, bn.widthOct);
        }
        return db;
    }

    inline double reductionProfileLinearAtPrepared (double f, const ReductionNodes& n,
                                                    const BandLogF& bandLogF,
                                                    double lo = 0.0, double hi = 4.0) noexcept
    {
        return std::clamp (std::pow (10.0, reductionProfileDbAtPrepared (f, n, bandLogF) / 20.0), lo, hi);
    }

    // Bit-exact equality of two node sets -- the KEY of the rasterisation cache
    // (a plugin re-rasterises only when this changes). memcmp on each double
    // sidesteps -Wfloat-equal and never touches struct padding; bools/enums use
    // integral ==. Identical params (read from the same source unchanged) compare
    // equal to the bit, so the cache can never skip a genuine change; a changed
    // bit (any edit) always re-rasterises. Shared so the JUCE processor and RsCore
    // key their caches identically (the byte-equivalence gate proves parity).
    inline bool reductionNodesIdentical (const ReductionNodes& a, const ReductionNodes& b) noexcept
    {
        auto sameD = [] (double x, double y) noexcept { return std::memcmp (&x, &y, sizeof (double)) == 0; };
        auto sameCut = [&] (const ReductionNodes::Cut& x, const ReductionNodes::Cut& y) noexcept
        {
            return x.on == y.on && sameD (x.freqHz, y.freqHz) && sameD (x.slopeDbPerOct, y.slopeDbPerOct);
        };
        if (! sameCut (a.lowCut, b.lowCut) || ! sameCut (a.highCut, b.highCut)) return false;
        for (int b_ = 0; b_ < 8; ++b_)
        {
            const auto& x = a.bands[(size_t) b_];
            const auto& y = b.bands[(size_t) b_];
            if (x.on != y.on || x.type != y.type
                || ! sameD (x.freqHz, y.freqHz) || ! sameD (x.sensDb, y.sensDb) || ! sameD (x.widthOct, y.widthOct))
                return false;
        }
        return true;
    }
} // namespace factory_core
