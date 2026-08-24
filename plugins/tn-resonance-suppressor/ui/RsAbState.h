#pragma once
//
// RsAbState.h — the real A/B-compare backing for the RsAbModel interface, faithful
// to the shipping JUCE 2.1.0 editor's setABSlot / copyActiveToOther:
//
//   * TWO session-only slots, each holding the FULL parameter state PLUS the
//     program (preset) index — exactly the scope the JUCE processor's A/B stashed
//     (stateToXml = APVTS params + presetIndex), and the same scope a host save
//     restores, minus transient execution state (Listen, display smoothing).
//   * setActiveSlot(slot): stash the slot being LEFT, switch active, then restore
//     the target slot — or, on its first visit, seed it from the current live state.
//   * copyActiveToOther(): copy the active slot's LIVE state onto the other slot's
//     storage (active + live params unchanged).
//
// Framework-free: it drives a factory_params::ParamStore for the parameter values
// and reaches the program index through two callbacks, so the CLAP shell backs the
// program hooks with its factory_presets::PresetSession (currentProgram /
// setCurrentProgramClean) while a headless test drives them with plain lambdas.
// Parameters are written through ParamStore::setFromHost (bulk, non-gesture — an
// A/B switch must NOT emit per-parameter automation; the editor relays a single
// host rescan instead), matching the JUCE apvts.replaceState() path.
//
#include "RsModels.h"

#include "factory_params/ParamStore.h"

#include <array>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace rs_ui
{
    class AbCompareModel final : public RsAbModel
    {
    public:
        // `getProgram` / `setProgram` read + write the current program index (the
        // shell wires these to PresetSession); either may be empty (program index
        // then defaults to 0 and restore is a no-op on it). `onSwitched` fires after
        // a slot switch completes (the editor relays a host rescan + mark-dirty).
        AbCompareModel (factory_params::ParamStore& store,
                        std::function<int()>       getProgram,
                        std::function<void (int)>  setProgram,
                        std::function<void()>      onSwitched = {})
            : store_ (store),
              getProgram_ (std::move (getProgram)),
              setProgram_ (std::move (setProgram)),
              onSwitched_ (std::move (onSwitched))
        {
        }

        int activeSlot() const override { return active_; }

        void setActiveSlot (int slot) override
        {
            if (slot == active_ || slot < 0 || slot > 1)
                return;

            slot_[static_cast<std::size_t> (active_)] = capture(); // stash the slot we leave
            seeded_[static_cast<std::size_t> (active_)] = true;
            active_ = slot;

            if (seeded_[static_cast<std::size_t> (slot)])
                restore (slot_[static_cast<std::size_t> (slot)]);  // return to a visited slot
            else
            {
                slot_[static_cast<std::size_t> (slot)] = capture(); // first visit: seed from current
                seeded_[static_cast<std::size_t> (slot)] = true;
            }

            if (onSwitched_)
                onSwitched_();
        }

        void copyActiveToOther() override
        {
            const int other = 1 - active_;
            slot_[static_cast<std::size_t> (other)] = capture(); // active's live state -> other slot
            seeded_[static_cast<std::size_t> (other)] = true;
        }

    private:
        struct Slot
        {
            std::vector<float> values;
            int                program = 0;
        };

        Slot capture() const
        {
            Slot s;
            s.values.resize (static_cast<std::size_t> (store_.size()));
            for (int i = 0; i < store_.size(); ++i)
                s.values[static_cast<std::size_t> (i)] = store_.value (i);
            s.program = getProgram_ ? getProgram_() : 0;
            return s;
        }

        void restore (const Slot& s)
        {
            for (int i = 0; i < store_.size() && i < static_cast<int> (s.values.size()); ++i)
                store_.setFromHost (i, s.values[static_cast<std::size_t> (i)]);
            if (setProgram_)
                setProgram_ (s.program);
        }

        factory_params::ParamStore& store_;
        std::function<int()>        getProgram_;
        std::function<void (int)>   setProgram_;
        std::function<void()>       onSwitched_;

        int                       active_ = 0;
        std::array<Slot, 2>       slot_ {};
        std::array<bool, 2>       seeded_ { { false, false } };
    };
}
