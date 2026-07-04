#pragma once
//
// factory_core/LinearPhaseCrossover5.h — a 5-band LINEAR-PHASE splitter whose band
// sum is a pure delay (perfect reconstruction with NO phase distortion), the
// mastering-grade counterpart of the allpass-compensated IIR Crossover5. Header-only,
// JUCE-independent, allocation-free in process().
//
// Complementarity BY CONSTRUCTION. Four linear-phase FIR low-pass prototypes
// h1..h4 (cutoffs f1<f2<f3<f4), ALL the same length N (odd) and hence the same
// group delay D = (N-1)/2, plus a pure D-sample delay of the input (dLy = x[n-D]):
//
//   B0 = LP1                       (LO)
//   B1 = LP2 - LP1                 (LO-MID)
//   B2 = LP3 - LP2                 (MID)
//   B3 = LP4 - LP3                 (HI-MID)
//   B4 = dLy - LP4                 (HI)
//   sum(B_i) = dLy = x delayed by D, EXACTLY (the kernels telescope to a unit
//   impulse at D regardless of how the h_i are designed) — so a parallel direct
//   path that is the same D-sample delay is phase-coherent with the band sum with
//   zero dispersion (the linear-mode analogue of the IIR tree's allpass identity).
//
// Each low-pass is a windowed-sinc (Kaiser beta 9.0, ~90 dB stopband) at cutoff
// f_i, DC-normalised to unity, so |LP_i(f_i)| ~ 0.5 (-6 dB) and adjacent bands
// cross complementary at each split. The FIR length N follows the sample rate
// (the engine sizes it for a fixed ~43 ms group delay at every rate) — a fixed tap
// count is forbidden (resolution-follows-rate, CLAUDE.md hard rule).
//
// The four low-pass convolutions ride factory_core::PartitionedConvolver (uniform-
// partitioned overlap-save: tiny FFTs, no per-block giant transform, zero added
// latency beyond the filter's own D-sample group delay). The frequency response is
// carried by an immutable PartitionedConvolver::Kernel per band; a redesign
// (draggable crossover) is computed OFF the audio thread into a spare kernel bank
// and published with a single lock-free atomic flip — process() only ever reads the
// active bank and never allocates, locks or blocks.
//
#include "PartitionedConvolver.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace factory_core
{
    class LinearPhaseCrossover5
    {
    public:
        static constexpr int    kSplits   = 4;    // four low-pass prototypes
        static constexpr double kBeta     = 9.0;  // Kaiser beta (~90 dB stopband)
        static constexpr double kMinRatio = 1.259921; // 2^(1/3), one-third octave

        // Prepare at `osRate` for up to `maxBlockOs` samples per process() call.
        // `taps` (odd) and `delayOs` = (taps-1)/2 are the FIR length / group delay
        // the caller derived from the sample rate; the delta-band delay line and the
        // partition geometry are sized here (the only allocation point).
        void prepare (double osRate, int maxBlockOs, int taps, int delayOs)
        {
            rate    = osRate;
            N       = std::max (1, taps | 1);      // force odd
            D       = std::max (0, delayOs);
            maxBlk  = std::max (1, maxBlockOs);

            for (int j = 0; j < kSplits; ++j)
                for (int c = 0; c < 2; ++c)
                    conv[j][c].prepare (maxBlk, N);

            designScratch.assign ((size_t) N, 0.0f);

            const int dl = std::max (1, D);
            for (int c = 0; c < 2; ++c) { dlyBuf[c].assign ((size_t) dl, 0.0f); dlyPos[c] = 0; }

            activeBank.store (0, std::memory_order_relaxed);
            designed = false;
            reset();
        }

        void reset() noexcept
        {
            for (int j = 0; j < kSplits; ++j)
                for (int c = 0; c < 2; ++c)
                    conv[j][c].reset();
            for (int c = 0; c < 2; ++c)
            {
                std::fill (dlyBuf[c].begin(), dlyBuf[c].end(), 0.0f);
                dlyPos[c] = 0;
            }
        }

        int    taps()           const noexcept { return N; }
        int    delayOsSamples()  const noexcept { return D; }
        bool   isDesigned()      const noexcept { return designed; }
        double effectiveHz (int i) const noexcept { return fHz[(size_t) std::clamp (i, 0, kSplits - 1)]; }

        // Redesign the four low-pass prototypes for crossover frequencies f1..f4.
        // Clamps to ascending / one-third-octave order (mirrors Crossover5), designs
        // each windowed-sinc into a SPARE kernel bank and flips the active bank with
        // one release store. Allocates inside buildKernel — MESSAGE THREAD ONLY; the
        // audio thread keeps running on the previously-active bank throughout.
        void design (double f1, double f2, double f3, double f4)
        {
            fHz[0] = f1;
            fHz[1] = std::max (f2, fHz[0] * kMinRatio);
            fHz[2] = std::max (f3, fHz[1] * kMinRatio);
            fHz[3] = std::max (f4, fHz[2] * kMinRatio);

            const int spare = 1 - activeBank.load (std::memory_order_relaxed);
            for (int j = 0; j < kSplits; ++j)
            {
                designLowpass (fHz[(size_t) j], designScratch.data());
                // Any convolver shares the geometry, so kernel[j] is usable by both
                // channels' convolvers; build with conv[0][0] as the geometry source.
                conv[0][0].buildKernel (designScratch.data(), N, kernel[(size_t) j][(size_t) spare]);
            }
            activeBank.store (spare, std::memory_order_release);
            designed = true;
        }

        // Split `nOs` samples of channel `ch` (osIn) into the five linear-phase bands
        // b0..b4. Reconstruction sum(b_i) == osIn delayed by D samples, exactly.
        // Allocation/lock/syscall-free. If no kernel has been designed the bands are
        // silenced (the caller keeps the standard IIR path until a design lands).
        void process (int ch, const float* osIn, int nOs,
                      float* b0, float* b1, float* b2, float* b3, float* b4) noexcept
        {
            if (! designed)
            {
                for (int i = 0; i < nOs; ++i) { b0[i] = b1[i] = b2[i] = b3[i] = b4[i] = 0.0f; }
                return;
            }

            const int bank = activeBank.load (std::memory_order_acquire);

            // y1..y4 into b0..b3 (convolve copies of the input in place), delayed x
            // into b4.
            float* y[kSplits] = { b0, b1, b2, b3 };
            for (int j = 0; j < kSplits; ++j)
            {
                std::copy (osIn, osIn + nOs, y[j]);
                conv[(size_t) j][(size_t) ch].process (y[j], nOs, kernel[(size_t) j][(size_t) bank]);
            }
            delayInto (ch, osIn, nOs, b4);

            // Telescoping differences (top-down so each y_j is read before overwrite).
            for (int i = 0; i < nOs; ++i) b4[i] -= b3[i];   // HI    = dLy - LP4
            for (int i = 0; i < nOs; ++i) b3[i] -= b2[i];   // HI-MID= LP4 - LP3
            for (int i = 0; i < nOs; ++i) b2[i] -= b1[i];   // MID   = LP3 - LP2
            for (int i = 0; i < nOs; ++i) b1[i] -= b0[i];   // LO-MID= LP2 - LP1
            // b0 stays LP1 (LO).
        }

    private:
        // Pure D-sample delay of the input into `out` (single-tap ring, stateful).
        void delayInto (int ch, const float* in, int nOs, float* out) noexcept
        {
            auto& buf = dlyBuf[(size_t) ch];
            const int len = (int) buf.size();
            int pos = dlyPos[(size_t) ch];
            if (D == 0) { std::copy (in, in + nOs, out); return; }
            for (int i = 0; i < nOs; ++i)
            {
                out[i]           = buf[(size_t) pos];
                buf[(size_t) pos] = in[i];
                if (++pos >= len) pos = 0;
            }
            dlyPos[(size_t) ch] = pos;
        }

        // Windowed-sinc low-pass, length N, group delay D, cutoff fc, DC-normalised.
        void designLowpass (double fc, float* ir) const noexcept
        {
            constexpr double pi = 3.14159265358979323846;
            const double wc = 2.0 * pi * std::clamp (fc, 0.0, 0.5 * rate) / rate; // rad/sample
            const double i0b = besselI0 (kBeta);
            double sum = 0.0;
            for (int n = 0; n < N; ++n)
            {
                const int m = n - D;
                const double ideal = (m == 0) ? (wc / pi)
                                              : std::sin (wc * (double) m) / (pi * (double) m);
                const double r   = (D == 0) ? 0.0 : (double) m / (double) D;
                const double win = besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
                const double h   = ideal * win;
                ir[n] = (float) h;
                sum  += h;
            }
            const double norm = (std::abs (sum) > 1.0e-300) ? (1.0 / sum) : 1.0; // unity DC gain
            for (int n = 0; n < N; ++n) ir[n] = (float) ((double) ir[n] * norm);
        }

        static double besselI0 (double x) noexcept
        {
            double s = 1.0, term = 1.0;
            const double yy = x * x / 4.0;
            for (int k = 1; k < 64; ++k)
            {
                term *= yy / (double) (k * k);
                s += term;
                if (term < 1.0e-14 * s) break;
            }
            return s;
        }

        double rate   = 44100.0;
        int    N      = 1;      // FIR length (odd)
        int    D      = 0;      // group delay = (N-1)/2 (OS samples)
        int    maxBlk = 1;
        bool   designed = false;
        double fHz[kSplits] { 130.0, 700.0, 2200.0, 7500.0 };

        PartitionedConvolver         conv[kSplits][2];       // per band, per channel (state)
        PartitionedConvolver::Kernel kernel[kSplits][2];     // per band, double-buffered (freq response)
        std::atomic<int>             activeBank { 0 };       // lock-free A/B handoff

        std::vector<float> designScratch;                    // message-thread IR scratch
        std::vector<float> dlyBuf[2];                         // delta-band delay line, per channel
        int                dlyPos[2] { 0, 0 };
    };
} // namespace factory_core
