#pragma once

#include "RsTheme.h"

#include "factory_ui_visage/Icons.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <algorithm>
#include <string>
#include <utility>

//
// rs_ui::RsPillCell — a resonance-suppressor footer pill cell (DELTA / S-CHAIN /
// SC LISTEN / LINK) and the header Bypass, ported from the RsPillToggle-in-a-cell
// composition in PluginEditor.cpp: a white card + leading glyph + caption with a
// right-aligned pill toggle, where a click ANYWHERE in the cell toggles the bound
// bool parameter. The shared factory_ui_visage::PillToggle draws a different
// composition (pill-left + caption-right), so this RS-specific cell layout is
// composed here — bound to the ParamStore by id and writing through the UI gesture
// path exactly like the shared widgets (so the editor's undo sees it). The `bare`
// form (no card, no glyph) is the header Bypass row. GUI-thread only.
//
namespace rs_ui
{
    class RsPillCell : public visage::Frame
    {
    public:
        RsPillCell (factory_params::ParamStore& store, int paramIndex, const RsTheme& theme,
                    factory_ui_visage::icons::Glyph glyph, std::string caption,
                    std::uint32_t onColour, bool card = true, bool hasGlyph = true)
            : store_ (store), index_ (paramIndex), theme_ (theme), glyph_ (std::move (glyph)),
              caption_ (std::move (caption)), onColour_ (onColour), card_ (card), hasGlyph_ (hasGlyph)
        {
        }

        int paramIndex() const noexcept { return index_; }

        void draw (visage::Canvas& canvas) override
        {
            namespace fuv = factory_ui_visage;
            const auto& p = theme_.base.palette;
            const float w = width(), h = height();
            const bool on = store_.value (index_) > 0.5f;

            if (card_)
            {
                canvas.setColor (visage::Color (0xffffffff));
                canvas.roundedRectangle (0.0f, 0.0f, w, h, theme_.rs.radiusBadge);
                canvas.setColor (visage::Color (p.track));
                canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, theme_.rs.radiusBadge, 1.0f);
            }

            const float pillW = 34.0f, pillH = 19.0f, rightInset = 9.0f;
            const float pillX = w - rightInset - pillW, pillY = (h - pillH) * 0.5f;

            float x = card_ ? 9.0f : 0.0f;
            if (hasGlyph_)
            {
                canvas.setColor (visage::Color (p.textSecondary));
                fuv::icons::paintGlyph (canvas, glyph_, x, (h - 16.0f) * 0.5f, 16.0f, 16.0f);
                x += 16.0f + 6.0f;
            }
            const float capW = std::max (0.0f, pillX - x - 4.0f);
            canvas.setColor (visage::Color (p.textSecondary));
            canvas.text (caption_, fuv::boldFont (theme_.base.font.caption),
                         card_ ? visage::Font::kLeft : visage::Font::kRight, x, 0.0f, capW, h);

            // pill
            canvas.setColor (visage::Color (on ? onColour_ : theme_.rs.toggleOffBg));
            canvas.roundedRectangle (pillX, pillY, pillW, pillH, pillH * 0.5f);
            const float knobD = pillH - 4.0f;
            const float kx = on ? (pillX + pillW - knobD - 2.0f) : (pillX + 2.0f);
            canvas.setColor (visage::Color (0xffffffff));
            canvas.circle (kx, pillY + 2.0f, knobD);
        }

        void mouseDown (const visage::MouseEvent&) override
        {
            const bool on = store_.value (index_) > 0.5f;
            store_.setFromUiGestured (index_, on ? 0.0f : 1.0f);
            redraw();
        }

    private:
        factory_params::ParamStore& store_;
        int index_;
        const RsTheme& theme_;
        factory_ui_visage::icons::Glyph glyph_;
        std::string caption_;
        std::uint32_t onColour_;
        bool card_;
        bool hasGlyph_;
    };
}
