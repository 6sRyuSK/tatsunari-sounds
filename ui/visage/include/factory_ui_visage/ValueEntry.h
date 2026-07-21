#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ValueText.h" // ValueEntryRequest / ValueEntryOpener / stripLeadingNumber

#include <functional>
#include <string>

#include <visage_widgets/text_editor.h>
#include <visage_graphics/palette.h>

//
// factory_ui_visage::ValueEntry — a theme-consistent, single-line text-entry
// overlay (a thin wrapper over visage::TextEditor) for direct value entry on a
// control's value read-out, ported from the shipped JUCE RsKnob / RsLinkSlider
// Label valueEditor. ONE instance is owned by the editor and re-positioned
// over whichever read-out was double-clicked (the same shared-overlay pattern as
// the Dropdown), requested by a control through the ValueEntryOpener hook.
//
// Semantics mirror the JUCE Label editor exactly (setEditable(false, true, false)):
//   * opens on DOUBLE-CLICK, pre-filled with the current value as a bare number
//     (stripLeadingNumber) and SELECT-ALL;
//   * Enter and focus-loss COMMIT (lossOfFocusDiscardsChanges = false), Esc CANCELS;
//   * re-submitting the unchanged pre-fill is a no-op (no undo entry — the Label's
//     own dedup);
//   * the commit routes through the control's ParamStore gesture path (begin / set /
//     end), which snaps + clamps to the parameter range — so out-of-range input is
//     clamped and non-numeric input yields the range-clamped 0 (JUCE getValueFromText).
//
namespace factory_ui_visage
{
    // (ValueEntryRequest / ValueEntryOpener / stripLeadingNumber moved to
    // ValueText.h — the visage-free half of the entry flow — included above.)

    class ValueEntry : public visage::TextEditor
    {
    public:
        explicit ValueEntry (const Theme& theme);

        // Open over `x,y,w,h` (this frame's PARENT-local coords — the editor converts
        // from the request's window px), pre-filled + selected, `commit` fired on
        // Enter / focus-loss. `fontPx` sizes the entry text to the value read-out.
        void open (float x, float y, float w, float h, const std::string& prefill,
                   float fontPx, std::function<void (const std::string&)> commit);

        // Force-close WITHOUT committing (external — e.g. the editor closes/rebinds a
        // control while an edit is in flight; mirrors RsKnob::closeValueEditor()).
        void cancelEntry();

        bool isOpen() const noexcept { return open_; }
        std::string currentText() const { return text().toUtf8(); }

        void resized() override;
        void focusChanged (bool is_focused, bool was_clicked) override;

    private:
        void doCommit(); // Enter / focus-loss
        void finish();   // hide + reset state

        const Theme& theme_;
        visage::Palette palette_;
        std::function<void (const std::string&)> commit_;
        std::string prefill_;
        bool open_ = false;
        bool finishing_ = false;
    };
}
