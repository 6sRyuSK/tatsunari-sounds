#pragma once

#include "Theme.h"

//
// factory_ui_visage::scaleThemeMetrics — a RUNTIME-only helper that returns a copy
// of a Theme with every PIXEL-DIMENSION field multiplied by a uniform scale `s`,
// leaving colours, ratios (…Factor / …Ratio), angles (arc radians), dB and alpha
// values UNTOUCHED. It exists so an editor that lays a design out at a reference
// size and then scales the whole window (RS's k() = height/747) can hand its
// widgets a theme whose px metrics (fonts, radii, paddings, track/glyph sizes …)
// track the window, instead of staying frozen at the design size and overflowing
// the shrinking cells.
//
// It is deliberately SEPARATE from the JSON layer: it does NOT touch Theme.cpp's
// parser / toJson / operator== (the theme document + its schema are the human
// taste-review surface and must stay byte-stable). At s == 1.0 it is the identity
// (float * 1.0f is exact), so `scaleThemeMetrics(t, 1.0f) == t`. Pure, header-only,
// JUCE-free and visage-free.
//
namespace factory_ui_visage
{
    inline Theme scaleThemeMetrics (const Theme& t, float s)
    {
        Theme o = t; // colours + non-px fields copied through verbatim

        // knob — ratios (lineWidthRatio / bodyInsetFactor / needleLengthRatio) and
        // the arc angles (radians) are dimensionless; only px sizes scale.
        o.knob.boundsInset      = t.knob.boundsInset      * s;
        o.knob.shadowBlurFactor = t.knob.shadowBlurFactor * s;
        o.knob.shadowOffsetX    = t.knob.shadowOffsetX    * s;
        o.knob.shadowOffsetY    = t.knob.shadowOffsetY    * s;
        o.knob.needleWidthPx    = t.knob.needleWidthPx    * s;

        // toggle — widthFactor / cornerRadiusFactor are ratios.
        o.toggle.height    = t.toggle.height    * s;
        o.toggle.knobInset = t.toggle.knobInset * s;
        o.toggle.textGap   = t.toggle.textGap   * s;

        o.card.cornerRadius  = t.card.cornerRadius  * s;
        o.card.outlineWidth  = t.card.outlineWidth  * s;
        o.card.shadowBlur    = t.card.shadowBlur    * s;
        o.card.shadowOffsetX = t.card.shadowOffsetX * s;
        o.card.shadowOffsetY = t.card.shadowOffsetY * s;

        o.font.label     = t.font.label     * s;
        o.font.labelBold = t.font.labelBold * s;
        o.font.title     = t.font.title     * s;
        o.font.callout   = t.font.callout   * s;
        o.font.caption   = t.font.caption   * s;

        o.segmented.height           = t.segmented.height           * s;
        o.segmented.cornerRadius     = t.segmented.cornerRadius     * s;
        o.segmented.pillInset        = t.segmented.pillInset        * s;
        o.segmented.pillCornerRadius = t.segmented.pillCornerRadius * s;

        o.dropdown.rowHeight      = t.dropdown.rowHeight      * s;
        o.dropdown.cornerRadius   = t.dropdown.cornerRadius   * s;
        o.dropdown.paddingX       = t.dropdown.paddingX       * s;
        o.dropdown.paddingY       = t.dropdown.paddingY       * s;
        o.dropdown.separatorInset = t.dropdown.separatorInset * s;
        o.dropdown.shadowBlur     = t.dropdown.shadowBlur     * s;
        o.dropdown.shadowOffsetY  = t.dropdown.shadowOffsetY  * s;

        // iconButton — glyphInsetFactor is a ratio.
        o.iconButton.cornerRadius = t.iconButton.cornerRadius * s;

        o.valueSetting.cornerRadius = t.valueSetting.cornerRadius * s;
        o.valueSetting.paddingX     = t.valueSetting.paddingX     * s;
        o.valueSetting.iconSize     = t.valueSetting.iconSize     * s;

        o.linkSlider.cornerRadius  = t.linkSlider.cornerRadius  * s;
        o.linkSlider.paddingX      = t.linkSlider.paddingX      * s;
        o.linkSlider.trackHeight   = t.linkSlider.trackHeight   * s;
        o.linkSlider.trackCorner   = t.linkSlider.trackCorner   * s;
        o.linkSlider.captionColumn = t.linkSlider.captionColumn * s;
        o.linkSlider.valueColumn   = t.linkSlider.valueColumn   * s;
        o.linkSlider.glyphSize     = t.linkSlider.glyphSize     * s;

        // spectrum — topDb / bottomDb (dB) and the fill alphas are dimensionless.
        o.spectrum.cornerRadius = t.spectrum.cornerRadius * s;
        o.spectrum.traceWidth   = t.spectrum.traceWidth   * s;
        o.spectrum.peakWidth    = t.spectrum.peakWidth    * s;

        return o;
    }
}
