#pragma once

#include "factory_ui_visage/Icons.h"

//
// rs_ui::icons — the two resonance-suppressor glyphs that the shared
// factory_ui_visage::icons set does not carry (DELTA, S-CHAIN), rebuilt on a
// visage::Path in the same viewBox/stroke as rs::icons (RsWidgets.h). Everything
// else the editor needs (quality/channel/link/listen/modeSoft/modeHard/undo/redo/
// copy/caret) already lives in the shared set and is reused verbatim — these two
// are kept plugin-local so the shared header stays untouched.
//
namespace rs_ui::icons
{
    using factory_ui_visage::icons::Glyph;

    inline Glyph delta() // triangle (DELTA)
    {
        Glyph g; g.box = 16.0f; g.stroke = 1.6f;
        g.path.moveTo (8.0f, 3.0f);
        g.path.lineTo (13.5f, 13.0f);
        g.path.lineTo (2.5f, 13.0f);
        g.path.close();
        return g;
    }

    inline Glyph sidechain() // down arrow into a baseline (S-CHAIN)
    {
        Glyph g; g.box = 16.0f; g.stroke = 1.8f;
        g.path.moveTo (8.0f, 2.5f);  g.path.lineTo (8.0f, 9.0f);
        g.path.moveTo (5.0f, 6.0f);  g.path.lineTo (8.0f, 9.4f); g.path.lineTo (11.0f, 6.0f);
        g.path.moveTo (2.5f, 13.0f); g.path.lineTo (13.5f, 13.0f);
        return g;
    }
}
