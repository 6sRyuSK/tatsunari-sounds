#pragma once

#include <visage_graphics/font.h>

//
// factory_ui_visage::Fonts — the design-system typeface.
//
// PROVISIONAL default typeface: **Quicksand** (Regular + Bold), SIL Open Font
// License 1.1. Quicksand is a geometric-rounded sans that matches the factory's
// warm, rounded "kawaii" look; the two weights are static instances (wght=400 /
// wght=700) of the Google Fonts variable font, embedded at build time from
// ui/visage/fonts/ (see ui/visage/fonts/OFL.txt for the licence).
//
// This is a PLACEHOLDER: the final typeface is an explicit human taste decision
// (Phase P2b+). Swapping it is a ONE-PLACE change:
//   1. drop the new .ttf files into ui/visage/fonts/
//   2. point FACTORY_UI_VISAGE_FONTS in ui/visage/CMakeLists.txt at them
//   3. update the two symbol names in src/Fonts.cpp
// Nothing else in the widget code references a concrete font.
//
namespace factory_ui_visage
{
    // A visage::Font at the given pixel size. `sizePx` is a logical size; the
    // canvas applies the DPI scale when the font is drawn via canvas.text().
    visage::Font regularFont (float sizePx);
    visage::Font boldFont (float sizePx);
}
