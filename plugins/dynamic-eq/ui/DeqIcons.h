#pragma once

#include "factory_ui_visage/Icons.h"

#include <vector>

//
// deq_ui::icons — the five EQ band-type glyphs the dynamic-eq band panel shows in
// place of the "Bell"/"Low Shelf"/… text (so the type reads at a glance like the
// filter shape). Built on a visage::Path in the same viewBox/stroke idiom as the
// shared factory_ui_visage::icons set (transcribed from rs::icons — see
// RsWidgets.h / RsIcons.h), so they sit consistently next to the other glyphs.
// Order matches factory_core::BandType (Bell, LowShelf, HighShelf, HighPass,
// LowPass) so a choice index maps straight to a glyph.
//
namespace deq_ui::icons
{
    using factory_ui_visage::icons::Glyph;

    inline Glyph bell() // symmetric bump
    {
        Glyph g; g.box = 16.0f; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 11.0f);
        g.path.bezierTo (6.0f, 11.0f, 6.0f, 5.0f, 8.0f, 5.0f);
        g.path.bezierTo (10.0f, 5.0f, 10.0f, 11.0f, 14.0f, 11.0f);
        return g;
    }

    inline Glyph lowShelf() // high plateau (left) stepping down to low (right)
    {
        Glyph g; g.box = 16.0f; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 5.0f);
        g.path.lineTo (6.0f, 5.0f);
        g.path.bezierTo (8.0f, 5.0f, 8.0f, 11.0f, 10.0f, 11.0f);
        g.path.lineTo (14.0f, 11.0f);
        return g;
    }

    inline Glyph highShelf() // low plateau (left) stepping up to high (right)
    {
        Glyph g; g.box = 16.0f; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 11.0f);
        g.path.lineTo (6.0f, 11.0f);
        g.path.bezierTo (8.0f, 11.0f, 8.0f, 5.0f, 10.0f, 5.0f);
        g.path.lineTo (14.0f, 5.0f);
        return g;
    }

    inline Glyph highPass() // cut: the low side falls all the way to the floor, then a high plateau
    {
        Glyph g; g.box = 16.0f; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 14.0f);
        g.path.lineTo (4.0f, 14.0f);
        g.path.bezierTo (6.0f, 14.0f, 7.0f, 4.0f, 9.0f, 4.0f);
        g.path.lineTo (14.0f, 4.0f);
        return g;
    }

    inline Glyph lowPass() // cut: a high plateau, then the high side falls all the way to the floor
    {
        Glyph g; g.box = 16.0f; g.stroke = 2.0f;
        g.path.moveTo (2.0f, 4.0f);
        g.path.lineTo (7.0f, 4.0f);
        g.path.bezierTo (9.0f, 4.0f, 10.0f, 14.0f, 12.0f, 14.0f);
        g.path.lineTo (14.0f, 14.0f);
        return g;
    }

    // In factory_core::BandType order (Bell, LowShelf, HighShelf, HighPass, LowPass).
    inline std::vector<Glyph> bandTypeIcons()
    {
        return { bell(), lowShelf(), highShelf(), highPass(), lowPass() };
    }
}
