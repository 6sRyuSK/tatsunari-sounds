#pragma once

#include "factory_params/ParamStore.h"
#include "factory_core/ReductionProfile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

//
// rs_ui::RsProfileModel — the reduction-node model shared by the suppression-curve
// view and the node-panel, ported from the node/param mapping in
// SuppressionCurveComponent + PluginProcessor (readNodes / cutPid / bandPid /
// slopeValue). It is a thin, JUCE-free adapter over a factory_params::ParamStore:
// it caches the parameter index of every node field once, exposes typed reads, and
// assembles a factory_core::ReductionNodes so the editor curve evaluates the SAME
// factory_core::reductionProfileDbAt() shape functions the audio rasteriser uses
// (single source of truth — what you see is what runs).
//
// Node id convention (matching the processor): 0 = low cut, 1 = high cut,
// 2..(1+kNumBands) = bands 0.. . Cuts carry on/freq/slope; bands carry
// on/freq/type/sens/width.
//
namespace rs_ui
{
    class RsProfileModel
    {
    public:
        static constexpr int   kNumBands = 8;
        static constexpr int   kNumNodes = 2 + kNumBands; // LC, HC, 8 bands
        static constexpr float kSensMin  = -30.0f;
        static constexpr float kSensMax  = 30.0f;

        explicit RsProfileModel (factory_params::ParamStore& store) : store_ (store)
        {
            for (int id = 0; id < kNumNodes; ++id)
            {
                NodeIdx& n = idx_[(std::size_t) id];
                n.on   = store_.indexOf (pid (id, "on"));
                n.freq = store_.indexOf (pid (id, "freq"));
                if (isCut (id))
                    n.slope = store_.indexOf (pid (id, "slope"));
                else
                {
                    n.type  = store_.indexOf (pid (id, "type"));
                    n.sens  = store_.indexOf (pid (id, "sens"));
                    n.width = store_.indexOf (pid (id, "width"));
                }
            }
        }

        factory_params::ParamStore& store() noexcept { return store_; }

        static bool isCut (int id) noexcept { return id < 2; }

        // --- param indices (for gesture writes through the store) --------------
        int idxOn    (int id) const noexcept { return idx_[(std::size_t) id].on; }
        int idxFreq  (int id) const noexcept { return idx_[(std::size_t) id].freq; }
        int idxSens  (int id) const noexcept { return idx_[(std::size_t) id].sens; }
        int idxWidth (int id) const noexcept { return idx_[(std::size_t) id].width; }
        int idxType  (int id) const noexcept { return idx_[(std::size_t) id].type; }
        int idxSlope (int id) const noexcept { return idx_[(std::size_t) id].slope; }

        // --- typed reads -------------------------------------------------------
        bool  nodeOn    (int id) const noexcept { return store_.value (idxOn (id)) > 0.5f; }
        float nodeFreq  (int id) const noexcept { return store_.value (idxFreq (id)); }
        float nodeSens  (int id) const noexcept { return isCut (id) ? 0.0f : store_.value (idxSens (id)); }
        float nodeWidth (int id) const noexcept { return isCut (id) ? 0.5f : store_.value (idxWidth (id)); }
        int   nodeType  (int id) const noexcept { return isCut (id) ? 0 : (int) store_.value (idxType (id)); }
        int   cutSlope  (int id) const noexcept { return isCut (id) ? (int) store_.value (idxSlope (id)) : 0; }

        // Cut slope choice index (0..3) -> dB/oct (mirrors processor slopeValue).
        static double slopeValue (int index) noexcept
        {
            static constexpr double kSlopes[] = { 6.0, 12.0, 24.0, 48.0 };
            return kSlopes[(std::size_t) std::clamp (index, 0, 3)];
        }

        // Assemble the core node config from the live parameter values.
        factory_core::ReductionNodes buildNodes() const
        {
            factory_core::ReductionNodes n;
            n.lowCut  = { nodeOn (0), (double) nodeFreq (0), slopeValue (cutSlope (0)) };
            n.highCut = { nodeOn (1), (double) nodeFreq (1), slopeValue (cutSlope (1)) };
            for (int b = 0; b < kNumBands; ++b)
            {
                const int id = 2 + b;
                n.bands[(std::size_t) b] = { nodeOn (id), (double) nodeFreq (id),
                                             (factory_core::ReductionBandType) nodeType (id),
                                             (double) nodeSens (id), (double) nodeWidth (id) };
            }
            return n;
        }

        // A config with ONLY node `id` enabled (its own contribution) — for the
        // per-node curve fills (mirrors SuppressionCurveComponent::singleNode).
        static factory_core::ReductionNodes singleNode (const factory_core::ReductionNodes& all, int id)
        {
            factory_core::ReductionNodes one;
            if      (id == 0) one.lowCut  = all.lowCut;
            else if (id == 1) one.highCut = all.highCut;
            else              one.bands[(std::size_t) (id - 2)] = all.bands[(std::size_t) (id - 2)];
            return one;
        }

    private:
        static std::string pid (int id, const char* suffix)
        {
            if (isCut (id))
                return std::string (id == 0 ? "lc_" : "hc_") + suffix;
            return "b" + std::to_string (id - 2) + "_" + suffix;
        }

        struct NodeIdx { int on = -1, freq = -1, sens = -1, width = -1, type = -1, slope = -1; };

        factory_params::ParamStore& store_;
        std::array<NodeIdx, kNumNodes> idx_ {};
    };
}
