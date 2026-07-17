#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Dropdown.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>
#include <vector>

//
// factory_ui_visage::PresetSelectorView — the shared factory-preset picker,
// ported from ui/include/factory_ui/PresetSelector.h: a combo-style current-name
// display flanked by "<" / ">" step arrows. Since visage has no ComboBox, the
// dropdown is our own Dropdown overlay (requested through `requestDropdown`).
//
// It is a DUMB VIEW over a list model, exactly like the JUCE original: it renders
// whatever rows it is given and reports the user's chosen row through onChange as
// a 0-based ROW INDEX among selectable (item) rows only — headers/separators do
// not consume an index. `steppable` further restricts what the ARROWS may land on
// (an action row such as "Save As..." is directly pickable but never stepped to).
// The editor-side controller owns what a row means and the processor round-trip.
//
namespace factory_ui_visage
{
    class PresetSelectorView : public visage::Frame
    {
    public:
        struct Entry
        {
            enum class Kind { item, header, separator };

            static Entry item (std::string text, bool enabled = true, bool steppable = true)
            {
                Entry e; e.kind = Kind::item; e.text = std::move (text);
                e.enabled = enabled; e.steppable = steppable; return e;
            }
            static Entry header (std::string text)
            {
                Entry e; e.kind = Kind::header; e.text = std::move (text); return e;
            }
            static Entry separator() { Entry e; e.kind = Kind::separator; return e; }

            Kind        kind = Kind::item;
            std::string text;
            bool        enabled = true;
            bool        steppable = true;
        };

        explicit PresetSelectorView (const Theme& theme);

        // Flat, all-steppable menu (convenience — like PresetSelector::setItems).
        void setItems (const std::vector<std::string>& names, int selectedIndex);
        // The general form: mixed item/header/separator rows. selectedIndex is a
        // 0-based item-row index (-1 = none). Never fires onChange.
        void setMenu (std::vector<Entry> entries, int selectedIndex);

        void setSelectedIndex (int index); // no notification
        int  selectedIndex() const { return selectedItem_; }

        // Fired on a user-driven change (a direct pick or an arrow step landing on
        // a steppable row), with the new 0-based item-row index.
        std::function<void (int)> onChange;

        // Set by the host to present the shared Dropdown (see Dropdown.h).
        DropdownRequest requestDropdown;

        // Open the dropdown menu programmatically (same path as a combo click) —
        // used for deterministic dropdown capture in the harness.
        void openMenu();

        void draw (visage::Canvas& canvas) override;
        void resized() override;
        void mouseDown (const visage::MouseEvent& e) override;

    private:
        int  itemRowCount() const;
        const Entry* itemAt (int itemIndex) const; // item-row index -> Entry
        bool isSteppable (int itemIndex) const;
        void step (int delta);

        const Theme& theme_;
        std::vector<Entry> entries_;
        int selectedItem_ = -1;

        // Frame-local rects computed in resized().
        float prevX_ = 0, nextX_ = 0, arrowW_ = 0;
        float comboX_ = 0, comboW_ = 0;
    };
}
