#pragma once

#include "factory_ui_visage/Theme.h"

#include <visage_graphics/canvas.h>

//
// factory_ui_visage::Chrome — shared background + card painters, so every editor
// paints the same warm-white gradient and soft rounded cards as the JUCE design
// system (factory_ui::paintBackground / paintCard). Draws onto a visage::Canvas
// in the caller frame's local coordinates.
//
namespace factory_ui_visage
{
    // Warm-white vertical gradient (background -> backgroundLo) filling the rect.
    void paintBackground (visage::Canvas& canvas, const Theme& theme,
                          float x, float y, float width, float height);

    // Soft drop shadow + white panel fill + track outline, per the card metrics.
    void paintCard (visage::Canvas& canvas, const Theme& theme,
                    float x, float y, float width, float height);

    // The bare "card + hairline" idiom shared by the small widget shells
    // (LinkSlider / ValueSetting / RsPillCell / the RS badges + chips): a rounded
    // fill plus the half-pixel-inset hairline border. Colours are explicit (with
    // alpha) so per-widget metrics stay at the call site — no theme lookup here.
    void paintCardShell (visage::Canvas& canvas, float x, float y, float width, float height,
                         float cornerRadius, visage::Color fill, visage::Color border,
                         float borderPx = 1.0f);

    // The hairline border alone — for shells whose fill is conditional or a
    // gradient (drawn by the caller) but whose border is the shared idiom.
    void paintHairline (visage::Canvas& canvas, float x, float y, float width, float height,
                        float cornerRadius, visage::Color colour, float borderPx = 1.0f);
}
