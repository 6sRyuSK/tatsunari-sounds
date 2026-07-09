#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//
// rs:: — plugin-local theme for the Resonance TatSuppressor "new UI" chrome
// (Phase P1). Palette / radii / spacing are transcribed verbatim from
// rs-ui-work/demo-analysis.md (SS1.3 palette table, SS1.5 radii/spacing table) —
// that document is the single source of truth for every value here; don't
// hand-tune a colour without checking it first.
//
// Font access is centralised in rs::font() (see below) so swapping the
// approximated system font for embedded Baloo 2 / Nunito later is a
// one-place change, per spec-P1.md.
//
namespace rs
{
    // ------------------------------------------------------------- palette
    // Hex values match demo-analysis.md SS1.3 exactly. Where a role coincides
    // with an existing FactoryLookAndFeel colour (panel/track/accent/text all
    // happen to already match this palette) we still define our own getter
    // here so this file stays the single reference for the new chrome, and
    // so a future palette tweak doesn't have to touch the shared factory_ui
    // header.
    namespace colour
    {
        // Warm neutrals / backgrounds.
        inline juce::Colour panelTop()     { return juce::Colour (0xfffdf6f2); }
        inline juce::Colour panelBottom()  { return juce::Colour (0xfffbeae2); }
        inline juce::Colour plotTop()      { return juce::Colour (0xfffffaf7); }
        inline juce::Colour plotBottom()   { return juce::Colour (0xfffff4ee); }
        inline juce::Colour chipBg()       { return juce::Colour (0xfffffdfb); }
        inline juce::Colour white()        { return juce::Colour (0xffffffff); }
        inline juce::Colour footerBg()     { return juce::Colour (0xfffff4ee); } // == knob "gap" sector colour
        inline juce::Colour segTrackBg()   { return juce::Colour (0xfffff1ec); } // Soft/Hard segmented track
        inline juce::Colour modeBoxTop()   { return juce::Colour (0xfffff6f3); }
        inline juce::Colour border()       { return juce::Colour (0xfff2ddd4); } // standard hairline / divider
        inline juce::Colour borderLight()  { return juce::Colour (0xfff7e7e0); }
        inline juce::Colour linkTrack()    { return juce::Colour (0xfff4e3db); }
        inline juce::Colour toggleOffBg()  { return juce::Colour (0xffe7d3ca); }

        // Coral / warm accents.
        inline juce::Colour accent()        { return juce::Colour (0xffff7a6b); } // PRIMARY coral
        inline juce::Colour orange()        { return juce::Colour (0xffff9472); } // logo grad start / selected band
        inline juce::Colour pink()          { return juce::Colour (0xffff6f91); } // logo grad end
        inline juce::Colour amber()         { return juce::Colour (0xffffba6b); } // ATK/REL accent
        inline juce::Colour knobTrack()     { return juce::Colour (0xffffd6cd); } // unfilled knob arc
        inline juce::Colour linkFillStart() { return juce::Colour (0xffffb0a3); }
        inline juce::Colour modeBoxBorder() { return juce::Colour (0xffffcabf); }

        // Teal / green (the design system's generic toggle-ON colour).
        inline juce::Colour teal()   { return juce::Colour (0xff45b8ac); }
        inline juce::Colour mint()   { return juce::Colour (0xff7fd1ae); } // TILT accent

        // Text / muted.
        inline juce::Colour text()          { return juce::Colour (0xff6b5750); } // primary dark (taupe-brown)
        inline juce::Colour textSecondary() { return juce::Colour (0xff8f7a72); } // settings captions
        inline juce::Colour iconInactive()  { return juce::Colour (0xff9c857c); }
        inline juce::Colour textMuted()     { return juce::Colour (0xffb9a39b); } // inactive tab/segment
        inline juce::Colour textFaint()     { return juce::Colour (0xffc9b3aa); } // preset arrows

        // Shadows (rgba, straight from demo-analysis SS1.3/SS1.5).
        inline juce::Colour shadowPanel() { return juce::Colour::fromFloatRGBA (191.0f / 255.0f, 140.0f / 255.0f, 120.0f / 255.0f, 0.30f); }
        inline juce::Colour shadowTaupe() { return juce::Colour::fromFloatRGBA (107.0f / 255.0f,  87.0f / 255.0f,  80.0f / 255.0f, 0.14f); }
        inline juce::Colour glowPink()    { return juce::Colour::fromFloatRGBA (255.0f / 255.0f, 111.0f / 255.0f, 145.0f / 255.0f, 0.40f); }
    }

    // --------------------------------------------------------------- fonts
    enum class FontKind { Display, Ui };

    // Centralised font access (spec-P1: "centralise into ONE helper"). Baloo 2
    // (Display) / Nunito (Ui) are NOT embedded in P1 -- both kinds currently
    // approximate to the platform default sans at a bold weight, matching the
    // demo's heavy (700/800) kawaii type. `weight` is a CSS-like number
    // (400/600/700/800...); JUCE's FontStyleFlags only distinguishes
    // plain/bold, so anything >= 600 maps to bold. `trackingEm` approximates
    // the demo's letter-spacing (a multiple of the font height, same unit as
    // CSS `em`) via Font::withExtraKerningFactor -- used sparingly, only
    // where demo-analysis calls out tracking on all-caps labels.
    //
    // Swapping in real Baloo 2 / Nunito typefaces later is a one-place change
    // here (construct FontOptions with a typeface name/Typeface::Ptr instead
    // of the default sans).
    inline juce::Font font (FontKind kind, float px, int weight = 700, float trackingEm = 0.0f)
    {
        juce::ignoreUnused (kind); // both kinds approximate the same family for now -- see comment above
        const int styleFlags = weight >= 600 ? juce::Font::bold : juce::Font::plain;
        juce::Font f (juce::FontOptions (px, styleFlags));
        if (trackingEm != 0.0f)
            f = f.withExtraKerningFactor (trackingEm);
        return f;
    }

    // -------------------------------------------------------------- radii
    // Corner-radius ladder, demo-analysis SS1.5.
    namespace radius
    {
        constexpr float outer     = 22.0f; // outer panel (NOT applied to the top-level editor -- see PluginEditor.cpp paint())
        constexpr float card      = 16.0f; // analyzer / footer card
        constexpr float popover   = 18.0f; // node popover (P2)
        constexpr float box       = 12.0f; // knob-name cards / MODE box
        constexpr float badge     = 9.0f;  // small cards & badges
        constexpr float badgeLg   = 11.0f; // preset pill
        constexpr float pill      = 999.0f; // toggles / segmented active pill (fully rounded)
        constexpr float cutHandle = 4.0f;  // node cut handles (P2)
    }

    // ------------------------------------------------------------ spacing
    // demo-analysis SS1.5. Used as base units for the k-scaled layout in
    // PluginEditor::resized() (design canvas is layout::designWidth wide).
    namespace spacing
    {
        constexpr int panelPad     = 20;
        constexpr int panelGap     = 16;
        constexpr int footerGap    = 18;
        constexpr int bigKnobGap   = 22;
        constexpr int smallKnobGap = 18;
        constexpr int labelGap     = 5;
        constexpr int settingsGap  = 7;
    }

    // ------------------------------------------------------------- layout
    // The demo's authored canvas size (demo-analysis SS1.1). PluginEditor
    // derives a uniform scale factor from this in resized() so the whole
    // chrome scales as a unit across the resizable range instead of any one
    // column starving at the resize floor.
    namespace layout
    {
        constexpr int designWidth  = 1069;
        constexpr int designHeight = 747;
    }
}
