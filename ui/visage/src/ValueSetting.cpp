#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>
#include <utility>

namespace factory_ui_visage
{
    ValueSetting::ValueSetting (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                                icons::Glyph glyph, std::string caption)
        : store_ (store), index_ (paramIndex), theme_ (theme),
          glyph_ (std::move (glyph)), caption_ (std::move (caption))
    {
    }

    int ValueSetting::currentIndex() const
    {
        const int n = static_cast<int> (store_.desc (index_).choices.size());
        return std::clamp (static_cast<int> (store_.value (index_)), 0, std::max (0, n - 1));
    }

    void ValueSetting::draw (visage::Canvas& canvas)
    {
        const ValueSettingMetrics& m = theme_.valueSetting;
        const Palette& p = theme_.palette;
        const float w = width();
        const float h = height();

        // White card + hairline.
        canvas.setColor (visage::Color (p.panel));
        canvas.roundedRectangle (0.0f, 0.0f, w, h, m.cornerRadius);
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, m.cornerRadius, 1.0f);

        // Icon (left), caption (left of the value), value (right, accent).
        const float pad = m.paddingX;
        canvas.setColor (visage::Color (p.textSecondary));
        icons::paintGlyph (canvas, glyph_, pad, (h - m.iconSize) * 0.5f, m.iconSize, m.iconSize);

        const float textX = pad + m.iconSize + 6.0f;
        const float textW = std::max (0.0f, w - textX - pad);
        canvas.setColor (visage::Color (p.textSecondary));
        canvas.text (caption_, boldFont (theme_.font.caption), visage::Font::kLeft,
                     textX, 0.0f, textW, h);

        const std::vector<std::string>& choices = store_.desc (index_).choices;
        const int idx = currentIndex();
        canvas.setColor (visage::Color (p.accent));
        canvas.text (idx < static_cast<int> (choices.size()) ? choices[static_cast<std::size_t> (idx)] : std::string(),
                     boldFont (theme_.font.callout), visage::Font::kRight, textX, 0.0f, textW, h);
    }

    void ValueSetting::openMenu()
    {
        if (! requestDropdown)
            return;

        std::vector<Dropdown::Item> items;
        const std::vector<std::string>& choices = store_.desc (index_).choices;
        items.reserve (choices.size());
        for (const std::string& c : choices)
            items.push_back (Dropdown::Item::make (c));

        requestDropdown (std::move (items), currentIndex(), this,
                         [this] (int chosen)
                         {
                             store_.setFromUiGestured (index_, static_cast<float> (chosen));
                             redraw();
                         });
    }

    void ValueSetting::mouseDown (const visage::MouseEvent&)
    {
        openMenu();
    }
}
