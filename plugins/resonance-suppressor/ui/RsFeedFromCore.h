#pragma once
//
// RsFeedFromCore.h — the REAL rs_ui::RsFeed backed by an rs_core::RsCore. It is
// the seam the CLAP shell (chunk 3) hands the Visage editor in place of the
// synthetic harness feed: a thin, zero-copy adapter that forwards the editor's
// analyser reads straight into the RsCore's published lock-free snapshots and
// routes the editor's two write hooks (Listen solo, display-time smoothing) into
// the RsCore's non-parameter side-channel.
//
// The three spectra return pointers straight into the RsCore's published
// std::array<std::atomic<float>, kMaxBins> buffers (pubMag / pubMagPre / pubRed),
// exactly as RsFeed.h documents for the real implementation — no copying, no
// per-call work; the editor reads element k with [k].load(relaxed). bins() and
// sampleRate() come from the RsCore's live display grid, so binFrequency() maps
// identically to the engine's binToHz().
//
// Header-only, framework-free, visage-free. GUI-thread only on the editor side;
// the atomic spectra are the lock-free hand-off from the audio thread. Chunk 3
// wires this into the editor — this header only provides + is unit-checked as a
// faithful 1:1 mapping of the RsCore read-outs onto the RsFeed contract.
//
#include "RsFeed.h"
#include "../RsCore.h"

namespace rs_ui
{
    class RsFeedFromCore final : public RsFeed
    {
    public:
        explicit RsFeedFromCore (rs_core::RsCore& core) noexcept : core_ (core) {}

        // Live analyser grid (RsCore tracks these across Quality switches).
        int    bins()       const noexcept override { return core_.numBins(); }
        double sampleRate() const noexcept override { return core_.sampleRate(); }

        // Per-bin spectra: pointers straight into the RsCore's published buffers.
        const std::atomic<float>* magDb()    const noexcept override { return core_.magnitudeDb(); }
        const std::atomic<float>* magPreDb() const noexcept override { return core_.magnitudePreDb(); }
        const std::atomic<float>* redDb()    const noexcept override { return core_.reductionDb(); }

        // Listen solo (editor -> core), same id convention as RsFeed / the core.
        void setListenNode (int nodeId) noexcept override { core_.setListenNode (nodeId); }
        int  getListenNode() const noexcept override      { return core_.getListenNode(); }

        // Analyser display time smoothing (ms, >= 0). Applied at the top of the
        // core's next process() block.
        void setDisplaySmoothMs (float ms) noexcept override { core_.setDisplaySmoothMs (ms); }

        // Optional info read-outs.
        int         latencySamples() const noexcept override { return core_.latencySamples(); }
        const char* qualityLabel()   const noexcept override { return core_.qualityLabel(); }

    private:
        rs_core::RsCore& core_;
    };
} // namespace rs_ui
