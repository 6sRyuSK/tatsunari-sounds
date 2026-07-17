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
        constexpr float kPi = 3.14159265358979323846f;

        // Stroke a circular arc with rounded caps, in JUCE's angle convention:
        // angle 0 points at 12 o'clock and grows clockwise, so a point at angle a
        // is (cx + r*sin a, cy - r*cos a). The caller sets the brush/colour first.
        void strokeArc (visage::Canvas& canvas, float cx, float cy, float r,
                        float a0, float a1, float strokeWidth)
        {
            const float sweep = a1 - a0;
            const float stepRad = 2.0f * kPi / 180.0f; // ~2 degrees per segment
            const int steps = std::max (2, static_cast<int> (std::ceil (std::abs (sweep) / stepRad)));

            visage::Path path;
            for (int i = 0; i <= steps; ++i)
            {
                const float a = a0 + sweep * (static_cast<float> (i) / static_cast<float> (steps));
                const visage::Point p (cx + r * std::sin (a), cy - r * std::cos (a));
                if (i == 0)
                    path.moveTo (p);
                else
                    path.lineTo (p);
            }
            canvas.fill (path.stroke (strokeWidth, visage::Path::Join::Round, visage::Path::EndCap::Round));
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
        const factory_params::ParamDesc& desc = store_.desc (index_);

        // Layout: dial square on top, two text rows (value, name) below.
        const float textH = 18.0f;
        const float knobAreaH = std::max (0.0f, height() - 2.0f * textH);

        const float inset = m.boundsInset;
        const float bw = width() - 2.0f * inset;
        const float bh = knobAreaH - 2.0f * inset;
        const float radius = std::min (bw, bh) * 0.5f;
        const float cx = width() * 0.5f;
        const float cy = inset + bh * 0.5f;

        const float lineW = radius * m.lineWidthRatio;
        const float arcR = radius - lineW * 0.5f;
        const float pos = currentNorm();
        const float toAngle = m.arcStart + pos * (m.arcEnd - m.arcStart);

        // Track arc.
        canvas.setColor (visage::Color (theme_.palette.track));
        strokeArc (canvas, cx, cy, arcR, m.arcStart, m.arcEnd, lineW);

        // Value arc (soft glow underlay + solid accent).
        if (pos > 0.0f)
        {
            canvas.setColor (visage::Color (theme_.palette.accent).withAlpha (m.glowAlpha));
            strokeArc (canvas, cx, cy, arcR, m.arcStart, toAngle, lineW * m.glowWidthFactor);
            canvas.setColor (visage::Color (theme_.palette.accent));
            strokeArc (canvas, cx, cy, arcR, m.arcStart, toAngle, lineW);
        }

        // Knob body: soft drop shadow, top-lit gradient, thin track outline.
        const float bodyR = radius - lineW * m.bodyInsetFactor;
        const float bodyD = bodyR * 2.0f;
        const float bodyX = cx - bodyR;
        const float bodyY = cy - bodyR;
        const float blur = std::max (3.0f, bodyR * m.shadowBlurFactor);

        canvas.setColor (visage::Color (theme_.palette.shadow));
        canvas.roundedRectangleShadow (bodyX + m.shadowOffsetX, bodyY + m.shadowOffsetY,
                                       bodyD, bodyD, bodyR, blur);

        canvas.setColor (visage::Brush::vertical (visage::Color (0xffffffff),
                                                  visage::Color (theme_.palette.panelLo)));
        canvas.circle (bodyX, bodyY, bodyD);

        canvas.setColor (visage::Color (theme_.palette.track));
        canvas.ring (bodyX, bodyY, bodyD, 1.0f);

        // Pointer dot near the rim.
        const float dotR = std::max (2.0f, lineW * m.pointerDotFactor);
        const float pr = bodyR * m.pointerPosFactor;
        const float dotX = cx + pr * std::sin (toAngle);
        const float dotY = cy - pr * std::cos (toAngle);
        canvas.setColor (visage::Color (theme_.palette.accent));
        canvas.circle (dotX - dotR, dotY - dotR, dotR * 2.0f);

        // Value + name text below the dial.
        canvas.setColor (visage::Color (theme_.palette.text));
        canvas.text (factory_params::formatValue (desc, store_.value (index_), decimals_),
                     regularFont (theme_.font.label), visage::Font::kCenter,
                     0.0f, knobAreaH, width(), textH);
        canvas.text (desc.name, regularFont (theme_.font.label), visage::Font::kCenter,
                     0.0f, knobAreaH + textH, width(), textH);
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
