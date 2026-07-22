#pragma once

#include "factory_ui_visage/Theme.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>
#include <vector>

//
// factory_ui_visage::Dropdown — the overlay list panel the design system uses
// wherever the old JUCE UI leaned on juce::ComboBox / juce::PopupMenu (visage has
// no combo). It is a self-contained popup: items, section headers, separators,
// disabled rows, a hover highlight (accentDim) and the selected row marked, drawn
// as a rounded card with the theme's card shadow.
//
// It works as a full-cover **scrim**: when opened it sizes itself to its parent,
// sits on top, and paints ONLY the panel at the anchor — so a click anywhere
// outside the panel closes it (the classic dropdown dismissal) while clicks on a
// row select. Keyboard: Esc closes; Up/Down move the highlight; Enter selects.
//
// A Dropdown is meant to live at the TOP of the hierarchy (one shared overlay per
// editor) so its panel can extend past the small control that triggered it; the
// triggering widget hands it items + an anchor frame + a completion callback (see
// PresetSelectorView / ValueSetting).
//
namespace factory_ui_visage
{
    class Dropdown : public visage::Frame
    {
    public:
        struct Item
        {
            enum class Kind { item, header, separator };

            static Item make (std::string text, bool enabled = true)
            {
                return { Kind::item, std::move (text), enabled };
            }
            static Item header (std::string text) { return { Kind::header, std::move (text), true }; }
            static Item separator() { return { Kind::separator, {}, true }; }

            Kind        kind = Kind::item;
            std::string text;
            bool        enabled = true;
        };

        explicit Dropdown (const Theme& theme);

        // Open with `items`, highlighting the item-row `selectedItemIndex` (index
        // among ITEM rows only, headers/separators excluded — matching onSelect),
        // anchored under `anchorLocal` (a rect in THIS frame's coordinates). The
        // caller must have sized this frame to cover the overlay area first.
        void open (std::vector<Item> items, int selectedItemIndex,
                   float anchorX, float anchorY, float anchorW, float anchorH);
        void close();
        bool isOpen() const { return open_; }

        // Number of ITEM rows (headers/separators excluded).
        int itemCount() const;
        // Centre (window px) of the item-row with item-index `itemIndex`, so a
        // driver can aim a real click at a specific row. False if closed/not found.
        bool rowCentreInWindow (int itemIndex, float& x, float& y) const;

        // Fired with the chosen 0-based ITEM-row index (headers/separators skipped).
        std::function<void (int)> onSelect;
        // Fired whenever the panel closes (selection or dismissal).
        std::function<void()> onClose;

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseMove (const visage::MouseEvent& e) override;
        void mouseExit (const visage::MouseEvent& e) override;
        bool keyPress (const visage::KeyEvent& e) override;

    private:
        struct Row { float top; float height; int itemIndex; }; // itemIndex = -1 for header/separator

        void  layout (float anchorX, float anchorY, float anchorW, float anchorH);
        int   rowAtY (float y) const;             // visual-row index, or -1
        int   firstEnabledItemRow (int dir, int fromVisualRow) const;
        void  selectVisualRow (int visualRow);

        const Theme& theme_;
        std::vector<Item> items_;
        std::vector<Row>  rows_;
        int   selectedItem_ = -1;   // item-row index highlighted as "current"
        int   hoverRow_ = -1;       // visual-row index under the pointer/keyboard
        bool  open_ = false;

        float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
    };

    // How a small control asks a shared, top-level Dropdown to present itself:
    // (items, currently-selected item-row, anchor frame, on-select callback). The
    // editor/gallery owns the Dropdown and wires this so the panel can overflow
    // the tiny control that triggered it. Used by PresetSelectorView / ValueSetting.
    using DropdownRequest =
        std::function<void (std::vector<Dropdown::Item>, int, visage::Frame*, std::function<void (int)>)>;
}
