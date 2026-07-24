#pragma once
//
// factory_core/PitchDetector.h — monophonic fundamental-frequency estimator
// using the McLeod Pitch Method (MPM): the normalised square difference
// function (NSDF) evaluated via FFT autocorrelation, key-maximum picking with
// a relative threshold, and parabolic interpolation for sub-sample lag.
// Header-only, JUCE-independent, headless-testable.
//
// CONTRACT
//   * prepare(sampleRate, lowestMinHz, maxWindowPeriods) sizes every buffer for
//     the worst case (window up to maxWindowPeriods periods of lowestMinHz,
//     plus one period of lag range) and precomputes one FFT per usable order.
//     estimate() then never allocates, locks, or makes a syscall — safe to run
//     on the audio thread.
//   * estimate(x, n, minHz, maxHz, clarityThreshold) analyses one contiguous
//     frame. The caller chooses n >= 2 * sampleRate/minHz (so the summation
//     window still spans a full period at the largest lag) and
//     minHz >= lowestMinHz. The FFT order is derived from the frame + lag
//     length, so the analysis resolution scales with the sample rate by
//     construction (a fixed order is forbidden repo-wide).
//   * Absolute silence floor (regression policy: detectors must never produce
//     phantom output on silence): a frame whose mean square is below
//     kPowerFloor is unvoiced regardless of the NSDF shape.
//   * Result: f0Hz == 0 and voiced == false when no reliable pitch was found;
//     clarity is the NSDF peak height in [0, 1] (1 == perfectly periodic).
//
#include "FFT.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace factory_core
{
    struct PitchEstimate
    {
        double f0Hz    = 0.0;   // 0 when unvoiced
        double clarity = 0.0;   // NSDF peak height, [0, 1]
        bool   voiced  = false;
    };

    class PitchDetector
    {
    public:
        // Mean-square silence floor (~ -80 dBFS RMS). Below this the frame is
        // unvoiced by definition — no phantom pitch on silence or dither.
        static constexpr double kPowerFloor = 1.0e-8;

        // MPM key-maximum threshold: the first local NSDF maximum reaching
        // kPeakRatio * (global maximum) wins, which suppresses octave-down
        // errors without biasing the chosen peak's position.
        static constexpr double kPeakRatio = 0.90;

        void prepare (double sampleRate, double lowestMinHz, double maxWindowPeriods)
        {
            fs = sampleRate;
            const double maxPeriod = fs / std::max (1.0, lowestMinHz);
            maxLag = (int) std::ceil (maxPeriod) + 2;
            maxWin = (int) std::ceil (maxWindowPeriods * maxPeriod) + 2;

            const int maxOrder = orderForLength (maxWin + maxLag + 1);
            minOrder = 6; // tiny frames still round up to a 64-point FFT
            ffts.assign ((size_t) (maxOrder - minOrder + 1), FFT());
            for (int o = minOrder; o <= maxOrder; ++o)
                ffts[(size_t) (o - minOrder)].prepare (o);

            cbuf.assign ((size_t) 1 << maxOrder, FFT::cd (0.0, 0.0));
            energyPrefix.assign ((size_t) maxWin + 1, 0.0);
            nsdf.assign ((size_t) maxLag + 2, 0.0);
        }

        int maxWindowSamples() const noexcept { return maxWin; }

        // Analyse one frame of n samples (the most recent last). Allocation-free.
        PitchEstimate estimate (const float* x, int n,
                                double minHz, double maxHz,
                                double clarityThreshold) noexcept
        {
            PitchEstimate out;
            if (x == nullptr || n < 16 || fs <= 0.0)
                return out;
            n = std::min (n, maxWin);

            // --- silence floor -------------------------------------------------
            double power = 0.0;
            for (int i = 0; i < n; ++i)
                power += (double) x[i] * (double) x[i];
            if (power / (double) n < kPowerFloor)
                return out;

            // --- lag range from the requested pitch range ----------------------
            int lagMin = (int) std::floor (fs / std::max (1.0, maxHz));
            int lagMax = (int) std::ceil  (fs / std::max (1.0, minHz));
            lagMin = std::max (2, lagMin);
            lagMax = std::min ({ lagMax, maxLag, n / 2 });
            if (lagMax <= lagMin + 2)
                return out;

            // --- autocorrelation r(tau) via FFT (Wiener–Khinchin) ---------------
            const int   order = orderForLength (n + lagMax + 1);
            const FFT&  fft   = ffts[(size_t) std::clamp (order - minOrder, 0,
                                                          (int) ffts.size() - 1)];
            const int   N     = fft.size();
            FFT::cd*    a     = cbuf.data();
            for (int i = 0; i < n; ++i) a[i] = FFT::cd ((double) x[i], 0.0);
            for (int i = n; i < N; ++i) a[i] = FFT::cd (0.0, 0.0);
            fft.forward (a);
            for (int i = 0; i < N; ++i) a[i] = FFT::cd (std::norm (a[i]), 0.0);
            fft.inverse (a);

            // --- NSDF: n'(tau) = 2 r(tau) / (m(tau)) ----------------------------
            energyPrefix[0] = 0.0;
            for (int i = 0; i < n; ++i)
                energyPrefix[(size_t) i + 1] = energyPrefix[(size_t) i]
                                             + (double) x[i] * (double) x[i];
            for (int tau = lagMin - 1; tau <= lagMax + 1 && tau < n; ++tau)
            {
                const double m = energyPrefix[(size_t) (n - tau)]
                               + (energyPrefix[(size_t) n] - energyPrefix[(size_t) tau]);
                nsdf[(size_t) tau] = m > 1.0e-12 ? 2.0 * a[tau].real() / m : 0.0;
            }

            // --- McLeod key-maximum picking -------------------------------------
            // Collect the highest point of every positive NSDF region inside the
            // lag range, then take the FIRST candidate reaching kPeakRatio of the
            // best one. Regions are delimited by zero crossings, which excludes
            // the tau≈0 main lobe (still positive at lagMin) unless a genuine
            // local maximum lives inside the range.
            double bestVal = 0.0;
            int    nCand   = 0;
            int    candTau[kMaxCandidates];
            double candVal[kMaxCandidates];

            bool   inRegion  = false;
            double regionVal = 0.0;
            int    regionTau = 0;
            bool   regionHasPeak = false;
            for (int tau = lagMin; tau <= lagMax; ++tau)
            {
                const double v = nsdf[(size_t) tau];
                if (v > 0.0)
                {
                    const bool isLocalMax = v >= nsdf[(size_t) (tau - 1)]
                                         && v >= nsdf[(size_t) (tau + 1)];
                    if (! inRegion) { inRegion = true; regionVal = -1.0; regionHasPeak = false; }
                    if (isLocalMax && v > regionVal)
                    {
                        regionVal = v;
                        regionTau = tau;
                        regionHasPeak = true;
                    }
                }
                if ((v <= 0.0 || tau == lagMax) && inRegion)
                {
                    if (regionHasPeak && nCand < kMaxCandidates)
                    {
                        candTau[nCand] = regionTau;
                        candVal[nCand] = regionVal;
                        ++nCand;
                        bestVal = std::max (bestVal, regionVal);
                    }
                    inRegion = false;
                }
            }
            if (nCand == 0 || bestVal <= 0.0)
                return out;

            int pick = -1;
            for (int c = 0; c < nCand; ++c)
                if (candVal[c] >= kPeakRatio * bestVal) { pick = c; break; }
            if (pick < 0)
                return out;

            // --- parabolic interpolation around the picked lag ------------------
            const int    t  = candTau[pick];
            const double y0 = nsdf[(size_t) (t - 1)];
            const double y1 = nsdf[(size_t) t];
            const double y2 = nsdf[(size_t) (t + 1)];
            const double den = y0 - 2.0 * y1 + y2;
            double delta = 0.0;
            if (std::abs (den) > 1.0e-15)
                delta = std::clamp (0.5 * (y0 - y2) / den, -1.0, 1.0);
            const double tauStar = (double) t + delta;
            const double peakVal = std::clamp (y1 - 0.25 * (y0 - y2) * delta, 0.0, 1.0);

            const double f0 = fs / tauStar;
            if (! (f0 > 0.0) || f0 < minHz * 0.9 || f0 > maxHz * 1.1)
                return out;

            out.f0Hz    = f0;
            out.clarity = peakVal;
            out.voiced  = peakVal >= clarityThreshold;
            if (! out.voiced)
            {
                out.f0Hz = 0.0;
                return out;
            }
            return out;
        }

    private:
        static constexpr int kMaxCandidates = 64;

        static int orderForLength (int len) noexcept
        {
            int o = 1;
            while ((1 << o) < len) ++o;
            return o;
        }

        double fs     = 0.0;
        int    maxLag = 0;
        int    maxWin = 0;
        int    minOrder = 6;

        std::vector<FFT>      ffts;         // one per order in [minOrder, maxOrder]
        std::vector<FFT::cd>  cbuf;         // FFT work buffer (max size)
        std::vector<double>   energyPrefix; // running Σ x² for the NSDF denominator
        std::vector<double>   nsdf;
    };
} // namespace factory_core
