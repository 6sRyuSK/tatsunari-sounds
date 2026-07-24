#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ValueEntry.h"
#include "factory_params/ParamStore.h"
#include "factory_params/Range.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <visage_ui/frame.h>
#include <visage_graphics/canvas.h>

//
// factory_ui_visage::Knob — a rotary knob that reproduces the factory look
// (value arc + soft glow, top-lit body gradient, track outline, drop shadow,
// pointer dot) exactly like FactoryLookAndFeel::drawRotarySlider, drawn on a
// visage::Frame instead of a JUCE Graphics.
//
// It binds to a factory_params::ParamStore by parameter index: it READS the live
// value (store.value) every time it draws, and WRITES through the UI gesture path
// (beginGesture / setFromUi / endGesture) exactly as a host-facing editor would.
//
// Editing: vertical + horizontal drag (RotaryHorizontalVerticalDrag-like), shift
// for fine, double-click to reset to default, mouse-wheel to step. Below the dial
// it shows the formatted value and the parameter name (JUCE text-below convention).
//
namespace factory_ui_visage
{
    // Value-ring / needle angle (radians) for a normalised position, in the dial
    // convention 0 = 12 o'clock growing clockwise. The sweep endpoints come from
    // the theme's KnobMetrics (RS: 225° start, 270° clockwise), so this is the
    // SINGLE source of truth every knob (the shared Knob AND the NodePanel
    // mini-knobs) maps value -> angle through — see RsNodePanel::drawMiniKnob.
    inline float knobAngleForNorm (const KnobMetrics& m, float norm)
    {
        norm = std::clamp (norm, 0.0f, 1.0f);
        return m.arcStart + norm * (m.arcEnd - m.arcStart);
    }

    // Needle tip for a dial centred at (cx, cy) with the given bar length and
    // angle (same convention as knobAngleForNorm). Shared so the needle geometry
    // can never drift between the shared Knob and the mini-knobs.
    inline visage::Point knobNeedleTip (float cx, float cy, float length, float angle)
    {
        return { cx + length * std::sin (angle), cy - length * std::cos (angle) };
    }

    // Fill a flat donut band spanning [a0, a1] (a1 >= a0, radians; dial angle
    // convention: 0 = 12 o'clock, growing clockwise — the SAME convention
    // knobAngleForNorm / knobNeedleTip use) at centreline radius r with the given
    // thickness, using visage's native flatArc GPU primitive (a solid,
    // anti-aliased band — no path-fill atlas, which the RS frame's large analyser
    // paths otherwise poison, silently dropping every path fill).
    //
    // SHARED so the footer Knob AND the NodePanel mini-knobs draw their value ring
    // through ONE arc-drawing path — the mini-knobs previously drew the arc with a
    // single canvas.arc that skipped the −90° screen-offset this applies, so the
    // ring landed 90° off from the needle (round-3 fix 6). flatArc takes a mid
    // angle + half-aperture; its mirrored-arc SDF (and visage's own half-aperture
    // clamp to π) do NOT render a single wide band faithfully — a zone approaching
    // ~180° over-covers its neighbours and narrow zones drop out — so tile the
    // span with small sub-arcs (each well inside the SDF's clean regime); pieces of
    // one zone share the caller's brush and a tiny angular overlap closes AA seams.
    inline void fillArcBand (visage::Canvas& canvas, float cx, float cy, float r,
                             float a0, float a1, float thickness)
    {
        constexpr float kPi    = 3.14159265358979323846f;
        constexpr float kTwoPi = 6.28318530717958648f;
        constexpr float kMaxSpan = 0.30f; // ~17deg per piece — flatArc is faithful here
        constexpr float kOverlap = 0.02f; // ~1.1deg same-brush overlap (hides seams)
        const float total = a1 - a0;
        if (total <= 1.0e-4f)
            return;
        const int   pieces = std::max (1, (int) std::ceil (total / kMaxSpan));
        const float span   = total / (float) pieces;
        for (int i = 0; i < pieces; ++i)
        {
            const float s = a0 + (float) i * span;
            const float e = std::min (a1, a0 + (float) (i + 1) * span + (i < pieces - 1 ? kOverlap : 0.0f));
            // flatArc renders a piece at screen angle (passed + 90deg) in our dial
            // convention, so pass (intended mid − 90deg) to land it where we want.
            // Normalise to [-pi, pi] (sin/cos are periodic) since the raw sweep runs
            // past 2pi for the dead zone.
            float mid = 0.5f * (s + e) - 0.5f * kPi;
            while (mid >  kPi) mid -= kTwoPi;
            while (mid < -kPi) mid += kTwoPi;
            canvas.arc (cx - r, cy - r, 2.0f * r, thickness, mid, 0.5f * (e - s), /*rounded*/ false);
        }
    }

