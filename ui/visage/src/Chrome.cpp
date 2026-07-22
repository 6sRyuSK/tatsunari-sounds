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

        // 2+3) White panel fill + soft track outline (the shared shell idiom).
        paintCardShell (canvas, x, y, width, height, c.cornerRadius,
                        visage::Color (theme.palette.panel), visage::Color (theme.palette.track),
                        c.outlineWidth);
    }

    void paintCardShell (visage::Canvas& canvas, float x, float y, float width, float height,
                         float cornerRadius, visage::Color fill, visage::Color border,
                         float borderPx)
    {
        canvas.setColor (fill);
        canvas.roundedRectangle (x, y, width, height, cornerRadius);
        paintHairline (canvas, x, y, width, height, cornerRadius, border, borderPx);
    }

    void paintHairline (visage::Canvas& canvas, float x, float y, float width, float height,
                        float cornerRadius, visage::Color colour, float borderPx)
    {
        // Inset half a pixel like factory_ui (reduced(0.5)).
        canvas.setColor (colour);
        canvas.roundedRectangleBorder (x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f,
                                       cornerRadius, borderPx);
    }
}
