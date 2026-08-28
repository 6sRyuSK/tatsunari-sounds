#pragma once

#include <string>
#include <vector>

//
// rs_ui — the small callback interfaces the RsEditor binds to for the two pieces
// of state it does NOT own directly: factory presets and the A/B compare slots.
// The harness supplies deterministic mocks; P4's shell supplies real ones backed
// by factory_presets / the processor's setABSlot / copyActiveToOther.
//
// RsPresetModel reuses PresetSelectorView's list-model semantics: it is a DUMB
// list the editor renders through PresetSelectorView, reporting a chosen item-row
// index back. Loading a preset (or switching A/B) mutates the ParamStore via the
// HOST path (setFromHost — no UI gesture, so it never lands in the undo history)
// and the editor clears its undo timeline afterwards, matching the JUCE editor
// (apvts.replaceState() -> undoManager.clearUndoHistory()).
//
namespace rs_ui
{
    // One selector row. `isAction` marks a non-loading row (e.g. "Save As...") that
    // the arrows skip and that never clears undo history.
    struct RsPresetItem
    {
        std::string name;
        bool        steppable = true;
        bool        isAction  = false;
    };

    class RsPresetModel
    {
    public:
        virtual ~RsPresetModel() = default;

        // The flat selector list (Init + factory presets + a trailing "Save As…").
        virtual std::vector<RsPresetItem> items() const = 0;

        // Current selection (item-row index, -1 = none).
        virtual int currentIndex() const = 0;

        // Apply the row at `index`. Returns true if it LOADED a preset (the editor
        // then clears undo history); false for a non-loading action row.
        virtual bool load (int index) = 0;
    };

    class RsAbModel
    {
    public:
        virtual ~RsAbModel() = default;

        virtual int  activeSlot() const = 0;         // 0 = A, 1 = B
        virtual void setActiveSlot (int slot) = 0;    // stash current -> switch -> load slot
        virtual void copyActiveToOther() = 0;         // copy active slot's state onto the other
    };
}
