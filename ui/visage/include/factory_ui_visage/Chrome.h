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
}