    class Knob : public visage::Frame
    {
    public:
        // `theme` must outlive the knob: the gallery owns one Theme and mutates it
        // in place for hot reload, so the reference always sees the current values.
        Knob (factory_params::ParamStore& store, int paramIndex, const Theme& theme, int decimals = 1);

        // Per-instance arc/pointer accent (0xAARRGGBB). 0 (the default) keeps the
        // theme accent, so existing call sites are unaffected; a plugin editor sets
        // this to give a knob its own accent (e.g. the RS amber ATK/REL, mint TILT)
        // without forking the widget.
        void setAccentColour (std::uint32_t argb) { accentOverride_ = argb; redraw(); }

        // Override the label drawn under the dial (default: the parameter's display
        // name). Lets a plugin editor use its own short/upper-case caption (e.g. the
        // RS "ATK"/"REL"/"TILT") without forking the widget. Empty = use desc.name.
        void setNameOverride (std::string name) { nameOverride_ = std::move (name); redraw(); }

        // Match a specific editor's knob proportions: the top (name) and bottom
        // (value) text-row heights in px, the name/value font sizes in px, and the
        // dial's bounds inset in px. Any argument < 0 keeps the widget default (16 px
        // rows, theme.font.label text, theme.knob.boundsInset), so the gallery is
        // unaffected. The RS editor sets a "big" (Depth/Detail) vs "small"
        // (Atk/Rel/Tilt) profile to reproduce the shipped JUCE RsKnob's exact dial
        // diameters + per-size fonts (round-3 fixes 2 + 7).
        void setDialProfile (float textTopPx, float textBottomPx, float nameFontPx,
                             float valueFontPx, float dialInsetPx)
        {
            textTopPx_ = textTopPx; textBottomPx_ = textBottomPx;
            nameFontPx_ = nameFontPx; valueFontPx_ = valueFontPx; dialInsetPx_ = dialInsetPx;
            redraw();
        }

        // Direct text entry: double-clicking the value read-out opens the shared
        // ValueEntry overlay (the editor hosts it). Unset (the gallery default) => no
        // text entry, double-click on the value row does nothing extra.
        ValueEntryOpener requestValueEntry;

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;
        bool mouseWheel (const visage::MouseEvent& e) override;

        int paramIndex() const { return index_; }

        // Re-point this knob at a DIFFERENT parameter index. A per-band control panel
        // (e.g. dynamic-eq's DeqBandPanel) rebinds its knobs to the newly-selected band
        // rather than owning one control set per band. Recomputes the range from the new
        // desc and DROPS any in-flight drag, so a rebind mid-drag can never write the
        // band you just left. redraw()s to pick up the new value immediately.
        void rebind (int paramIndex);

    private:
        float currentNorm() const;    // live store value -> normalised 0..1
        void  writeNorm (float norm); // normalised -> real, store via setFromUi, redraw
        float valueRowHeight() const; // bottom value read-out row height (px)
        void  openValueEntry();       // double-click the value row -> shared overlay
        void  commitValueEntry (const std::string& text); // parse + clamp + gesture-write

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
        int decimals_;
        factory_params::RangeSpec range_;

        std::uint32_t accentOverride_ = 0; // 0 == use theme accent
        std::string   nameOverride_;       // empty == use desc.name

        // Dial profile (round-3 fixes 2 + 7); < 0 == widget default. See setDialProfile.
        float textTopPx_ = -1.0f, textBottomPx_ = -1.0f;
        float nameFontPx_ = -1.0f, valueFontPx_ = -1.0f, dialInsetPx_ = -1.0f;

        bool dragging_ = false;
        float dragNorm_ = 0.0f;         // authoritative accumulator during a drag
        visage::Point lastDragPos_;
    };
}
