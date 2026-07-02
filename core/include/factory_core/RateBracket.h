#pragma once
//
// factory_core/RateBracket.h — the "run a fixed-rate section inside a host running
// at another rate" bracket, extracted whole from the NAM Player so it is headless
// and JUCE-independent (and therefore testable as the real production code path,
// not a re-implementation of it).
//
// A stereo signal enters at the host rate. RateBracket resamples host->model,
// hands the model-rate block to a caller-supplied section callback (the NAM
// routing / amp chain, run in place), resamples model->host, and buffers the
// result in a small output FIFO so it can deliver exactly the host block size
// every call. The reported latency is fixed and deterministic; the caller delays
// the dry path by latencySamples() to keep wet and dry aligned.
//
// Latency model (the fix for the 0.1.0 wet/dry mismatch): the output FIFO is
// pre-filled with ONLY a small safety margin (kFifoMargin), NOT the whole
// reported latency. The resampler chain already contributes its round-trip group
// delay g = resamplerRoundTripLatency(host, model, base) inherently; pre-filling
// by g again (as 0.1.0 did) double-counted it, so the wet path lagged the dry
// path / host PDC by exactly g samples. With prefill == kFifoMargin the true wet
// delay is g + kFifoMargin == latencySamples(), matching the dry delay exactly.
// kFifoMargin only needs to cover the per-block production jitter of a fractional
// resampler so the FIFO never underruns; it is independent of g.
//
// When hostRate == modelRate the bracket is a bit-exact passthrough with zero
// latency (the resamplers are bypassed entirely).
//
// prepare() allocates; reset() and process() are allocation-free, lock-free and
// noexcept (audio-thread safe). Templated on the streaming resampler type so the
// band-limited PolyphaseResampler can be swapped in without touching this file.
//
#include "factory_core/PolyphaseResampler.h"
#include "factory_core/Resampler.h"
#include "factory_core/ResamplerLatency.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace factory_core
{
    // Defaults to the band-limited PolyphaseResampler: the section this brackets is
    // non-linear (the NAM amp), so its output has energy above the lower Nyquist that
    // the down/up conversions must not alias/image. (RateBracket<Resampler> exists for
    // linear paths / comparison but must NOT wrap a non-linear section.)
    template <typename ResamplerT = PolyphaseResampler>
    class RateBracket
    {
    public:
        // Safety cushion pre-loaded into the output FIFO so a fractional resampler's
        // per-block production jitter can never underrun it. Independent of the
        // resampler group delay (see the header note); 16 host samples is ample.
        static constexpr int kFifoMargin = 16;

        void prepare (double hostRate, double modelRate, int maxHostBlock)
        {
            hostR    = hostRate;
            modelR   = modelRate;
            maxHost  = std::max (1, maxHostBlock);
            resampling = std::abs (hostRate - modelRate) > 1.0e-6;

            if (! resampling)
            {
                reportedLatency = 0;
                namMaxBlk       = maxHost;
                return;
            }

            // Model-rate block capacity for the worst-case (largest) host block, with
            // headroom for the resampler's fractional-phase carry-over.
            namMaxBlk = (int) std::ceil (maxHost * modelR / hostR) + 32;

            int base = 2;
            for (int ch = 0; ch < 2; ++ch)
            {
                down[(size_t) ch].prepare (hostR, modelR);
                up[(size_t) ch].prepare   (modelR, hostR);
                base = down[(size_t) ch].groupDelayInputSamples();

                namBuf[(size_t) ch].assign    ((size_t) namMaxBlk, 0.0f);
                // Up-sampling namMaxBlk model samples back to host rate yields at most
                // ceil(namMaxBlk * host/model) + base host samples; +64 headroom.
                const int upCap = (int) std::ceil (namMaxBlk * hostR / modelR) + base + 64;
                upScratch[(size_t) ch].assign ((size_t) upCap, 0.0f);
                fifo[(size_t) ch].prepare (maxHost * 4 + 64);
            }

            reportedLatency = resamplerRoundTripLatency (hostR, modelR, base) + kFifoMargin;
            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (resampling)
                {
                    down[(size_t) ch].reset();
                    up[(size_t) ch].reset();
                    fifo[(size_t) ch].reset();
                    fifo[(size_t) ch].pushZeros (kFifoMargin);   // margin only — see header
                }
            }
        }

        int  latencySamples()    const noexcept { return reportedLatency; }
        int  modelBlockCapacity() const noexcept { return namMaxBlk; }
        bool active()            const noexcept { return resampling; }

        // Run n host samples through the model-rate section. sectionFn is invoked as
        // sectionFn(float* l, float* r, int m) and must process the two model-rate
        // channels IN PLACE. inX/outX may alias (in == out is fine). Allocation-free.
        template <typename SectionFn>
        void process (const float* inL, const float* inR, float* outL, float* outR,
                      int n, SectionFn&& sectionFn) noexcept
        {
            if (n <= 0) return;

            if (! resampling)
            {
                for (int i = 0; i < n; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
                sectionFn (outL, outR, n);
                return;
            }

            const int namN0 = down[0].process (inL, n, namBuf[0].data(), namMaxBlk);
            const int namN1 = down[1].process (inR, n, namBuf[1].data(), namMaxBlk);
            const int namN  = std::min (namN0, namN1);

            sectionFn (namBuf[0].data(), namBuf[1].data(), namN);

            for (int ch = 0; ch < 2; ++ch)
            {
                const int hostM = up[(size_t) ch].process (namBuf[(size_t) ch].data(), namN,
                                                            upScratch[(size_t) ch].data(),
                                                            (int) upScratch[(size_t) ch].size());
                fifo[(size_t) ch].push (upScratch[(size_t) ch].data(), hostM);
            }
            fifo[0].pull (outL, n);
            fifo[1].pull (outR, n);
        }

    private:
        // Fixed-capacity ring buffering resampled output so exactly `blockSize` host
        // samples can be delivered each callback. Single audio thread; no locks.
        struct HostFifo
        {
            std::vector<float> buf;
            int mask = 0, rd = 0, wr = 0, count = 0;
            void prepare (int minSize)
            {
                int p = 1; while (p < minSize) p <<= 1;
                buf.assign ((size_t) p, 0.0f); mask = p - 1; rd = wr = count = 0;
            }
            void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); rd = wr = count = 0; }
            void pushZeros (int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = 0.0f; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void push (const float* x, int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = x[i]; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void pull (float* out, int n) noexcept
            {
                for (int i = 0; i < n; ++i) { if (count > 0) { out[i] = buf[(size_t) rd]; rd = (rd + 1) & mask; --count; } else out[i] = 0.0f; }
            }
        };

        double hostR = 0.0, modelR = 0.0;
        int    maxHost = 0, namMaxBlk = 0, reportedLatency = 0;
        bool   resampling = false;

        std::array<ResamplerT, 2>          down, up;
        std::array<std::vector<float>, 2>  namBuf, upScratch;
        std::array<HostFifo, 2>            fifo;
    };
} // namespace factory_core
