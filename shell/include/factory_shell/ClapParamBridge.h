#pragma once
//
// factory_shell/ClapParamBridge.h — maps a factory_params ParamDesc table onto
// the CLAP `clap.params` surface. This is the DSP-agnostic, plugin-agnostic half
// of the shell (no Core, no Policy, no templates), so it is a plain compiled unit
// in shell/src and is unit-testable on its own.
//
// PARAM-ID DECISION (load-bearing): the CLAP `clap_id` for a parameter is its
// ParamDesc.uid == fnv1a32(id) — the stable, CLAP-era identifier the params model
// already pins (params_test golden values). This is self-consistent and stable
// across builds.
//   CUTOVER TODO (out of scope for this coexistence chunk): to preserve a user's
//   existing *automation lanes* when a project migrates from the shipping VST3 to
//   this CLAP/wrapped-VST3, the clap_id would instead have to match the shipping
//   build's VST3 param tag (the framework hashes the paramID string into a 31-bit
//   VST3 ParamID via its own algorithm). Reconciling uid vs that VST3 tag is a
//   deliberate cutover refinement; here (a parallel, flag-gated build) we use uid.
//
// EXPOSURE: which parameters appear on the CLAP surface is a per-plugin decision
// handed in as a predicate (nullptr => expose all). A plugin excludes parameters
// it keeps registered only for the shipping build's automation-lane / old-session
// compatibility (which the DSP no longer consumes). Excluded parameters are still
// persisted by the state codec (the ParamStore stays the single value store), they
// are just not host-automatable via CLAP.
//
#include "factory_params/ParamDesc.h"

#include <clap/clap.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace factory_shell
{
    class ParamBridge
    {
    public:
        // Predicate deciding whether a parameter is on the CLAP surface. Called
        // only at construction (main thread), once per parameter.
        using ExposePredicate = bool (*) (const factory_params::ParamDesc&);

        // `descs` must outlive the bridge (the shell keeps the ParamDesc table as a
        // member and hands the same vector to both the ParamStore and this bridge).
        // `isExposed` may be nullptr to expose every parameter.
        ParamBridge (const std::vector<factory_params::ParamDesc>& descs, ExposePredicate isExposed);

        // Number of CLAP-exposed parameters.
        std::uint32_t count() const noexcept { return static_cast<std::uint32_t> (exposed.size()); }

        // Fill clap_param_info for the exposed parameter at `paramIndex` (0..count-1).
        bool getInfo (std::uint32_t paramIndex, clap_param_info_t* info) const noexcept;

        // Map a CLAP clap_id (== uid) back to the FULL ParamStore index, or -1.
        // Real-time safe: binary search over a prebuilt sorted table, no allocation.
        int storeIndexForId (clap_id id) const noexcept;

        // The FULL ParamStore index behind an exposed CLAP param index (or -1).
        int storeIndexForParamIndex (std::uint32_t paramIndex) const noexcept;

        // value_to_text / text_to_value against the FULL-table descriptor `storeIndex`.
        void valueToText (int storeIndex, double value, char* out, std::uint32_t cap) const;
        bool textToValue (int storeIndex, const char* text, double* outValue) const;

        // Display decimals implied by a float param's interval (continuous -> 2).
        static int decimalsFor (const factory_params::ParamDesc& d) noexcept;

    private:
        const std::vector<factory_params::ParamDesc>* table = nullptr;
        std::vector<int>                              exposed;   // FULL indices, exposed order
        std::vector<std::pair<std::uint32_t, int>>    byUid;     // sorted (uid, FULL index), exposed only
    };
} // namespace factory_shell
