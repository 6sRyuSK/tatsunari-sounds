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
//     estimate() / estimateCandidates() then never allocate, lock, or make a
//     syscall — safe to run on the audio thread.
//   * estimate(x, n, minHz, maxHz, clarityThreshold) analyses one contiguous
//     frame and returns the MPM single-frame pick (first peak reaching
//     kPeakRatio of the global maximum).
//   * estimateCandidates(...) fills up to maxOut NSDF local-maximum candidates
//     (parabolically interpolated), for callers that want temporal trajectory
//     selection instead of the single-frame MPM pick. NO harmonic-support
//     scoring is applied here — periodicity alone cannot distinguish f0 from
//     integer multiples (see pitch-fix-detection-accuracy.md §2.1).
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

    struct PitchCandidate
    {
        double f0Hz    = 0.0;
        double clarity = 0.0;   // NSDF peak height, [0, 1]
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

        // Default floor for estimateCandidates: expose secondary peaks (incl. the
        // true f0 when MPM would prefer a harmonic) without dumping noise.
        static constexpr double kCandidateRatio = 0.45;

        static constexpr int kMaxCandidates = 64;

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
        // MPM single-frame pick — unchanged contract for existing callers/tests.
        PitchEstimate estimate (const float* x, int n,
                                double minHz, double maxHz,
                                double clarityThreshold) noexcept
        {
            PitchEstimate out;
            PitchCandidate cands[kMaxCandidates];
            const int nCand = estimateCandidates (x, n, minHz, maxHz,
                                                  cands, kMaxCandidates, kPeakRatio);
            if (nCand <= 0)
                return out;

            // MPM: first candidate reaching kPeakRatio of the global maximum.
            // estimateCandidates already filtered by minClarityRatio; with
            // kPeakRatio that leaves only peaks >= 0.9*best, still ordered by
            // ascending lag — so index 0 is the MPM pick.
            double bestVal = 0.0;
            for (int c = 0; c < nCand; ++c)
                bestVal = std::max (bestVal, cands[c].clarity);
            int pick = -1;
            for (int c = 0; c < nCand; ++c)
                if (cands[c].clarity >= kPeakRatio * bestVal) { pick = c; break; }
            if (pick < 0)
                return out;

            const double peakVal = cands[pick].clarity;
            const double f0 = cands[pick].f0Hz;
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

        // Fill `out[0..return)` with NSDF local-maximum candidates inside
        // [minHz, maxHz], sorted by ascending lag (highest frequency first).
        // Only peaks with clarity >= minClarityRatio * bestPeak are kept.
        // Allocation-free. Returns 0 on silence / no peaks.
        int estimateCandidates (const float* x, int n,
                                double minHz, double maxHz,
                                PitchCandidate* out, int maxOut,
                                double minClarityRatio = kCandidateRatio) noexcept
        {
            if (out == nullptr || maxOut <= 0 || x == nullptr || n < 16 || fs <= 0.0)
                return 0;
            n = std::min (n, maxWin);
            minClarityRatio = std::clamp (minClarityRatio, 0.0, 1.0);

            // --- silence floor -------------------------------------------------
            double power = 0.0;
            for (int i = 0; i < n; ++i)
                power += (double) x[i] * (double) x[i];
            if (power / (double) n < kPowerFloor)
                return 0;

            // --- lag range from the requested pitch range ----------------------
            int lagMin = (int) std::floor (fs / std::max (1.0, maxHz));
            int lagMax = (int) std::ceil  (fs / std::max (1.0, minHz));
            lagMin = std::max (2, lagMin);
            lagMax = std::min ({ lagMax, maxLag, n / 2 });
            if (lagMax <= lagMin + 2)
                return 0;

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

            // --- Collect positive-region local maxima --------------------------
            double bestVal = 0.0;
            int    nRaw    = 0;
            int    rawTau[kMaxCandidates];
            double rawVal[kMaxCandidates];

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
                    if (regionHasPeak && nRaw < kMaxCandidates)
                    {
                        rawTau[nRaw] = regionTau;
                        rawVal[nRaw] = regionVal;
                        ++nRaw;
                        bestVal = std::max (bestVal, regionVal);
                    }
                    inRegion = false;
                }
            }
            if (nRaw == 0 || bestVal <= 0.0)
                return 0;

            const double floor = minClarityRatio * bestVal;
            int written = 0;
            for (int c = 0; c < nRaw && written < maxOut; ++c)
            {
                if (rawVal[c] < floor)
                    continue;

                const int    t  = rawTau[c];
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
                    continue;

                out[written].f0Hz    = f0;
                out[written].clarity = peakVal;
                ++written;
            }
            return written;
        }

    private:
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
