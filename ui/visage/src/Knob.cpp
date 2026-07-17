#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_params/Text.h"

#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>

namespace factory_ui_visage
{
    namespace
    {
        constexpr float kPi    = 3.14159265358979323846f;
        constexpr float kTwoPi = 6.28318530717958648f;

        // Fill a flat donut band spanning [a0, a1] (a1 >= a0, radians; dial angle
        // convention: 0 = 12 o'clock, growing clockwise) at centreline radius r with
        // the given thickness, using visage's native flatArc GPU primitive (a solid,
        // anti-aliased band — no path-fill atlas, which the RS frame's large analyser
        // paths otherwise poison, silently dropping every path fill).
        //
        // flatArc takes a mid angle + half-aperture; its mirrored-arc SDF (and
        // visage's own half-aperture clamp to π) do NOT render a single wide band
        // faithfully — a zone approaching/over ~180° (a near-full value ring, or the
        // pale remainder at a low value) over-covers its neighbours and the narrow
        // zones drop out, collapsing the three-zone donut. So tile the span with
        // small sub-arcs (each well inside the SDF's clean regime); pieces of one
        // zone share the caller's brush, and a tiny angular overlap closes AA seams.
        void fillArcBand (visage::Canvas& canvas, float cx, float cy, float r,
                          float a0, float a1, float thickness)
        {
            constexpr float kMaxSpan = 0.30f;  // ≈17° per piece — flatArc is faithful here
            constexpr float kOverlap = 0.02f;  // ~1.1° same-brush overlap (hides seams)
            const float total = a1 - a0;
            if (total <= 1.0e-4f)
                return;
            const int   pieces = std::max (1, (int) std::ceil (total / kMaxSpan));
            const float span   = total / (float) pieces;
            for (int i = 0; i < pieces; ++i)
            {
                const float s = a0 + (float) i * span;
                const float e = std::min (a1, a0 + (float) (i + 1) * span + (i < pieces - 1 ? kOverlap : 0.0f));
                // flatArc renders a piece at screen angle (passed + 90°) in our dial
                // convention (0 = 12 o'clock, growing clockwise — the same the needle
                // uses; the +90° offset comes from visage's origin-flip on WebGL,
                // measured empirically). So pass (intended mid − 90°) to land the
                // piece where we want it. Normalise to [-π, π] (sin/cos are periodic)
                // since the raw sweep runs past 2π for the dead zone.
                float mid = 0.5f * (s + e) - 0.5f * kPi;
                while (mid >  kPi) mid -= kTwoPi;
                while (mid < -kPi) mid += kTwoPi;
                canvas.arc (cx - r, cy - r, 2.0f * r, thickness, mid, 0.5f * (e - s), /*rounded*/ false);
            }
        }
    } // namespace

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

    void Knob::draw (visage::Canvas& canvas)
    {
        const KnobMetrics& m = theme_.knob;
        const Palette& p = theme_.palette;
        const factory_params::ParamDesc& desc = store_.desc (index_);
        const std::uint32_t accent = accentOverride_ != 0 ? accentOverride_ : p.accent;

        // Layout (design reference): name row (top), dial (middle), value row (bottom).
        const float textH = 16.0f;
        const float dialTop = textH;
        const float dialH = std::max (0.0f, height() - 2.0f * textH);

        const float inset = m.boundsInset;
        const float bw = width() - 2.0f * inset;
        const float bh = dialH - 2.0f * inset;
        const float R = std::min (bw, bh) * 0.5f;
        const float cx = width() * 0.5f;
        const float cy = dialTop + inset + bh * 0.5f;

        const float band = R * m.lineWidthRatio;   // donut ring thickness
        const float arcR = R - band * 0.5f;         // ring centreline
        const float pos = currentNorm();
        const float toAngle = m.arcStart + pos * (m.arcEnd - m.arcStart);

        // Outer drop shadow behind the whole knob (taupe .16, 0 4px 10px).
        canvas.setColor (visage::Color (0x296b5750));
        canvas.roundedRectangleShadow (cx - R + m.shadowOffsetX, cy - R + m.shadowOffsetY,
                                       2.0f * R, 2.0f * R, R, m.shadowBlurFactor);

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
        canvas.setColor (visage::Color (accent));
        canvas.segment (cx, cy, cx + len * std::sin (toAngle), cy - len * std::cos (toAngle), m.needleWidthPx, true);

        // Name above (muted, bold), value below (accent, bold).
        canvas.setColor (visage::Color (p.textSecondary));
        canvas.text (nameOverride_.empty() ? desc.name : nameOverride_,
                     boldFont (theme_.font.label), visage::Font::kCenter, 0.0f, 0.0f, width(), textH);
        canvas.setColor (visage::Color (accent));
        canvas.text (factory_params::formatValue (desc, store_.value (index_), decimals_),
                     boldFont (theme_.font.label), visage::Font::kCenter, 0.0f, height() - textH, width(), textH);
    }

    void Knob::mouseDown (const visage::MouseEvent& e)
    {
        // Double-click restores the default value. A single click is repeat count
        // 1 in visage (a double-click is 2), so the threshold must be >= 2 — else
        // every press would reset instead of starting a drag.
        if (e.repeatClickCount() >= 2)
        {
            const factory_params::ParamDesc& desc = store_.desc (index_);
            store_.beginGesture (index_);
            store_.setFromUi (index_, desc.defaultValue);
            store_.endGesture (index_);
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
}
