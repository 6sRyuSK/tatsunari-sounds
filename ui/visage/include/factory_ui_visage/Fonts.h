#pragma once

#include <visage_graphics/font.h>

//
// factory_ui_visage::Fonts — the design-system typeface.
//
// DEFAULT typeface: **Quicksand** (Regular + Bold), SIL Open Font License 1.1.
// Quicksand is a geometric-rounded sans that matches the factory's warm, rounded
// "kawaii" look; the two weights are static instances (wght=400 / wght=700) of
// the Google Fonts variable font, embedded at build time from ui/visage/fonts/.
//
// CONFIRMED DEFAULT (human sign-off 2026-07-17): Quicksand is the final, approved
// design-system typeface — no longer provisional. The Phase P2b taste comparison
// is settled in its favour.
//
// FONT-FAMILY SWITCH (retained for experiments). The library still embeds THREE
// families and can switch between them at RUNTIME via a single global selector,
// so a future taste experiment (or a per-plugin trial) needs no rebuild — but
// Quicksand is the shipped default and nothing should change it without a new
// human decision:
//   * Quicksand            — the CONFIRMED default (2026-07-17)
//   * Nunito               — OFL, static 400/700 (experiment only)
//   * M PLUS Rounded 1c    — OFL, Latin subset 400/700 (experiment only; the full CJK face is huge)
// See ui/visage/fonts/*-OFL.txt for the licences. `regularFont`/`boldFont` return
// the ACTIVE family's face, so a `setFontFamily(...)` call followed by a redraw
// re-types the whole UI with no rebuild. Nothing in the widget code names a
// concrete face — it all goes through here.
//
// Adding/removing a candidate is a one-place change: drop the .ttf into
// ui/visage/fonts/ (the CMake glob embeds it) and extend the switch in Fonts.cpp.
//
namespace factory_ui_visage
{
    enum class FontFamily
    {
        Quicksand, // default
        Nunito,
        MPlus
    };

    // The active family (default Quicksand). UI-thread only.
    void       setFontFamily (FontFamily family);
    FontFamily fontFamily();

    // Set by lowercase name ("quicksand" | "nunito" | "mplus"). Returns false for
    // an unknown name (the active family is left unchanged) so callers/bridges can
    // report a bad value instead of silently doing nothing.
    bool setFontFamilyByName (const char* name);

    // The active family's short name ("quicksand" | "nunito" | "mplus").
    const char* fontFamilyName();

    // A visage::Font at the given pixel size, in the ACTIVE family. `sizePx` is a
    // logical size; the canvas applies the DPI scale when drawn via canvas.text().
    visage::Font regularFont (float sizePx);
    visage::Font boldFont (float sizePx);
}
