#include "factory_ui_visage/IconButton.h"
#include "factory_ui_visage/Fonts.h"

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
        directional_ = false; // a glyph replaces any directional A->B mode
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

    void IconButton::setDimmed (bool dim)
    {
        if (dimmed_ != dim)
        {
            dimmed_ = dim;
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

        // Directional A->B copy affordance (fix 4): fixed "A"/"B" letters with a
        // flipping arrow between them, drawn in the accent (dimmed when unavailable).
        // Geometry transcribed from the JUCE RsIconButton directional branch.
        if (directional_)
        {
            const std::uint32_t col = dimmed_ ? p.textDim : p.accent;
            const float ix = w * 0.08f, iy = h * 0.18f, iw = w * 0.84f, ih = h * 0.64f;
            const float lw = iw * 0.34f;                 // left/right letter columns
            const float lx = ix, rx = ix + iw - lw;
            const float mx = ix + lw, mw = iw - 2.0f * lw; // arrow column between them
            const float fsize = std::min (ih * 0.92f, lw * 1.15f);
            canvas.setColor (visage::Color (col));
            canvas.text ("A", boldFont (fsize), visage::Font::kCenter, lx, iy, lw, ih);
            canvas.text ("B", boldFont (fsize), visage::Font::kCenter, rx, iy, lw, ih);
            const float ay   = iy + ih * 0.5f;
            const float head = fsize * 0.28f;
            const float alen = std::min (mw * 0.82f, fsize * 1.15f);
            const float acx  = mx + mw * 0.5f;
            const float x0   = acx - alen * 0.5f, x1 = acx + alen * 0.5f;
            canvas.segment (x0, ay, x1, ay, 1.4f, /*rounded*/ true);
            if (reversedDir_) // B->A: arrowhead points left
            {
                canvas.segment (x0 + head, ay - head, x0, ay, 1.4f, true);
                canvas.segment (x0, ay, x0 + head, ay + head, 1.4f, true);
            }
            else              // A->B: arrowhead points right
            {
                canvas.segment (x1 - head, ay - head, x1, ay, 1.4f, true);
                canvas.segment (x1, ay, x1 - head, ay + head, 1.4f, true);
            }
            return;
        }

        // Glyph, tinted accent when on else the caption mid-tone (dimmed to the
        // muted tone when the owner has marked the action unavailable).
        const float inset = h * m.glyphInsetFactor;
        canvas.setColor (visage::Color (on_ ? p.accent : (dimmed_ ? p.textDim : p.textSecondary)));
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
