#pragma once
//
// factory_core/PartitionedConvolver.h — headless, ZERO-LATENCY partitioned FFT
// convolution for long cabinet IRs. Replaces the single-partition FftConvolver on
// paths where the IR is long relative to the block: a single-partition overlap-save
// needs FFT size N >= maxBlock + maxIrLen - 1, which at 192 kHz (a 0.17 s IR is
// ~32.6k taps) balloons to N = 65536 — a huge per-block FFT. Uniform partitioning
// caps the FFT at 2*P (P = a small partition length), trading one giant transform
// for a few tiny ones plus K spectral products.
//
// Zero latency is preserved by a two-part split (Gardner-style, single tail level):
//   * HEAD — the first P IR taps — is a DIRECT time-domain FIR on the live input, so
//     the current output sample sees the current input immediately (latency 0).
//   * TAIL — IR taps [P, L) — is uniform-partitioned overlap-save. Partition k
//     (taps [kP, (k+1)P)) contributes to output window w from input frame (w-k), a
//     PAST frame (k >= 1), so the whole tail is computable at each window's START
//     from already-seen input. No part of the tail needs future samples.
//
// Correctness: head(0..P-1) + tail(P..L-1) == full linear convolution with ir[0..L).
// Verified headless against a naive time-domain oracle across block sizes and IR
// lengths spanning many partitions (tests/dsp_test.cpp).
//
// Threading mirrors FftConvolver: buildKernel() (const, allocating) runs on the
// message thread and hands an immutable Kernel to the audio thread; process() never
// allocates. FFT::forward/inverse are const, so a concurrent buildKernel + process
// share the twiddles read-only.
//
#include "factory_core/FFT.h"

#include <algorithm>
#include <complex>
#include <vector>

namespace factory_core
{
    class PartitionedConvolver
    {
    public:
        using cd = std::complex<double>;

        // Immutable frequency-domain kernel: the direct head taps plus one spectrum
        // (size N) per tail partition. Built off the audio thread, swapped lock-free.
        struct Kernel
        {
            std::vector<float>           head;   // ir[0 .. min(L,P)-1]
            std::vector<std::vector<cd>> tail;   // tail[j] = FFT([ir[(j+1)P .. ] | 0]), size N; count = K-1
            int partition = 0;                   // P this kernel was built for
            int fftSize   = 0;                   // N this kernel was built for
        };

        void prepare (int maxBlock, int maxIrLen)
        {
            maxIr = std::max (1, maxIrLen);

            // Partition length: a small power of two based on the host block, capped so
            // the FFT (N = 2P) stays tiny and the head FIR stays cheap.
            part = 64;
            while (part < maxBlock && part < 256) part <<= 1;

            n     = 2 * part;
            order = 0; while ((1 << order) < n) ++order;
            fft.prepare (order);

            maxParts = (maxIr + part - 1) / part;          // worst-case partition count (incl. head)
            frame.assign ((size_t) n, cd (0.0, 0.0));
            accum.assign ((size_t) n, cd (0.0, 0.0));
            fdl.assign   ((size_t) std::max (1, maxParts), std::vector<cd> ((size_t) n, cd (0.0, 0.0)));

            headHist.assign ((size_t) part, 0.0f);
            headMask = part - 1;

            prevWin.assign ((size_t) part, 0.0f);
            curWin.assign  ((size_t) part, 0.0f);
            tailOut.assign ((size_t) part, 0.0f);

            reset();
        }

        void reset() noexcept
        {
            std::fill (headHist.begin(), headHist.end(), 0.0f);
            std::fill (prevWin.begin(),  prevWin.end(),  0.0f);
            std::fill (curWin.begin(),   curWin.end(),   0.0f);
            std::fill (tailOut.begin(),  tailOut.end(),  0.0f);
            for (auto& f : fdl) std::fill (f.begin(), f.end(), cd (0.0, 0.0));
            headPos = 0; winIdx = 0; fdlHead = 0;
        }

        int fftSize()   const noexcept { return n; }
        int partitionLen() const noexcept { return part; }
        int maxIrLen()  const noexcept { return maxIr; }
        int latencySamples() const noexcept { return 0; }

