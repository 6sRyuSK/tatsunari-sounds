#include "factory_ui_visage/Chrome.h"

namespace factory_ui_visage
{
    void paintBackground (visage::Canvas& canvas, const Theme& theme,
                          float x, float y, float width, float height)
    {
        // Vertical warm-white gradient, top (background) -> foot (backgroundLo).
        canvas.setColor (visage::Brush::vertical (visage::Color (theme.palette.background),
                                                  visage::Color (theme.palette.backgroundLo)));
        canvas.fill (x, y, width, height);
    }

    void paintCard (visage::Canvas& canvas, const Theme& theme,
                    float x, float y, float width, float height)
    {
        const CardMetrics& c = theme.card;

        // 1) Soft drop shadow, nudged down by the card offset (JUCE DropShadow{0,5}).
        canvas.setColor (visage::Color (theme.palette.shadow));
        canvas.roundedRectangleShadow (x + c.shadowOffsetX, y + c.shadowOffsetY,
                                       width, height, c.cornerRadius, c.shadowBlur);

        // 2) White panel fill.
        canvas.setColor (visage::Color (theme.palette.panel));
        canvas.roundedRectangle (x, y, width, height, c.cornerRadius);

        // 3) Soft track outline, inset half a pixel like factory_ui (reduced(0.5)).
        canvas.setColor (visage::Color (theme.palette.track));
        canvas.roundedRectangleBorder (x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f,
                                       c.cornerRadius, c.outlineWidth);
    }
}
