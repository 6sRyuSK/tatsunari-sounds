#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Icons.h"

#include <algorithm>

namespace factory_ui_visage
{
    PresetSelectorView::PresetSelectorView (const Theme& theme) : theme_ (theme) {}

    void PresetSelectorView::setItems (const std::vector<std::string>& names, int selectedIndex)
    {
        std::vector<Entry> entries;
        entries.reserve (names.size());
        for (const std::string& n : names)
            entries.push_back (Entry::item (n));
        setMenu (std::move (entries), selectedIndex);
    }

    void PresetSelectorView::setMenu (std::vector<Entry> entries, int selectedIndex)
    {
        entries_ = std::move (entries);
        setSelectedIndex (selectedIndex);
    }

    void PresetSelectorView::setSelectedIndex (int index)
    {
        const int n = itemRowCount();
        selectedItem_ = (index >= 0 && index < n) ? index : -1;
        redraw();
    }

    int PresetSelectorView::itemRowCount() const
    {
        int n = 0;
        for (const Entry& e : entries_)
            if (e.kind == Entry::Kind::item)
                ++n;
        return n;
    }

    const PresetSelectorView::Entry* PresetSelectorView::itemAt (int itemIndex) const
    {
        int i = 0;
        for (const Entry& e : entries_)
            if (e.kind == Entry::Kind::item)
            {
                if (i == itemIndex)
                    return &e;
                ++i;
            }
        return nullptr;
    }

    bool PresetSelectorView::isSteppable (int itemIndex) const
    {
        const Entry* e = itemAt (itemIndex);
        return e != nullptr && e->enabled && e->steppable;
    }

    void PresetSelectorView::step (int delta)
    {
        const int n = itemRowCount();
        if (n <= 0)
            return;
        const int start = selectedItem_;
        int candidate = start;
        for (;;)
        {
            const int next = candidate + delta;
            if (next < 0 || next >= n)
                return; // ran off the end without finding a steppable row
            candidate = next;
            if (isSteppable (candidate))
                break;
        }
        if (candidate != start)
        {
            setSelectedIndex (candidate);
            if (onChange)
                onChange (candidate);
        }
    }

    void PresetSelectorView::openMenu()
    {
        if (! requestDropdown)
            return;

        std::vector<Dropdown::Item> items;
        items.reserve (entries_.size());
        for (const Entry& e : entries_)
        {
            switch (e.kind)
            {
                case Entry::Kind::item:      items.push_back (Dropdown::Item::make (e.text, e.enabled)); break;
                case Entry::Kind::header:    items.push_back (Dropdown::Item::header (e.text)); break;
                case Entry::Kind::separator: items.push_back (Dropdown::Item::separator()); break;
            }
        }

        requestDropdown (std::move (items), selectedItem_, this,
                         [this] (int chosen)
                         {
                             setSelectedIndex (chosen);
                             if (onChange)
                                 onChange (chosen);
                         });
    }

    void PresetSelectorView::resized()
    {
        arrowW_ = std::min (24.0f, height());
        prevX_  = 0.0f;
        nextX_  = width() - arrowW_;
        // The combo occupies the middle, JUCE reduced(4,0).
        comboX_ = arrowW_ + 4.0f;
        comboW_ = std::max (0.0f, width() - 2.0f * arrowW_ - 8.0f);
    }

    void PresetSelectorView::draw (visage::Canvas& canvas)
    {
        const Palette& p = theme_.palette;
        const float h = height();

        // Combo box: white panel + hairline outline.
        canvas.setColor (visage::Color (p.panel));
        canvas.roundedRectangle (comboX_, 0.0f, comboW_, h, 8.0f);
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (comboX_ + 0.5f, 0.5f, comboW_ - 1.0f, h - 1.0f, 8.0f, 1.0f);

        // Current name, centred; a caret affordance on the right of the box.
        const Entry* cur = itemAt (selectedItem_);
        canvas.setColor (visage::Color (p.text));
        canvas.text (cur != nullptr ? cur->text : std::string ("—"),
                     boldFont (theme_.font.labelBold), visage::Font::kCenter,
                     comboX_ + 10.0f, 0.0f, comboW_ - 28.0f, h);
        canvas.setColor (visage::Color (p.accent));
        icons::paintGlyph (canvas, icons::caret(), comboX_ + comboW_ - 22.0f, 0.0f, 16.0f, h);

        // Prev / next arrows (coral), disabled-dim when no steppable row remains.
        auto drawArrow = [&] (float x, const char* txt, bool enabled)
        {
            canvas.setColor (visage::Color (enabled ? p.accent : p.textDim));
            canvas.text (txt, boldFont (theme_.font.title), visage::Font::kCenter, x, 0.0f, arrowW_, h);
        };
        bool canPrev = false, canNext = false;
        for (int i = selectedItem_ - 1; i >= 0; --i) if (isSteppable (i)) { canPrev = true; break; }
        for (int i = selectedItem_ + 1, n = itemRowCount(); i < n; ++i) if (isSteppable (i)) { canNext = true; break; }
        drawArrow (prevX_, "<", canPrev);
        drawArrow (nextX_, ">", canNext);
    }

    void PresetSelectorView::mouseDown (const visage::MouseEvent& e)
    {
        // e.position is the frame-local click point (relativePosition() is a delta).
        const float x = e.position.x;
        if (x < prevX_ + arrowW_)
            step (-1);
        else if (x >= nextX_)
            step (+1);
        else
            openMenu();
    }
}
