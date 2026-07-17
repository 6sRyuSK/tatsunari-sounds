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
            canvas.setColor (visage::Color (active ? p.panel : p.textDim));
            canvas.text (labels[static_cast<std::size_t> (i)], boldFont (theme_.font.labelBold),
                         visage::Font::kCenter, cx, y, segW, h);
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
            store_.beginGesture (index_);
            store_.setFromUi (index_, static_cast<float> (seg));
            store_.endGesture (index_);
            redraw();
        }
    }
}
