#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Icons.h"

#include <algorithm>
#include <string>
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
        paintCardShell (canvas, 0.0f, 0.0f, w, h, m.cornerRadius,
                        visage::Color (p.panel), visage::Color (p.track));

        const float pad = m.paddingX;
        const std::vector<std::string>& choices = store_.desc (index_).choices;
        const int idx = currentIndex();
        const std::string value =
            idx < static_cast<int> (choices.size()) ? choices[static_cast<std::size_t> (idx)] : std::string();

        // COMBO mode (empty caption): an optional leading glyph (setChoiceIcons — the
        // band type's filter shape) + the choice text + a down-caret, a plain combo box
        // like the JUCE ComboBox. Text is a light regular weight (same as the plain
        // slope/channel combos) so it does not shout next to the knobs; dimmed when
        // disabled. LABELLED mode (non-empty caption): icon + caption + value (the RS /
        // pitch-fix row look).
        if (caption_.empty())
        {
            const float caret = 10.0f;
            const std::uint32_t fg = enabled_ ? p.text : p.textDim;
            float textX = pad;
            if (! choiceIcons_.empty() && idx < static_cast<int> (choiceIcons_.size()))
            {
                const float gsz = std::min (h - 8.0f, 16.0f);
                canvas.setColor (visage::Color (fg));
                icons::paintGlyph (canvas, choiceIcons_[static_cast<std::size_t> (idx)],
                                   pad, (h - gsz) * 0.5f, gsz, gsz);
                textX = pad + gsz + 5.0f;
            }
            const float valW = std::max (0.0f, w - textX - pad - caret - 2.0f);
            canvas.setColor (visage::Color (fg));
            canvas.text (value, regularFont (theme_.font.label), visage::Font::kLeft, textX, 0.0f, valW, h);
            canvas.setColor (visage::Color (p.textDim));
            icons::paintGlyph (canvas, icons::caret(), w - pad - caret, (h - caret) * 0.5f, caret, caret);
            return;
        }

        // Icon (left), caption (left of the value), value (right, accent).
        canvas.setColor (visage::Color (p.textSecondary));
        icons::paintGlyph (canvas, glyph_, pad, (h - m.iconSize) * 0.5f, m.iconSize, m.iconSize);

        const float textX = pad + m.iconSize + 6.0f;
        const float textW = std::max (0.0f, w - textX - pad);
        canvas.setColor (visage::Color (p.textSecondary));
        canvas.text (caption_, boldFont (theme_.font.caption), visage::Font::kLeft,
                     textX, 0.0f, textW, h);

        canvas.setColor (visage::Color (p.accent));
        canvas.text (value, boldFont (theme_.font.callout), visage::Font::kRight, textX, 0.0f, textW, h);
    }

    void ValueSetting::openMenu()
    {
        if (! requestDropdown)
            return;

        std::vector<Dropdown::Item> items;
        const std::vector<std::string>& choices = store_.desc (index_).choices;
        items.reserve (choices.size());
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            if (! choiceIcons_.empty() && i < choiceIcons_.size())
                items.push_back (Dropdown::Item::makeIcon (choices[i], choiceIcons_[i]));
            else
                items.push_back (Dropdown::Item::make (choices[i]));
        }

        requestDropdown (std::move (items), currentIndex(), this,
                         [this] (int chosen)
                         {
                             store_.setFromUiGestured (index_, static_cast<float> (chosen));
                             redraw();
                             if (onChange) onChange();
                         });
    }

    void ValueSetting::mouseDown (const visage::MouseEvent&)
    {
        if (! enabled_) return;
        openMenu();
    }
}
