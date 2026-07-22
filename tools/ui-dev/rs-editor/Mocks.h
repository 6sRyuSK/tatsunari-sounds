#pragma once

#include "RsModels.h"

#include "factory_params/ParamStore.h"

#include <array>
#include <string>
#include <vector>

//
// Harness mocks for the RS editor's preset + A/B interfaces.
//
// MockPresetModel exposes Init + the SIX factory preset NAMES (transcribed from
// FactoryPresets.h — names only, per the task; the real curated parameter values
// are pending human audition sign-off and are NOT reproduced here) plus a trailing
// "Save As…" action row. A mock "load" resets every parameter to its default —
// enough to demonstrate the wiring (state replaced -> the editor clears undo)
// without fabricating preset values.
//
// MockAbModel keeps two in-memory snapshots of the ParamStore and swaps them with
// the JUCE setABSlot semantics (stash current -> switch -> load slot, seeding a
// slot from the current state on its first visit).
//
namespace rs_harness
{
    class MockPresetModel : public rs_ui::RsPresetModel
    {
    public:
        explicit MockPresetModel (factory_params::ParamStore& store) : store_ (store) {}

        std::vector<rs_ui::RsPresetItem> items() const override
        {
            std::vector<rs_ui::RsPresetItem> v;
            for (const char* n : kNames)
                v.push_back ({ n, /*steppable*/ true, /*isAction*/ false });
            v.push_back ({ "Save As\xE2\x80\xA6", /*steppable*/ false, /*isAction*/ true });
            return v;
        }

        int currentIndex() const override { return current_; }

        bool load (int index) override
        {
            if (index < 0 || index >= kNumNames)
                return false; // the "Save As…" action row (or out of range)
            for (int i = 0; i < store_.size(); ++i)
                store_.setFromHost (i, store_.desc (i).defaultValue);
            current_ = index;
            return true;
        }

    private:
        static constexpr int kNumNames = 7;
        static constexpr const char* kNames[kNumNames] = {
            "Init", "Vocal De-Harsh", "Full Mix Tame", "Gentle Smooth",
            "Aggressive Cut", "De-Harsh M/S", "Sibilance Tamer"
        };
        factory_params::ParamStore& store_;
        int current_ = 0;
    };

    class MockAbModel : public rs_ui::RsAbModel
    {
    public:
        explicit MockAbModel (factory_params::ParamStore& store) : store_ (store) {}

        int activeSlot() const override { return active_; }

        void setActiveSlot (int slot) override
        {
            if (slot == active_ || slot < 0 || slot > 1) return;
            slotState_[(std::size_t) active_] = snapshot();
            seeded_[(std::size_t) active_] = true;
            active_ = slot;
            if (seeded_[(std::size_t) slot])
                apply (slotState_[(std::size_t) slot]);
            else
            {
                slotState_[(std::size_t) slot] = snapshot(); // first visit: seed from current
                seeded_[(std::size_t) slot] = true;
            }
        }

        void copyActiveToOther() override
        {
            const int other = 1 - active_;
            slotState_[(std::size_t) other] = snapshot();
            seeded_[(std::size_t) other] = true;
        }

    private:
        std::vector<float> snapshot() const
        {
            std::vector<float> v ((std::size_t) store_.size());
            for (int i = 0; i < store_.size(); ++i) v[(std::size_t) i] = store_.value (i);
            return v;
        }
        void apply (const std::vector<float>& s)
        {
            for (int i = 0; i < store_.size() && i < (int) s.size(); ++i)
                store_.setFromHost (i, s[(std::size_t) i]);
        }

        factory_params::ParamStore& store_;
        int active_ = 0;
        std::array<std::vector<float>, 2> slotState_ {};
        std::array<bool, 2> seeded_ { { false, false } };
    };
}
