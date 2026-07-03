#pragma once
//
// factory_core/HarmonicShaper.h — a mode-selectable degree-5 polynomial harmonic
// shaper with an optional first-order antiderivative anti-aliasing (ADAA1) path.
// Header-only, JUCE-independent, allocation-free. One instance shapes one band of
// one channel of the multiband enhancer.
//
// Curve family (all modes share the shape):
//     f(u) = u + sum_{k=2..5} c_k(mode) * e * u^k        for |u| <= 1
// with e = enhance in [0,1]. The LINEAR term is always 1 (the fundamental stays
// ~unity — no "louder == better" illusion), and each harmonic level scales as
// 20*log10(e), monotone and predictable. For |u| > 1 the curve continues with a
// C1 soft saturation  f(1+t) = f(1) + f'(1)(1 - e^{-t})  (mirrored below -1);
// f'(±1) >= 0 for every mode so the transfer stays monotone through the join.
//
// Anti-aliasing:
//   * When wrapped by >=2x oversampling the caller evaluates the raw residual
//     f(u)-u; the degree-5 curve's images are then killed by the oversampler's
//     ~80 dB stopband.
//   * At 1x (Zero-Latency mode, and HQ at 176.4/192 kHz) the caller enables ADAA:
//     r(x)=f(x)-x with antiderivative R(x)=F(x)-x^2/2 gives
//         y = (R(x[n]) - R(x[n-1])) / (x[n] - x[n-1])
//     (falling back to r((x[n]+x[n-1])/2) when |dx| is tiny). The LINEAR part is
//     exact and undelayed; only the residual carries the ADAA half-sample delay.
//
// Glue mode adds an envelope-normalised drive supplied by the engine via
// setEnvGain(g): the residual becomes r_g(x) = (f(g*x) - g*x)/g, so the linear
// term passes through untouched while the harmonic ratio's level-sensitivity is
// halved (alpha = 0.5). Its ADAA antiderivative is R_g(x) = F(g*x)/g^2 - x^2/2.
//
// Mode changes crossfade the coefficients over ~30 ms (same topology -> no reset,
// no click).
//
#include <algorithm>
#include <cmath>

namespace factory_core
{
    class HarmonicShaper
    {
    public:
        enum class Mode { Tube = 0, Tape, Bright, Clean, Glue };

        void prepare (double rate) noexcept
        {
            fs = rate;
            const double rampSamples = std::max (1.0, 0.03 * fs);   // ~30 ms coeff crossfade
            modeSmooth = 1.0 - std::exp (-1.0 / rampSamples);
            for (int i = 0; i < 4; ++i) cc[i] = targetCoeffs (mode)[i];
            reset();
        }

        void reset() noexcept
        {
            xPrev = 0.0;
            for (int i = 0; i < 4; ++i) cc[i] = targetCoeffs (mode)[i];
        }

        void setMode (Mode m) noexcept { mode = m; glueOn = (m == Mode::Glue); }
        void setEnhance (double e) noexcept { enh = std::clamp (e, 0.0, 1.0); }
        void setAdaa (bool on) noexcept { adaa = on; }
        void setEnvGain (double g) noexcept { envGain = std::clamp (g, 1.0e-3, 1.0e3); }

        // Return the residual r(x) so the caller forms x + r(x). Non-allocating.
        double processResidual (double x) noexcept
        {
            // Crossfade coefficients toward the current mode (clickless morph).
            const double* tc = targetCoeffs (mode);
            for (int i = 0; i < 4; ++i)
                cc[i] += (tc[i] - cc[i]) * modeSmooth;

            double y;
            if (! adaa)
            {
                y = residualVal (x);
            }
            else
            {
                const double dx = x - xPrev;
                if (std::abs (dx) < 1.0e-6)
                    y = residualVal (0.5 * (x + xPrev));
                else
                    y = (rAntideriv (x) - rAntideriv (xPrev)) / dx;
            }
            xPrev = x;
            return y;
        }

    private:
        static const double* targetCoeffs (Mode m) noexcept
        {
            //                        c2      c3      c4     c5
            static const double tube  [4] { 0.35,  0.08,  0.00, 0.00 };
            static const double tape  [4] { 0.00, -0.30,  0.00, 0.06 };
            static const double bright[4] { 0.08,  0.10,  0.10, 0.15 };
            static const double clean [4] { 0.20, -0.10,  0.00, 0.00 };
            switch (m)
            {
                case Mode::Tube:   return tube;
                case Mode::Tape:   return tape;
                case Mode::Bright: return bright;
                case Mode::Clean:  return clean;
                case Mode::Glue:   return tape;   // Glue uses the Tape curve + env normalisation
            }
            return tube;
        }

        // --- core polynomial on |u| <= 1 -------------------------------------
        double fCore (double u) const noexcept
        {
            const double u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u;
            return u + enh * (cc[0] * u2 + cc[1] * u3 + cc[2] * u4 + cc[3] * u5);
        }
        double fpCore (double u) const noexcept
        {
            const double u2 = u * u, u3 = u2 * u, u4 = u3 * u;
            return 1.0 + enh * (2.0 * cc[0] * u + 3.0 * cc[1] * u2 + 4.0 * cc[2] * u3 + 5.0 * cc[3] * u4);
        }
        // Antiderivative of fCore with F(0)=0.
        double FCore (double u) const noexcept
        {
            const double u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u, u6 = u5 * u;
            return 0.5 * u2 + enh * (cc[0] * u3 / 3.0 + cc[1] * u4 / 4.0 + cc[2] * u5 / 5.0 + cc[3] * u6 / 6.0);
        }

        // --- full curve with C1 soft-saturating continuation -----------------
        double f (double u) const noexcept
        {
            if (u > 1.0)
            {
                const double t = u - 1.0;
                return fCore (1.0) + fpCore (1.0) * (1.0 - std::exp (-t));
            }
            if (u < -1.0)
            {
                const double t = -1.0 - u;
                return fCore (-1.0) - fpCore (-1.0) * (1.0 - std::exp (-t));
            }
            return fCore (u);
        }
        double F (double u) const noexcept
        {
            if (u > 1.0)
            {
                const double t = u - 1.0, f1 = fCore (1.0), fp1 = fpCore (1.0);
                return FCore (1.0) + (f1 + fp1) * t - fp1 * (1.0 - std::exp (-t));
            }
            if (u < -1.0)
            {
                const double t = -1.0 - u, fm = fCore (-1.0), fpm = fpCore (-1.0);
                return FCore (-1.0) - fm * t + fpm * (t - (1.0 - std::exp (-t)));
            }
            return FCore (u);
        }

        // Residual (Glue folds in the envelope-normalised drive g).
        double residualVal (double x) const noexcept
        {
            if (glueOn)
            {
                const double g = envGain;
                return (f (g * x) - g * x) / g;         // = residual(g*x)/g
            }
            return f (x) - x;
        }
        // Antiderivative of residualVal (for ADAA), R(x) = F(.)-x^2/2.
        double rAntideriv (double x) const noexcept
        {
            if (glueOn)
            {
                const double g = envGain;
                return F (g * x) / (g * g) - 0.5 * x * x;
            }
            return F (x) - 0.5 * x * x;
        }

        double fs         = 44100.0;
        Mode   mode       = Mode::Tube;
        bool   glueOn     = false;
        bool   adaa       = false;
        double enh        = 0.0;
        double envGain    = 1.0;
        double cc[4]      { 0.35, 0.08, 0.0, 0.0 }; // smoothed coeffs c2..c5
        double modeSmooth = 0.01;
        double xPrev      = 0.0;
    };
} // namespace factory_core
