#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_params/Text.h"

#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>

namespace factory_ui_visage
{
    // fillArcBand (the tiled flatArc donut band) now lives in Knob.h so the
    // NodePanel mini-knobs share the SAME value-ring arc convention as this Knob
    // (round-3 fix 6). constexpr kTwoPi kept for the dead-zone sweep below.
    namespace { constexpr float kTwoPi = 6.28318530717958648f; }

    Knob::Knob (factory_params::ParamStore& store, int paramIndex, const Theme& theme, int decimals)
        : store_ (store), index_ (paramIndex), theme_ (theme), decimals_ (decimals),
          range_ (factory_params::makeRange (store.desc (paramIndex)))
    {
    }

    float Knob::currentNorm() const
    {
        return factory_params::convertTo0to1 (range_, store_.value (index_));
    }

    void Knob::writeNorm (float norm)
    {
        norm = std::clamp (norm, 0.0f, 1.0f);
        const float real = factory_params::convertFrom0to1 (range_, norm);
        store_.setFromUi (index_, real);
        redraw();
    }

    void Knob::rebind (int paramIndex)
    {
        index_    = paramIndex;
        range_    = factory_params::makeRange (store_.desc (paramIndex));
        dragging_ = false; // drop any in-flight drag so a rebind cannot write the old param
        redraw();
    }

    void Knob::draw (visage::Canvas& canvas)
    {
        const KnobMetrics& m = theme_.knob;
        const Palette& p = theme_.palette;
        const factory_params::ParamDesc& desc = store_.desc (index_);
        const std::uint32_t accent = accentOverride_ != 0 ? accentOverride_ : p.accent;

        // Layout (design reference): name row (top), dial (middle), value row (bottom).
        // Row heights + dial inset follow the per-instance profile when set (RS "big"
        // vs "small" match the JUCE RsKnob; default reproduces the gallery look).
        const float textTop = textTopPx_    >= 0.0f ? textTopPx_    : 16.0f;
        const float textBot = textBottomPx_ >= 0.0f ? textBottomPx_ : 16.0f;
        const float nameFontPx  = nameFontPx_  >= 0.0f ? nameFontPx_  : theme_.font.label;
        const float valueFontPx = valueFontPx_ >= 0.0f ? valueFontPx_ : theme_.font.label;
        const float dialTop = textTop;
        const float dialH = std::max (0.0f, height() - textTop - textBot);

        const float inset = dialInsetPx_ >= 0.0f ? dialInsetPx_ : m.boundsInset;
        const float bw = width() - 2.0f * inset;
        const float bh = dialH - 2.0f * inset;
        const float R = std::min (bw, bh) * 0.5f;
        const float cx = width() * 0.5f;
        const float cy = dialTop + inset + bh * 0.5f;

        const float band = R * m.lineWidthRatio;   // donut ring thickness
        const float arcR = R - band * 0.5f;         // ring centreline
        const float pos = currentNorm();
        const float toAngle = knobAngleForNorm (m, pos);

        // NB: no outer drop shadow behind the whole knob — v2.1.0's RsLookAndFeel
        // knob has none (the pale taupe ring it composited over the #fff4ee card
        // read as an unwanted #e7dbd5 halo). Only the inner body shadow (below)
        // is kept, matching v2.1.0's face treatment.

        // Full 360° flat donut in three solid zones of identical thickness `band`:
        //   (a) accent (per-knob colour) from the sweep start (−135°) to the value,
        //   (b) accentDim remainder from the value to the sweep end (+135°),
        //   (c) panelLo across the bottom 90° dead zone (+135° → +225°).
        // Each zone is tiled from small flatArc pieces (see fillArcBand) so a wide
        // zone renders as a clean solid band instead of collapsing.
        canvas.setColor (visage::Color (accent));
        fillArcBand (canvas, cx, cy, arcR, m.arcStart, toAngle, band);
        canvas.setColor (visage::Color (p.accentDim));
        fillArcBand (canvas, cx, cy, arcR, toAngle, m.arcEnd, band);
        canvas.setColor (visage::Color (p.panelLo));
        fillArcBand (canvas, cx, cy, arcR, m.arcEnd, m.arcStart + kTwoPi, band);

        // 1px track ring at the outer edge (inset hairline).
        canvas.setColor (visage::Color (p.track));
        canvas.ring (cx - R, cy - R, 2.0f * R, 1.0f);

        // Inner body: small drop shadow + radial-gradient white->panelLo, the light
        // centred high (50% / 36%) like the reference's inset highlight.
        const float bodyR = std::max (2.0f, R - band * m.bodyInsetFactor);
        canvas.setColor (visage::Color (0x246b5750)); // taupe .14
        canvas.roundedRectangleShadow (cx - bodyR, cy - bodyR + 1.0f, 2.0f * bodyR, 2.0f * bodyR, bodyR, 3.0f);
        canvas.setColor (visage::Brush::radial (visage::Color (0xffffffff), visage::Color (p.panelLo),
                                                visage::Point (cx, cy - bodyR * 0.28f), bodyR * 1.15f, bodyR * 1.15f));
        canvas.circle (cx - bodyR, cy - bodyR, 2.0f * bodyR);

        // Needle: a rounded bar from the centre outward, in the knob's accent.
        const float len = bodyR * m.needleLengthRatio;
        const visage::Point tip = knobNeedleTip (cx, cy, len, toAngle);
        canvas.setColor (visage::Color (accent));
        canvas.segment (cx, cy, tip.x, tip.y, m.needleWidthPx, true);

        // Name above (muted, bold), value below (accent, bold). Font sizes follow
        // the profile (JUCE RsKnob: big 12/13, small 10/11); default theme.font.label.
        canvas.setColor (visage::Color (p.textSecondary));
        canvas.text (nameOverride_.empty() ? desc.name : nameOverride_,
                     boldFont (nameFontPx), visage::Font::kCenter, 0.0f, 0.0f, width(), textTop);
        // Value readout, spaces stripped for the demo's tight "62%" / "120ms" look
        // (v2.1.0 RsKnob does the same: getTextFromValue(...).removeCharacters(" ")).
        std::string valueText = factory_params::formatValue (desc, store_.value (index_), decimals_);
        valueText.erase (std::remove (valueText.begin(), valueText.end(), ' '), valueText.end());
        canvas.setColor (visage::Color (accent));
        canvas.text (valueText, boldFont (valueFontPx), visage::Font::kCenter, 0.0f, height() - textBot, width(), textBot);
    }

