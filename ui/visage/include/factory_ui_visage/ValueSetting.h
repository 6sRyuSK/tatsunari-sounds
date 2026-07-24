#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Icons.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_params/ParamStore.h"

#include <functional>
#include <string>
#include <vector>

#include <visage_ui/frame.h>

//
// factory_ui_visage::ValueSetting — an icon + caption + value row that opens a
// Dropdown of all choices on click, ported from rs::RsValueSetting. Draws its own
// white card; binds to a Choice parameter in a ParamStore (reads store.value as
// the choice index, writes via the UI gesture path). The choice list comes from
// the parameter's `choices`. The popup is the shared Dropdown, requested through
// `requestDropdown` (see Dropdown.h).
//
namespace factory_ui_visage
{
    class ValueSetting : public visage::Frame
    {
    public:
        ValueSetting (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                      icons::Glyph glyph, std::string caption);

        // Set by the host to present the shared Dropdown.
        DropdownRequest requestDropdown;

        // Called after a menu selection commits (mirrors juce::ComboBox::onChange) —
        // lets a panel react to the new choice (e.g. enable/disable a dependent
        // control). Not fired for programmatic value changes.
        std::function<void()> onChange;

        int paramIndex() const { return index_; }
        // Re-point at a different Choice parameter (a per-band panel rebinds to the
        // selected band). The choice list is re-read from the new desc on draw.
        void rebind (int paramIndex) { index_ = paramIndex; redraw(); }

        // COMBO mode only: draw a glyph per choice (indexed by choice value) instead
        // of the choice text — e.g. an EQ band type shown as its filter shape. The
        // caret affordance is kept. Empty (default) = draw the choice text.
        void setChoiceIcons (std::vector<icons::Glyph> glyphs) { choiceIcons_ = std::move (glyphs); redraw(); }

        // Dim + make non-interactive (mirrors juce::Component::setEnabled): a disabled
        // setting reads greyed and ignores clicks (e.g. slope is inapplicable to a
        // bell/shelf band). No-op if unchanged.
        void setEnabled (bool enabled) { if (enabled != enabled_) { enabled_ = enabled; redraw(); } }
        bool isEnabled() const { return enabled_; }

        // Open the choice menu programmatically (same path as a click) — used for
        // deterministic dropdown capture in the harness.
        void openMenu();

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;

    private:
        int currentIndex() const;

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
        icons::Glyph glyph_;
        std::string caption_;
        std::vector<icons::Glyph> choiceIcons_; // combo mode: glyph per choice (empty = text)
        bool enabled_ = true;
    };
}