        // Build the head taps + tail partition spectra from a mono IR (truncated to
        // the configured maximum). Allocates — call off the audio thread.
        void buildKernel (const float* ir, int len, Kernel& k) const
        {
            const int L = std::clamp (len, 0, maxIr);
            const int headLen = std::min (L, part);

            k.head.assign ((size_t) headLen, 0.0f);
            for (int i = 0; i < headLen; ++i) k.head[(size_t) i] = ir[i];

            k.tail.clear();
            for (int start = part; start < L; start += part)
            {
                std::vector<cd> H ((size_t) n, cd (0.0, 0.0));
                const int m = std::min (part, L - start);
                for (int i = 0; i < m; ++i) H[(size_t) i] = cd ((double) ir[start + i], 0.0);
                fft.forward (H.data());
                k.tail.push_back (std::move (H));
            }
            k.partition = part;
            k.fftSize   = n;
        }

        // Convolve `block` (numSamples, in place) with kernel k. A kernel built for a
        // different geometry (or empty) is treated as "no IR" => passthrough.
        void process (float* block, int numSamples, const Kernel& k) noexcept
        {
            if (numSamples <= 0) return;
            if (k.partition != part || k.fftSize != n || k.head.empty())
                return;                                     // passthrough

            const int hh = (int) k.head.size();
            const int K1 = (int) k.tail.size();             // number of tail partitions (K-1)

            for (int s = 0; s < numSamples; ++s)
            {
                const float x = block[s];

                // Head: direct FIR over the last hh inputs (includes x => zero latency).
                headHist[(size_t) headPos] = x;
                double acc = 0.0;
                int idx = headPos;
                for (int t = 0; t < hh; ++t)
                {
                    acc += (double) k.head[(size_t) t] * (double) headHist[(size_t) idx];
                    idx = (idx - 1) & headMask;
                }
                headPos = (headPos + 1) & headMask;

                block[s] = (float) (acc + (double) tailOut[(size_t) winIdx]);

                // Accumulate this window's input; on completion push its frame + refresh
                // the tail contribution for the next window.
                curWin[(size_t) winIdx] = x;
                if (++winIdx >= part)
                {
                    pushFrameAndRefresh (k, K1);
                    winIdx = 0;
                }
            }
        }

    private:
        // Window complete: FFT the frame [prevWin | curWin], store it in the FDL, then
        // recompute tailOut = sum over tail partitions of (past frame * Hpart), the
        // valid (last P) region — all from past input, so the next window is ready.
        void pushFrameAndRefresh (const Kernel& k, int K1) noexcept
        {
            for (int i = 0; i < part; ++i) frame[(size_t) i]           = cd ((double) prevWin[(size_t) i], 0.0);
            for (int i = 0; i < part; ++i) frame[(size_t) (part + i)]  = cd ((double) curWin[(size_t) i], 0.0);
            fft.forward (frame.data());
            fdl[(size_t) fdlHead] = frame;
            fdlHead = (fdlHead + 1) % std::max (1, maxParts);

            std::swap (prevWin, curWin);

            std::fill (accum.begin(), accum.end(), cd (0.0, 0.0));
            for (int kk = 1; kk <= K1; ++kk)                 // partition kk uses frame (fdlHead-kk)
            {
                int fi = fdlHead - kk;
                fi %= maxParts; if (fi < 0) fi += maxParts;
                const std::vector<cd>& F = fdl[(size_t) fi];
                const std::vector<cd>& H = k.tail[(size_t) (kk - 1)];
                for (int j = 0; j < n; ++j) accum[(size_t) j] += F[(size_t) j] * H[(size_t) j];
            }
            fft.inverse (accum.data());
            for (int j = 0; j < part; ++j) tailOut[(size_t) j] = (float) accum[(size_t) (part + j)].real();
        }

        FFT fft;
        std::vector<cd>              frame, accum;
        std::vector<std::vector<cd>> fdl;          // ring of the last maxParts frame spectra
        std::vector<float>           headHist, prevWin, curWin, tailOut;
        int order = 0, n = 0, part = 0, maxIr = 0, maxParts = 0;
        int headPos = 0, headMask = 0, winIdx = 0, fdlHead = 0;
    };
} // namespace factory_core
