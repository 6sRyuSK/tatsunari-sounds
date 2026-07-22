#pragma once

#include <visage_graphics/canvas.h>
#include <visage_graphics/path.h>

#include <algorithm>

//
// factory_ui_visage::icons — the small glyph set for the P2b chrome, transcribed
// verbatim from rs::icons (plugins/resonance-suppressor/Source/RsWidgets.h): the
// same viewBox coordinates and stroke weights, rebuilt on a visage::Path instead
// of a juce::Path. SVG "C x1 y1,x2 y2,x y" maps to path.bezierTo(x1,y1,x2,y2,x,y);
// "M/L" map to moveTo/lineTo. Each glyph is expressed in its native viewBox and
// placed into a target rect with paintGlyph() (min-dimension fit, centred).
//
namespace factory_ui_visage::icons
{
    struct Glyph
    {
        visage::Path path;
        float box    = 16.0f;
        bool  filled = false;
        float stroke = 1.6f;
    };

    // ---- viewBox 16 (rs demo SS3.6) --------------------------------------------
    inline Glyph quality() // rising bars (QUALITY)
    {
        Glyph g; g.stroke = 2.0f;
        g.path.moveTo (3.5f, 13.0f); g.path.lineTo (3.5f, 10.5f);
        g.path.moveTo (8.0f, 13.0f); g.path.lineTo (8.0f, 7.0f);
        g.path.moveTo (12.5f, 13.0f); g.path.lineTo (12.5f, 4.0f);
        return g;
    }

    inline Glyph channel() // two overlapping circles (CH, M/S)
    {
        Glyph g; g.stroke = 1.6f;
        g.path.addCircle (5.6f, 8.0f, 3.2f);
        g.path.addCircle (10.4f, 8.0f, 3.2f);
        return g;
    }

    inline Glyph link() // two chain links (STEREO LINK)
    {
        Glyph g; g.stroke = 1.6f;
        g.path.addRoundedRectangle (2.0f, 5.5f, 7.5f, 5.0f, 2.5f);
        g.path.addRoundedRectangle (6.5f, 5.5f, 7.5f, 5.0f, 2.5f);
        return g;
    }

    inline Glyph listen() // concentric "monitor" dot (SC LISTEN)
    {
        Glyph g; g.stroke = 1.6f;
        g.path.addCircle (8.0f, 8.0f, 4.5f);
        g.path.addCircle (8.0f, 8.0f, 1.6f);
        return g;
    }

    inline Glyph modeSoft() // smooth bell
    {
        Glyph g; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 11.0f);
        g.path.bezierTo (6.0f, 11.0f, 6.0f, 5.0f, 8.0f, 5.0f);
        g.path.bezierTo (10.0f, 5.0f, 10.0f, 11.0f, 14.0f, 11.0f);
        return g;
    }

    inline Glyph modeHard() // stepped
    {
        Glyph g; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 11.0f); g.path.lineTo (6.0f, 11.0f); g.path.lineTo (6.0f, 5.0f);
        g.path.lineTo (10.0f, 5.0f); g.path.lineTo (10.0f, 11.0f); g.path.lineTo (14.0f, 11.0f);
        return g;
    }

    // ---- viewBox 24 (header additions) -----------------------------------------
    inline Glyph undo() // hooked arrow pointing left
    {
        Glyph g; g.box = 24.0f; g.stroke = 2.0f;
        g.path.moveTo (15.0f, 13.0f); g.path.lineTo (15.0f, 9.0f); g.path.lineTo (6.0f, 9.0f);
        g.path.moveTo (9.5f, 6.0f); g.path.lineTo (6.0f, 9.0f); g.path.lineTo (9.5f, 12.0f);
        return g;
    }

    inline Glyph redo() // mirror of undo
    {
        Glyph g; g.box = 24.0f; g.stroke = 2.0f;
        g.path.moveTo (9.0f, 13.0f); g.path.lineTo (9.0f, 9.0f); g.path.lineTo (18.0f, 9.0f);
        g.path.moveTo (14.5f, 6.0f); g.path.lineTo (18.0f, 9.0f); g.path.lineTo (14.5f, 12.0f);
        return g;
    }

    inline Glyph copy() // two overlapping rounded rectangles
    {
        Glyph g; g.box = 24.0f; g.stroke = 1.8f;
        g.path.addRoundedRectangle (4.0f, 8.0f, 11.0f, 12.0f, 2.0f);
        g.path.addRoundedRectangle (9.0f, 4.0f, 11.0f, 12.0f, 2.0f);
        return g;
    }

    inline Glyph caret() // small down chevron (menu affordance)
    {
        Glyph g; g.box = 24.0f; g.stroke = 2.0f;
        g.path.moveTo (8.0f, 10.0f); g.path.lineTo (12.0f, 14.0f); g.path.lineTo (16.0f, 10.0f);
        return g;
    }

    // Uniformly scale a glyph's viewBox into `area` (min-dimension fit, centred)
    // and paint it (stroke or fill) with the CURRENTLY-SET canvas brush.
    inline void paintGlyph (visage::Canvas& canvas, const Glyph& glyph,
                            float x, float y, float w, float h)
    {
        const float s = std::min (w, h) / glyph.box;
        visage::Path p = glyph.path.scaled (s).translated (x + w * 0.5f - glyph.box * s * 0.5f,
                                                            y + h * 0.5f - glyph.box * s * 0.5f);
        if (glyph.filled)
            canvas.fill (p);
        else
            canvas.fill (p.stroke (glyph.stroke * s, visage::Path::Join::Round, visage::Path::EndCap::Round));
    }
}
