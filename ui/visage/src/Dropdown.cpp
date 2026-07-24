#include "factory_ui_visage/Dropdown.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Icons.h"

#include <algorithm>

namespace factory_ui_visage
{
    namespace
    {
        constexpr float kAnchorGap    = 4.0f;  // gap between the anchor and the panel
        constexpr float kMinPanelW    = 168.0f;
        constexpr float kSeparatorH   = 9.0f;
    }

    Dropdown::Dropdown (const Theme& theme) : theme_ (theme)
    {
        setVisible (false);
        setAcceptsKeystrokes (true);
    }

    void Dropdown::layout (float anchorX, float anchorY, float anchorW, float anchorH)
    {
        const DropdownMetrics& d = theme_.dropdown;

        panelW_ = std::max (kMinPanelW, anchorW);

        // Build the visual rows and measure the content height.
        rows_.clear();
        float top = 0.0f;
        int   itemCounter = 0;
        for (const Item& it : items_)
        {
            const float h = (it.kind == Item::Kind::separator) ? kSeparatorH : d.rowHeight;
            const int idx = (it.kind == Item::Kind::item) ? itemCounter++ : -1;
            rows_.push_back ({ top, h, idx });
            top += h;
        }
        panelH_ = top + 2.0f * d.paddingY;

        // Prefer below the anchor; flip above if it would overflow the bottom.
        const float below = anchorY + anchorH + kAnchorGap;
        if (below + panelH_ <= height() || anchorY - kAnchorGap - panelH_ < 0.0f)
            panelY_ = below;
        else
            panelY_ = anchorY - kAnchorGap - panelH_;

        panelX_ = std::clamp (anchorX, 0.0f, std::max (0.0f, width() - panelW_));
    }

    void Dropdown::open (std::vector<Item> items, int selectedItemIndex,
                         float anchorX, float anchorY, float anchorW, float anchorH)
    {
        items_ = std::move (items);
        selectedItem_ = selectedItemIndex;
        layout (anchorX, anchorY, anchorW, anchorH);

        // Highlight the current item's row, else the first enabled item.
        hoverRow_ = -1;
        for (std::size_t r = 0; r < rows_.size(); ++r)
            if (rows_[r].itemIndex == selectedItem_) { hoverRow_ = (int) r; break; }
        if (hoverRow_ < 0)
            hoverRow_ = firstEnabledItemRow (+1, -1);

        open_ = true;
        setVisible (true);
        setOnTop (true);
        requestKeyboardFocus();
        redraw();
    }

    void Dropdown::close()
    {
        if (! open_)
            return;
        open_ = false;
        setVisible (false);
        redraw();
        if (onClose)
            onClose();
    }

    int Dropdown::itemCount() const
    {
        int n = 0;
        for (const Item& it : items_)
            if (it.kind == Item::Kind::item)
                ++n;
        return n;
    }

    bool Dropdown::rowCentreInWindow (int itemIndex, float& x, float& y) const
    {
        if (! open_)
            return false;
        const float contentTop = panelY_ + theme_.dropdown.paddingY;
        for (const Row& row : rows_)
            if (row.itemIndex == itemIndex)
            {
                const visage::Point o = positionInWindow();
                x = o.x + panelX_ + panelW_ * 0.5f;
                y = o.y + contentTop + row.top + row.height * 0.5f;
                return true;
            }
        return false;
    }

    int Dropdown::rowAtY (float y) const
    {
        const float contentTop = panelY_ + theme_.dropdown.paddingY;
        for (std::size_t r = 0; r < rows_.size(); ++r)
            if (y >= contentTop + rows_[r].top && y < contentTop + rows_[r].top + rows_[r].height)
                return (int) r;
        return -1;
    }

    int Dropdown::firstEnabledItemRow (int dir, int fromVisualRow) const
    {
        const int n = (int) rows_.size();
        int r = fromVisualRow;
        for (int step = 0; step < n; ++step)
        {
            r += dir;
            if (r < 0 || r >= n)
                return dir > 0 ? -1 : -1;
            if (rows_[(std::size_t) r].itemIndex >= 0 && items_[(std::size_t) r].enabled)
                return r;
        }
        return -1;
    }

    void Dropdown::selectVisualRow (int visualRow)
    {
        if (visualRow < 0 || visualRow >= (int) rows_.size())
            return;
        const Row& row = rows_[(std::size_t) visualRow];
        if (row.itemIndex < 0 || ! items_[(std::size_t) visualRow].enabled)
            return;
        const int chosen = row.itemIndex;
        close();
        if (onSelect)
            onSelect (chosen);
    }