    // The bottom value read-out row height (px) — the profile's textBottom, else the
    // widget default. The rotary excludes this row (like the JUCE RsKnob), so double-
    // clicking here edits the value instead of dragging / resetting the dial.
    float Knob::valueRowHeight() const
    {
        return textBottomPx_ >= 0.0f ? textBottomPx_ : 16.0f;
    }

    void Knob::mouseDown (const visage::MouseEvent& e)
    {
        // Value read-out row: with text entry wired (requestValueEntry set), a double-
        // click there opens the entry and a single click is a no-op — the JUCE RsKnob's
        // rotary excludes the value area, so it can't compete with a dial drag. Without
        // it wired (the gallery), the value row is NOT intercepted, so double-click-to-
        // reset still works there — the gallery is unaffected.
        if (requestValueEntry && e.position.y >= height() - valueRowHeight())
        {
            if (e.repeatClickCount() >= 2) openValueEntry();
            return;
        }

        // Alt-click OR double-click restores the default value (round-3 fix 5:
        // alt-click reset on every knob, matching the JUCE editor; Shift, not Alt,
        // is fine-drag, so resetting on alt-down never conflicts with a drag). A
        // single click is repeat count 1 in visage (a double-click is 2), so the
        // double-click threshold must be >= 2 — else every press would reset.
        if (e.isAltDown() || e.repeatClickCount() >= 2)
        {
            const factory_params::ParamDesc& desc = store_.desc (index_);
            store_.setFromUiGestured (index_, desc.defaultValue);
            dragging_ = false;
            redraw();
            return;
        }

        dragging_ = true;
        dragNorm_ = currentNorm();
        lastDragPos_ = e.position; // frame-local anchor; successive positions give true deltas
        store_.beginGesture (index_);
    }

    void Knob::mouseDrag (const visage::MouseEvent& e)
    {
        if (! dragging_)
            return;

        const visage::Point pos = e.position; // frame-local position (not a move delta)
        const float dx = pos.x - lastDragPos_.x;
        const float dy = pos.y - lastDragPos_.y;
        lastDragPos_ = pos;

        // RotaryHorizontalVerticalDrag: dragging right or up increases the value.
        const float pixelsForFullRange = 250.0f; // JUCE default drag sensitivity
        const float fine = e.isShiftDown() ? 0.25f : 1.0f;
        dragNorm_ = std::clamp (dragNorm_ + (dx - dy) / pixelsForFullRange * fine, 0.0f, 1.0f);
        writeNorm (dragNorm_);
    }

    void Knob::mouseUp (const visage::MouseEvent&)
    {
        if (dragging_)
        {
            dragging_ = false;
            store_.endGesture (index_);
        }
    }

    bool Knob::mouseWheel (const visage::MouseEvent& e)
    {
        const float raw = e.wheel_delta_y != 0.0f ? e.wheel_delta_y : e.precise_wheel_delta_y;
        if (raw == 0.0f)
            return false;

        const float step = e.isShiftDown() ? 0.01f : 0.05f;
        const float norm = std::clamp (currentNorm() + (raw > 0.0f ? step : -step), 0.0f, 1.0f);
        store_.beginGesture (index_);
        writeNorm (norm);
        store_.endGesture (index_);
        return true;
    }

    void Knob::openValueEntry()
    {
        if (! requestValueEntry) return;
        const float rowH = valueRowHeight();
        const visage::Point o = positionInWindow();
        ValueEntryRequest req;
        req.x = o.x; req.y = o.y + height() - rowH; req.w = width(); req.h = rowH;
        req.prefill = entryPrefillText (store_.desc (index_), store_.value (index_), decimals_);
        req.fontPx  = valueFontPx_ >= 0.0f ? valueFontPx_ : theme_.font.label;
        req.commit  = [this] (const std::string& t) { commitValueEntry (t); };
        requestValueEntry (req);
    }

    void Knob::commitValueEntry (const std::string& text)
    {
        if (commitEntryText (store_, index_, text)) // invalid/empty REVERTS (ValueText.h)
            redraw();
    }
}
