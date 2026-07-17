#pragma once

#include "factory_ui_visage/Theme.h"

#include <cstdint>
#include <string>

//
// rs_ui::RsTheme — the resonance-suppressor plugin theme: the SHARED
// factory_ui_visage::Theme (with a small overlay for the RS look) plus the
// RS-specific extras the shared schema does not carry. Loaded by merging the
// shared theme with plugins/resonance-suppressor/ui/theme-rs.json:
//
//   * the theme-rs.json TOP-LEVEL shared-schema keys (knob / card / …) overlay
//     the shared Theme via Theme::applyOverlay — this is what makes the shared
//     Knob draw the RsLookAndFeel "chunky arc" (a thick value ring at the RS
//     225°/270° sweep) without forking the widget, and is live hot-reloadable;
//   * the theme-rs.json "rs" object carries the RS extras (accent palette,
//     radii ladder, spacing, the analyser style constants that used to live in
//     the — now un-ported — AnalyzerDevPanel, and node-handle geometry). Those
//     are transcribed here as the compiled reference (RsExtras::defaults(), == the
//     "rs" object in theme-rs.json) so the human reviewing the JSON can veto them.
//
// Colours are 0xAARRGGBB, matching factory_ui_visage::Palette. Every value below
// is transcribed from RsTheme.h (rs::colour / rs::radius / rs::spacing),
// AnalyzerStyle.h (kV201Style) and the local hex constants in
// SuppressionCurveComponent.h / NodePanel.h. JUCE-free, visage-free.
//
namespace rs_ui
{
    struct RsExtras
    {
        // ---- RS palette extras (roles the shared Palette lacks) --------------
        std::uint32_t plotTop       = 0xfffffaf7; // analyser plot gradient top
        std::uint32_t plotBottom    = 0xfffff4ee; // analyser plot gradient foot
        std::uint32_t chipBg        = 0xfffffdfb; // analyser header chips
        std::uint32_t footerBg      = 0xfffff4ee; // footer card fill
        std::uint32_t segTrackBg    = 0xfffff1ec; // segmented track
        std::uint32_t modeBoxTop    = 0xfffff6f3; // MODE cell gradient top
        std::uint32_t modeBoxBorder = 0xffffcabf; // MODE cell border (coral-tinted)
        std::uint32_t borderLight   = 0xfff7e7e0; // faint grid / divider
        std::uint32_t linkTrack     = 0xfff4e3db; // link-slider track
        std::uint32_t linkFillStart = 0xffffb0a3; // link-slider fill gradient start
        std::uint32_t toggleOffBg   = 0xffe7d3ca; // pill toggle OFF track

        // Coral / warm accents (per-knob + node identity).
        std::uint32_t orange   = 0xffff9472; // logo grad start / LC ring / popover accent
        std::uint32_t pink     = 0xffff6f91; // logo grad end
        std::uint32_t amber    = 0xffffba6b; // ATK / REL knob accent
        std::uint32_t mint     = 0xff7fd1ae; // TILT knob accent
        // teal (toggle-ON / Listen / reduction curtain) is the shared
        // palette.positive (#45b8ac) — see Theme::Palette::positive.
        std::uint32_t highCutRing = 0xff79b8ef; // HC handle ring

        // Text / muted extras.
        std::uint32_t iconInactive = 0xff9c857c;
        std::uint32_t textFaint    = 0xffc9b3aa; // preset arrows / cut guides

        // Soft shadows / glow (0xAARRGGBB straight from demo-analysis SS1.3).
        std::uint32_t glowPink    = 0x66ff6f91; // logo glow (a=0.40)
        std::uint32_t shadowTaupe = 0x246b5750; // knob face shadow (a=0.14)
        std::uint32_t nodeShadow  = 0x476b5750; // node drop shadow (a=0.28)
        std::uint32_t dotRing     = 0xffffe4db; // node-panel header dot ring
        std::uint32_t typeCaption = 0xffc39a8c; // "TYPE"/"SLOPE" caption

