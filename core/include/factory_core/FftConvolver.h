#pragma once
//
// factory_core/FftConvolver.h — headless, zero-latency FFT convolution built on
// factory_core::FFT.
//
// Kept only as a headless comparison oracle for the PartitionedConvolver tests; no
// shipping plugin routes audio through it (the NAM Player cab uses PartitionedConvolver).
//
// The convolver separates two concerns so the IR can be swapped lock-free from the
// message thread while the audio thread keeps running:
//   * The frequency-domain kernel H (FFT of the padded IR) is IMMUTABLE once built.
//     buildKernel() computes it (allocating the caller's vector — call it off the
//     audio thread); the audio thread receives a new kernel via an atomic handoff
//     and passes it to process(). H depends only on the FFT size, which prepare()
//     fixes, so any kernel built by this convolver is interchangeable.
//   * The processing state (input history `tail`, working buffer `frame`) is owned
//     by the audio thread. process() never allocates.
//
// Scheme: single-partition overlap-save. With FFT size N >= maxBlock + maxIrLen - 1,
// each block of n <= maxBlock samples is convolved by FFT of the frame [ last N-n
// inputs | current n inputs ], multiplied by H, inverse-FFT'd, keeping the last n
// samples (the wrap-free linear-convolution region). The current output block
// depends on the current input block => latency is exactly 0. buildKernel() and
// process() may run concurrently: FFT::forward/inverse are const and touch only the
// caller's buffer, so a message-thread buildKernel and an audio-thread process share
// the FFT twiddles read-only.
//
#include "factory_core/FFT.h"

#include <algorithm>
#include <complex>
#include <vector>

namespace factory_core
{
    class FftConvolver
    {
    public:
        using cd = std::complex<double>;

        void prepare (int maxBlock, int maxIrLen)
        {
            maxBlk = std::max (1, maxBlock);
            maxIr  = std::max (1, maxIrLen);

            const int need = maxBlk + maxIr - 1;
            order = 1;
            while ((1 << order) < need) ++order;
            n = 1 << order;

            fft.prepare (order);
            frame.assign ((size_t) n, cd (0.0, 0.0));
            tail.assign  ((size_t) n, 0.0);
        }

        void reset() noexcept { std::fill (tail.begin(), tail.end(), 0.0); }

        int fftSize() const noexcept { return n; }
        int maxIrLen() const noexcept { return maxIr; }
        int latencySamples() const noexcept { return 0; }

        // Build the frequency-domain kernel (size N) from a mono IR, truncated to the
        // configured maximum. Allocates the output vector, so call off the audio
        // thread. The resulting H is passed back to process().
        void buildKernel (const float* ir, int len, std::vector<cd>& H) const
        {
            const int L = std::clamp (len, 0, maxIr);
            H.assign ((size_t) n, cd (0.0, 0.0));
            for (int i = 0; i < L; ++i)
                H[(size_t) i] = cd ((double) ir[i], 0.0);
            fft.forward (H.data());       // in-place FFT of the padded IR
        }

        // Convolve `block` (numSamples, in place) with kernel H. A kernel of the
        // wrong size (or an empty one) is treated as "no IR" => passthrough.
        void process (float* block, int numSamples, const std::vector<cd>& H) noexcept
        {
            if ((int) H.size() != n || numSamples <= 0)
                return;
            const int ns = std::min (numSamples, maxBlk);

            std::move (tail.begin() + ns, tail.end(), tail.begin());
            for (int i = 0; i < ns; ++i)
                tail[(size_t) (n - ns + i)] = (double) block[i];

            for (int i = 0; i < n; ++i)
                frame[(size_t) i] = cd (tail[(size_t) i], 0.0);
            fft.forward (frame.data());
            for (int i = 0; i < n; ++i)
                frame[(size_t) i] *= H[(size_t) i];
            fft.inverse (frame.data());

            for (int i = 0; i < ns; ++i)                 // valid region = last ns samples
                block[i] = (float) frame[(size_t) (n - ns + i)].real();
        }

    private:
        FFT fft;
        std::vector<cd>     frame;   // audio-thread working buffer
        std::vector<double> tail;    // most recent N input samples (newest last)
        int order = 0, n = 0, maxBlk = 0, maxIr = 0;
    };
} // namespace factory_core
