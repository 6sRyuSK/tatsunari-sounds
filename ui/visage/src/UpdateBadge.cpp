#include "factory_ui_visage/UpdateBadge.h"
#include "factory_ui_visage/Fonts.h"

namespace factory_ui_visage
{
    UpdateBadge::UpdateBadge (const Theme& theme) : theme_ (theme)
    {
        setVisible (false);
    }

    void UpdateBadge::setLabel (const std::string& text)
    {
        if (label_ == text)
            return;
        label_ = text;
        redraw();
    }

    void UpdateBadge::draw (visage::Canvas& canvas)
    {
        const float w = width(), h = height();
        const float r = h * 0.5f;
        const auto fill = visage::Color (hover_ ? theme_.palette.accent : theme_.palette.accentDim);
        const auto text = visage::Color (hover_ ? 0xffffffffu : theme_.palette.text);
        canvas.setColor (fill);
        canvas.roundedRectangle (0.0f, 0.0f, w, h, r);
        canvas.setColor (text);
        canvas.text (label_.c_str(), boldFont (theme_.font.caption),
                     visage::Font::kCenter, 0.0f, 0.0f, w, h);
    }

    void UpdateBadge::mouseDown (const visage::MouseEvent&)
    {
        if (onClick)
            onClick();
    }

    void UpdateBadge::mouseEnter (const visage::MouseEvent&)
    {
        hover_ = true;
        redraw();
    }

    void UpdateBadge::mouseExit (const visage::MouseEvent&)
    {
        hover_ = false;
        redraw();
    }
}
