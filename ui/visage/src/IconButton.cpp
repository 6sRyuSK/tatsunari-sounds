#include "factory_ui_visage/IconButton.h"

#include <algorithm>
#include <utility>

namespace factory_ui_visage
{
    IconButton::IconButton (const Theme& theme, icons::Glyph glyph, Mode mode)
        : theme_ (theme), glyph_ (std::move (glyph)), mode_ (mode)
    {
    }

    void IconButton::setGlyph (icons::Glyph glyph)
    {
        glyph_ = std::move (glyph);
        redraw();
    }

    void IconButton::setToggleState (bool on)
    {
        if (on_ != on)
        {
            on_ = on;
            redraw();
        }
    }

    void IconButton::draw (visage::Canvas& canvas)
    {
        const IconButtonMetrics& m = theme_.iconButton;
        const Palette& p = theme_.palette;
        const float w = width();
        const float h = height();

        // Background: accent-dim when toggled on; a soft white lift on hover/press.
        if (on_)
        {
            canvas.setColor (visage::Color (p.accentDim));
            canvas.roundedRectangle (0.0f, 0.0f, w, h, m.cornerRadius);
        }
        else if (hover_ || down_)
        {
            canvas.setColor (visage::Color (p.panel).withAlpha (down_ ? 0.95f : 0.7f));
            canvas.roundedRectangle (0.0f, 0.0f, w, h, m.cornerRadius);
            canvas.setColor (visage::Color (p.track));
            canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, m.cornerRadius, 1.0f);
        }

        // Glyph, tinted accent when on else the caption mid-tone.
        const float inset = h * m.glyphInsetFactor;
        canvas.setColor (visage::Color (on_ ? p.accent : p.textSecondary));
        icons::paintGlyph (canvas, glyph_, inset, inset, w - 2.0f * inset, h - 2.0f * inset);
    }

    void IconButton::mouseDown (const visage::MouseEvent&)
    {
        down_ = true;
        if (mode_ == Mode::toggle)
        {
            on_ = ! on_;
            if (onToggle)
                onToggle (on_);
        }
        if (onClick)
            onClick();
        redraw();
    }

    void IconButton::mouseUp (const visage::MouseEvent&)
    {
        if (down_)
        {
            down_ = false;
            redraw();
        }
    }

    void IconButton::mouseEnter (const visage::MouseEvent&)
    {
        hover_ = true;
        redraw();
    }

    void IconButton::mouseExit (const visage::MouseEvent&)
    {
        hover_ = false;
        down_ = false;
        redraw();
    }
}
