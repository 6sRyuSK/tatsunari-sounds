#pragma once
//
// OfflineReamp — JUCE-free, deterministic offline render of the NAM Player wet chain
// for "reamp-pair" export (the MERGE feature). It renders a mono 48 kHz input through
// the exact same factory_core building blocks the live plugin uses, so the exported
// output/input pair can be trained into a single .nam that reproduces the whole
// 3-slot chain. Being JUCE-free, it is unit-tested headless with a mock nonlinearity.
//
// What it renders (the "always captured" set): the NamRoutingEngine — slot enable,
// Series/Parallel routing, per-slot In Gain and Level. The mono L-channel path is
// used with Balance forced to centre (fed L == R, take L). Optionally (the
// "Include Cab IR + Tone" switch) it also applies the cab IR convolution + IR Level
// and the Lo/Hi-Cut tone. Global I/O (Input Trim / Output / Mix) is deliberately NOT
// applied — those are outside the captured amp+cab.
//
// Determinism: the engine's parameter smoothers are snapped to their targets so the
// output depends only on the configuration, not on ramp state. Off the audio thread,
// so allocation is fine; processed in chunks to bound working memory.
//
#include "factory_core/NamRoutingEngine.h"
#include "factory_core/PartitionedConvolver.h"
#include "factory_core/Biquad.h"

#include <algorithm>
#include <functional>
#include <vector>

class OfflineReamp
{
public:
    // Render `input` (mono, 48 kHz) through `engine` (already prepared for
    // (48k, chunk), models set, slots set with Balance == 0). When `includeIrTone`,
    // the IR (irConv+irKernel, gained by irLevelLin) and the Lo/Hi-Cut biquads
    // (loCut/hiCut, either may be null) are applied after the engine. `onProgress`
    // (optional) is called with 0..1 after each chunk. Returns the mono output.
    static std::vector<float> render (factory_core::NamRoutingEngine& engine,
                                      int chunk,
                                      const std::vector<float>& input,
                                      bool includeIrTone,
                                      factory_core::PartitionedConvolver* irConv,
                                      const factory_core::PartitionedConvolver::Kernel* irKernel,
                                      float irLevelLin,
                                      const factory_core::BiquadCoeffs* loCut,
                                      const factory_core::BiquadCoeffs* hiCut,
                                      const std::function<void (float)>& onProgress = {})
    {
        const int N = (int) input.size();
        std::vector<float> out ((size_t) std::max (0, N), 0.0f);
        if (N <= 0) return out;

        const int blk = std::max (1, chunk);
        engine.snap();                                   // deterministic: no ramp state

        factory_core::Biquad lc, hc;
        if (loCut != nullptr) lc.setCoeffs (*loCut);
        if (hiCut != nullptr) hc.setCoeffs (*hiCut);

        std::vector<float> bufL ((size_t) blk), bufR ((size_t) blk);
        const bool useIr = includeIrTone && irConv != nullptr
                        && irKernel != nullptr && ! irKernel->head.empty();

        for (int off = 0; off < N; off += blk)
        {
            const int m = std::min (blk, N - off);
            for (int i = 0; i < m; ++i) { bufL[(size_t) i] = input[(size_t) (off + i)]; bufR[(size_t) i] = bufL[(size_t) i]; }

            // Mono L-path (Balance centred): both channels identical, take L.
            engine.process (bufL.data(), bufR.data(), bufL.data(), bufR.data(), m);

            if (includeIrTone)
            {
                if (useIr)
                {
                    irConv->process (bufL.data(), m, *irKernel);
                    for (int i = 0; i < m; ++i) bufL[(size_t) i] *= irLevelLin;
                }
                if (loCut != nullptr) lc.process (bufL.data(), m);
                if (hiCut != nullptr) hc.process (bufL.data(), m);
            }

            for (int i = 0; i < m; ++i) out[(size_t) (off + i)] = bufL[(size_t) i];
            if (onProgress) onProgress ((float) (off + m) / (float) N);
        }
        return out;
    }
};