        // GR (gain-reduction) badge.
        std::uint32_t grBg     = 0xffeafaf7;
        std::uint32_t grBorder = 0xffb8e8e2;
        std::uint32_t grText   = 0xff2f9488;

        // ---- radii ladder (rs::radius) ---------------------------------------
        float radiusOuter     = 22.0f;
        float radiusCard      = 16.0f;
        float radiusPopover   = 18.0f;
        float radiusBox       = 12.0f;
        float radiusBadge     = 9.0f;
        float radiusBadgeLg   = 11.0f;
        float radiusCutHandle = 4.0f;

        // ---- spacing (rs::spacing) -------------------------------------------
        float panelPad     = 20.0f;
        float panelGap     = 16.0f;
        float bigKnobGap   = 22.0f;
        float smallKnobGap = 18.0f;

        // ---- node-handle geometry (SuppressionCurveComponent::drawNodes) ------
        float bandDot       = 15.0f; // band node diameter (20 selected)
        float bandDotSel    = 20.0f;
        float cutHandle     = 13.0f; // cut square (18 selected)
        float cutHandleSel  = 18.0f;
        float nodeHitRadius = 14.0f; // nodeAt() grab radius

        // ---- analyser style (design reference 2026-07-17 — supersedes the ported
        // kV201Style). The reduction "curtain" hangs from the plot TOP in teal
        // (palette.positive); the PRE and POST spectra are both thin SOLID coral
        // lines (POST returns to the kV201Style solid-line role); the combined
        // reduction PROFILE is a dashed muted-coral line running through the node
        // dots; and each active node adds a subtle pale bump fill near the baseline.
        // A human still signs off these values via theme-rs.json.
        float curtainFillAlpha    = 0.34f;      // teal reduction curtain (from the top)
        float curtainClampFrac    = 0.5f;       // max curtain depth (fraction of plot height)
        std::uint32_t preColour   = 0xffff7a6b; // PRE (input) — solid coral line
        float preLineWidth        = 2.0f;
        std::uint32_t postColour  = 0xffff7a6b; // POST (output) — thin SOLID coral (kV201Style)
        float postLineWidth       = 1.4f;       // kV201Style postLineWidth
        float postLineAlpha       = 0.85f;      // kV201Style postLineAlpha
        std::uint32_t profileColour = 0xffe08a7f; // combined profile — dashed muted coral
        float profileLineWidth    = 1.5f;
        float profileDashOn       = 5.0f;       // combined-profile dash pattern (on / off px)
        float profileDashOff      = 4.0f;
        float perNodeFillAlpha    = 0.12f;      // subtle per-node bump fill (near the baseline)
        float displaySmoothMs     = 50.0f;      // -> RsFeed::setDisplaySmoothMs

        static RsExtras defaults() { return RsExtras {}; }
    };

    struct RsTheme
    {
        factory_ui_visage::Theme base; // shared theme (+ overlay)
        RsExtras                 rs;    // RS-specific extras

        static RsTheme defaults()
        {
            RsTheme t;
            t.base = factory_ui_visage::Theme::defaults();
            // The shared "donut + needle" knob (design reference 2026-07-17) is now
            // the factory-wide default, so the RS editor only overrides the card
            // radius here; theme-rs.json's top-level `card` mirrors it.
            t.base.card.cornerRadius = 16.0f;
            t.rs = RsExtras::defaults();
            return t;
        }

        // Merge the shared default theme with an overlay document (theme-rs.json
        // text). The overlay's shared-schema keys (knob/card/…) are applied to the
        // base via Theme::applyOverlay (which ignores the "rs" extras object); the
        // extras themselves are the compiled RsExtras::defaults() (== the JSON's
        // "rs" object — the human-review surface). Returns false + fills `error`
        // only if the shared-schema part is malformed (so a typo in the chunky-knob
        // overlay is reported, exactly like a bad shared theme).
        static bool load (const std::string& overlayJson, RsTheme& out, std::string& error)
        {
            RsTheme t = RsTheme::defaults();
            if (! overlayJson.empty())
                if (! t.base.applyOverlay (overlayJson, error))
                    return false;
            out = t;
            return true;
        }
    };
}
