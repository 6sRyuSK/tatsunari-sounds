#include "factory_ui_visage/PillToggle.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>

namespace factory_ui_visage
{
    PillToggle::PillToggle (factory_params::ParamStore& store, int paramIndex, const Theme& theme)
        : store_ (store), index_ (paramIndex), theme_ (theme)
    {
    }

    void PillToggle::draw (visage::Canvas& canvas)
    {
        const ToggleMetrics& tg = theme_.toggle;
        const factory_params::ParamDesc& desc = store_.desc (index_);
        const bool on = isOn();

        const float h = std::min (tg.height, height());
        const float boxW = h * tg.widthFactor;
        const float y = (height() - h) * 0.5f; // vertically centred pill
        const float inset = tg.knobInset * 0.5f;

        // Pill background: accent when on, track when off.
        canvas.setColor (visage::Color (on ? theme_.palette.accent : theme_.palette.track));
        canvas.roundedRectangle (0.0f, y, boxW, h, h * tg.cornerRadiusFactor);

        // White sliding knob.
        const float knobD = h - tg.knobInset;
        const float kx = on ? (boxW - knobD - inset) : inset;
        canvas.setColor (visage::Color (0xffffffff));
        canvas.circle (kx, y + inset, knobD);

        // Caption to the right (short override, else the parameter display name).
        canvas.setColor (visage::Color (theme_.palette.text));
        canvas.text (caption_.empty() ? desc.name : caption_,
                     boldFont (theme_.font.labelBold), visage::Font::kLeft,
                     boxW + tg.textGap, 0.0f, std::max (0.0f, width() - boxW - tg.textGap), height());
    }

    void PillToggle::mouseDown (const visage::MouseEvent&)
    {
        store_.setFromUiGestured (index_, isOn() ? 0.0f : 1.0f);
        redraw();
        if (onToggle) onToggle (isOn());
    }
}