    void Dropdown::draw (visage::Canvas& canvas)
    {
        if (! open_)
            return;

        const DropdownMetrics& d = theme_.dropdown;
        const Palette& p = theme_.palette;

        // Card: soft drop shadow, panel fill, hairline outline.
        canvas.setColor (visage::Color (p.shadow));
        canvas.roundedRectangleShadow (panelX_, panelY_ + d.shadowOffsetY, panelW_, panelH_,
                                       d.cornerRadius, d.shadowBlur);
        canvas.setColor (visage::Color (p.panel));
        canvas.roundedRectangle (panelX_, panelY_, panelW_, panelH_, d.cornerRadius);
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (panelX_ + 0.5f, panelY_ + 0.5f, panelW_ - 1.0f, panelH_ - 1.0f,
                                       d.cornerRadius, 1.0f);

        const float contentTop = panelY_ + d.paddingY;
        const float rowX = panelX_ + d.paddingX;
        const float rowW = panelW_ - 2.0f * d.paddingX;

        for (std::size_t r = 0; r < rows_.size(); ++r)
        {
            const Row&  row = rows_[r];
            const Item& it  = items_[r];
            const float ry  = contentTop + row.top;

            if (it.kind == Item::Kind::separator)
            {
                canvas.setColor (visage::Color (p.track));
                canvas.rectangle (panelX_ + d.separatorInset, ry + row.height * 0.5f - 0.5f,
                                  panelW_ - 2.0f * d.separatorInset, 1.0f);
                continue;
            }

            if (it.kind == Item::Kind::header)
            {
                canvas.setColor (visage::Color (p.textDim));
                canvas.text (it.text, boldFont (theme_.font.caption), visage::Font::kLeft,
                             rowX, ry, rowW, row.height);
                continue;
            }

            // Item row: selected (accentDim) or hovered (accentDim) highlight.
            const bool selected = (row.itemIndex == selectedItem_);
            const bool hovered  = ((int) r == hoverRow_) && it.enabled;
            if (selected || hovered)
            {
                canvas.setColor (visage::Color (p.accentDim).withAlpha (hovered ? 1.0f : 0.6f));
                canvas.roundedRectangle (panelX_ + 3.0f, ry + 1.0f, panelW_ - 6.0f, row.height - 2.0f, 6.0f);
            }
            float textX = rowX;
            float textW = rowW;
            if (it.hasIcon)
            {
                const float gsz = 16.0f;
                canvas.setColor (visage::Color (it.enabled ? p.textSecondary : p.textDim));
                icons::paintGlyph (canvas, it.icon, rowX, ry + (row.height - gsz) * 0.5f, gsz, gsz);
                textX = rowX + gsz + 8.0f;
                textW = std::max (0.0f, rowW - gsz - 8.0f);
            }
            canvas.setColor (visage::Color (it.enabled ? p.text : p.textDim));
            canvas.text (it.text, regularFont (theme_.font.label), visage::Font::kLeft,
                         textX, ry, textW, row.height);
        }
    }

    void Dropdown::mouseDown (const visage::MouseEvent& e)
    {
        if (! open_)
            return;
        // Frame-local hit point (the dropdown covers the whole parent, so this is
        // also the overlay-local coordinate); relativePosition() is a move delta.
        const visage::Point pos = e.position;
        const bool insidePanel = pos.x >= panelX_ && pos.x < panelX_ + panelW_
                              && pos.y >= panelY_ && pos.y < panelY_ + panelH_;
        if (! insidePanel)
        {
            close(); // outside click dismisses
            return;
        }
        const int r = rowAtY (pos.y);
        if (r >= 0 && rows_[(std::size_t) r].itemIndex >= 0 && items_[(std::size_t) r].enabled)
            selectVisualRow (r);
        // A click on a header/separator/disabled row keeps the panel open.
    }

    void Dropdown::mouseMove (const visage::MouseEvent& e)
    {
        if (! open_)
            return;
        const visage::Point pos = e.position;
        const int r = rowAtY (pos.y);
        const int newHover = (r >= 0 && rows_[(std::size_t) r].itemIndex >= 0 && items_[(std::size_t) r].enabled) ? r : -1;
        if (newHover != hoverRow_)
        {
            hoverRow_ = newHover;
            redraw();
        }
    }

    void Dropdown::mouseExit (const visage::MouseEvent&)
    {
        if (open_ && hoverRow_ != -1)
        {
            hoverRow_ = -1;
            redraw();
        }
    }

    bool Dropdown::keyPress (const visage::KeyEvent& e)
    {
        if (! open_)
            return false;

        switch (e.keyCode())
        {
            case visage::KeyCode::Escape:
                close();
                return true;
            case visage::KeyCode::Up:
            {
                const int r = firstEnabledItemRow (-1, hoverRow_ < 0 ? (int) rows_.size() : hoverRow_);
                if (r >= 0) { hoverRow_ = r; redraw(); }
                return true;
            }
            case visage::KeyCode::Down:
            {
                const int r = firstEnabledItemRow (+1, hoverRow_);
                if (r >= 0) { hoverRow_ = r; redraw(); }
                return true;
            }
            case visage::KeyCode::Return:
                selectVisualRow (hoverRow_);
                return true;
            default:
                return false;
        }
    }
}
