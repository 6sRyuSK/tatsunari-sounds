#include "factory_ui_visage/Segmented.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>

namespace factory_ui_visage
{
    Segmented::Segmented (factory_params::ParamStore& store, int paramIndex, const Theme& theme)
        : store_ (store), index_ (paramIndex), theme_ (theme)
    {
    }

    int Segmented::segmentCount() const
    {
        return static_cast<int> (store_.desc (index_).choices.size());
    }

    int Segmented::currentIndex() const
    {
        const int n = segmentCount();
        int idx = static_cast<int> (store_.value (index_));
        return std::clamp (idx, 0, std::max (0, n - 1));
    }

    void Segmented::draw (visage::Canvas& canvas)
    {
        const SegmentedMetrics& sg = theme_.segmented;
        const Palette& p = theme_.palette;

        const float h = std::min (sg.height, height());
        const float y = (height() - h) * 0.5f;
        const float w = width();

        // Track background.
        canvas.setColor (visage::Color (p.panelLo));
        canvas.roundedRectangle (0.0f, y, w, h, sg.cornerRadius);

        const int n = segmentCount();
        if (n <= 0)
            return;

        const float segW = w / static_cast<float> (n);
        const int sel = currentIndex();
        const std::vector<std::string>& labels = store_.desc (index_).choices;
        const float labelPx = labelFontPx_ > 0.0f ? labelFontPx_ : theme_.font.labelBold;

        for (int i = 0; i < n; ++i)
        {
            const float cx = i * segW;
            const bool active = (i == sel);
            if (active)
            {
                canvas.setColor (visage::Color (p.accent));
                canvas.roundedRectangle (cx + sg.pillInset, y + sg.pillInset,
                                         segW - 2.0f * sg.pillInset, h - 2.0f * sg.pillInset,
                                         sg.pillCornerRadius);
            }
            const std::string& label = labels[static_cast<std::size_t> (i)];
            const std::uint32_t fg = active ? p.panel : p.textDim;
            if (i < static_cast<int> (glyphs_.size()))
            {
                // Leading glyph + label, centred as a group (approx label width).
                const float gs = 14.0f;
                const float textW = 7.0f * static_cast<float> (label.size());
                const float groupW = gs + 5.0f + textW;
                const float gx = cx + (segW - groupW) * 0.5f;
                canvas.setColor (visage::Color (fg));
                icons::paintGlyph (canvas, glyphs_[static_cast<std::size_t> (i)], gx, y + (h - gs) * 0.5f, gs, gs);
                canvas.setColor (visage::Color (fg));
                canvas.text (label, boldFont (labelPx), visage::Font::kLeft,
                             gx + gs + 5.0f, y, cx + segW - (gx + gs + 5.0f), h);
            }
            else
            {
                canvas.setColor (visage::Color (fg));
                canvas.text (label, boldFont (labelPx), visage::Font::kCenter, cx, y, segW, h);
            }
        }
    }

    void Segmented::mouseDown (const visage::MouseEvent& e)
    {
        const int n = segmentCount();
        if (n <= 0)
            return;
        // e.position is the click's frame-local coordinate (window_position -
        // positionInWindow); relativePosition() is a movement delta, not a hit point.
        const int seg = std::clamp (static_cast<int> (e.position.x / (width() / static_cast<float> (n))),
                                    0, n - 1);
        if (seg != currentIndex())
        {
            store_.setFromUiGestured (index_, static_cast<float> (seg));
            redraw();
        }
    }
}
