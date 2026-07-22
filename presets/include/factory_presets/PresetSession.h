#pragma once
//
// factory_presets/PresetSession.h — the framework-free core of the program/preset
// machinery that today is split across ProgramAdapter.h (the program list, Init
// synthesis, exclusion set, per-program apply) and the editor's
// PresetSelectorController (dirty tracking, the self-triggered-change guard),
// with all host glue removed. It drives a factory_params::ParamStore in REAL
// units and holds no framework, GUI, or file-system dependency, so the CLAP shell
// and a headless test can share exactly this logic.
//
// PROGRAM LIST — index 0 is the synthesised "Init" (every managed parameter to
// its default), indices 1..N are the PresetBank entries in order. As with
// ProgramAdapter, the bank stores only presets 1..N; Init is never stored.
//
// APPLY — applyProgram(index) writes each MANAGED parameter's real value into the
// store: the preset's value where the preset lists it, its descriptor default
// otherwise (so no residue from the previously selected program). EXCLUDED
// parameters (the kExclude set — bypass, monitoring toggles, …) are never written,
// by any program including Init, even if a preset table happens to list one. It
// writes through ParamStore::setFromHost, which snaps/clamps to the legal grid and
// bumps the change epoch, so the values land identical to a host-driven edit.
//
// DIRTY — a program is clean immediately after applyProgram; isDirty() reports
// whether any managed parameter has since diverged from what that program wrote
// (compared bit-exactly against the snapped values it stored). Excluded parameters
// never affect dirtiness. Editing a managed value back to the applied value clears
// it again.
//
// suppressNextAutoSync — the one-shot latch PresetSelectorController uses so a
// program change THIS session drove is not echoed back by the host's follow-up
// notification. Set it right before driving the host; the host-follow path calls
// consumeNextAutoSync() and skips its resync when it returns true.
//
#include "factory_presets/PresetBank.h"
#include "factory_params/ParamStore.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace factory_presets
{
    class PresetSession
    {
    public:
        // Drives `storeToDrive` (not owned; must outlive the session). `excludeIds`
        // are parameters presets must never touch. The initial baseline for dirty
        // tracking is the store's current values at program 0 (Init) — after
        // restoring a saved state, the shell should re-establish it via
        // applyProgram or setCurrentProgramClean().
        PresetSession (factory_params::ParamStore& storeToDrive,
                       const PresetBank& presetBank,
                       const std::vector<std::string>& excludeIds = {})
            : store (storeToDrive), bank (presetBank)
        {
            numProgramsValue = 1 + (bank.numPresets > 0 ? bank.numPresets : 0);

            const std::size_t np = static_cast<std::size_t> (store.size());
            managed.assign (np, 1);
            for (const auto& id : excludeIds)
            {
                const int idx = store.indexOf (id);
                if (idx >= 0)
                    managed[static_cast<std::size_t> (idx)] = 0;
            }

            appliedTargets.resize (np);
            for (int i = 0; i < store.size(); ++i)
                appliedTargets[static_cast<std::size_t> (i)] = store.value (i);
        }

        // --- program list -----------------------------------------------------
        int numPrograms()    const noexcept { return numProgramsValue; }
        int currentProgram() const noexcept { return currentIndex; }

        std::string programName (int index) const
        {
            if (index == 0)
                return "Init";
            if (bank.presets != nullptr && index >= 1 && index <= bank.numPresets)
                return bank.presets[index - 1].name;
            return {};
        }

        bool isExcluded (std::string_view id) const
        {
            const int idx = store.indexOf (id);
            return idx >= 0 && managed[static_cast<std::size_t> (idx)] == 0;
        }

        // --- apply ------------------------------------------------------------
        // The (paramIndex, real value) pairs written, in managed-parameter order —
        // the shell relays them to the host the way ProgramAdapter's setCurrentProgram
        // does. Out-of-range index is a no-op returning an empty list.
        struct Applied { int index; float value; };

        std::vector<Applied> applyProgram (int index)
        {
            std::vector<Applied> writes;
            if (index < 0 || index >= numProgramsValue)
                return writes;

            for (int i = 0; i < store.size(); ++i)
            {
                if (! managed[static_cast<std::size_t> (i)])
                    continue;

                const auto& d = store.desc (i);
                float real = d.defaultValue;

                if (index >= 1 && bank.presets != nullptr)
                {
                    const Preset& pr = bank.presets[index - 1];
                    for (int e = 0; e < pr.numParams; ++e)
                        if (d.id == pr.params[e].paramID)
                        {
                            real = pr.params[e].value;
                            break;
                        }
                }

                store.setFromHost (i, real);                 // snaps + clamps + bumps epoch
                const float stored = store.value (i);
                appliedTargets[static_cast<std::size_t> (i)] = stored;
                writes.push_back ({ i, stored });
            }

            currentIndex = index;
            return writes;
        }

        // Adopt `index` as the current program WITHOUT writing parameters, taking
        // the store's present values as the clean baseline (used after a state
        // restore, where the parameters are already in place).
        void setCurrentProgramClean (int index)
        {
            if (index >= 0 && index < numProgramsValue)
                currentIndex = index;
            for (int i = 0; i < store.size(); ++i)
                appliedTargets[static_cast<std::size_t> (i)] = store.value (i);
        }

        // --- dirty ------------------------------------------------------------
        bool isDirty() const
        {
            for (int i = 0; i < store.size(); ++i)
            {
                if (! managed[static_cast<std::size_t> (i)])
                    continue;
                if (! bitEqual (store.value (i), appliedTargets[static_cast<std::size_t> (i)]))
                    return true;
            }
            return false;
        }

        // --- self-triggered-change guard -------------------------------------
        void suppressNextAutoSync() noexcept { suppress = true; }
        bool consumeNextAutoSync()  noexcept { const bool v = suppress; suppress = false; return v; }

    private:
        static bool bitEqual (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        factory_params::ParamStore& store;
        const PresetBank&           bank;

        int  numProgramsValue = 1;
        int  currentIndex     = 0;
        bool suppress         = false;

        std::vector<char>  managed;         // 1 = preset-controlled, 0 = excluded
        std::vector<float> appliedTargets;  // last-applied snapped value per parameter
    };
}
