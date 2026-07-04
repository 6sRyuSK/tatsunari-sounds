#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

//
// factory_ui spectrum-display helpers — the coordinate mapping and path drawing
// shared by every plugin's spectrum view, kept separate from the analyser maths
// (SpectrumAnalyzerModel.h) so displays that get their magnitudes elsewhere can
// still reuse the drawing. Header-only, GUI-thread only.
//
// Per-spectrum colours are intentionally caller parameters, not baked in, so a
// display can tint each trace independently.
//
namespace factory_ui
{
    // 20 Hz – 20 kHz logarithmic frequency axis across a plot rectangle: three
    // decades spread evenly over the width (the "standard" analyser mapping).
    struct LogFreqAxis
    {
        juce::Rectangle<float> plot;

        static constexpr float kMinHz = 20.0f;
        static constexpr float kMaxHz = 20000.0f;
        static constexpr float kSpan  = 1000.0f; // kMinHz * kSpan == kMaxHz

        float freqToX (float f) const
        {
            const float t = std::log (juce::jlimit (kMinHz, kMaxHz, f) / kMinHz) / std::log (kSpan);
            return plot.getX() + t * plot.getWidth();
        }
        float xToFreq (float x) const
        {
            const float t = (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth());
            return kMinHz * std::pow (kSpan, t);
        }
    };

    // Linear vertical value→Y mapping. `topValue` / `bottomValue` are the data
    // values at the plot's top / bottom edges (e.g. 0 dB at the top, −96 dB at the
    // foot); values are clamped to the endpoints' range, as every analyser does.
    struct VerticalAxis
    {
        juce::Rectangle<float> plot;
        float topValue = 0.0f;
        float bottomValue = -100.0f;

        float toY (float v) const
        {
            const float lo = juce::jmin (topValue, bottomValue);
            const float hi = juce::jmax (topValue, bottomValue);
            return juce::jmap (juce::jlimit (lo, hi, v), topValue, bottomValue, plot.getY(), plot.getBottom());
        }
    };

    // Accumulates a spectrum outline as points are fed left-to-right, exposing
    // both the open polyline (for peak-hold strokes) and a closed area down to a
    // horizontal baseline (for gradient / solid fills).
    class SpectrumTrace
    {
    public:
        // `baselineY` is the flat foot of the filled area (e.g. the plot bottom, or
        // a 0 dB line); `rightX` is where the fill closes on the right edge.
        void begin (float baselineY, float rightX)
        {
            line_ = {};
            area_ = {};
            baselineY_ = baselineY;
            rightX_    = rightX;
            started_   = false;
        }

        void addPoint (float x, float y)
        {
            if (! started_)
            {
                line_.startNewSubPath (x, y);
                area_.startNewSubPath (x, baselineY_);
                area_.lineTo (x, y);
                started_ = true;
            }
            else
            {
                line_.lineTo (x, y);
                area_.lineTo (x, y);
            }
        }

        bool isEmpty() const noexcept { return ! started_; }

        // The open polyline through the fed points.
        const juce::Path& line() const noexcept { return line_; }

        // A closed area between the polyline and the baseline: from the first
        // point's x up to the curve, along it, then down to the baseline at rightX.
        juce::Path area() const
        {
            juce::Path a = area_;
            if (started_)
            {
                a.lineTo (rightX_, baselineY_);
                a.closeSubPath();
            }
            return a;
        }

    private:
        juce::Path line_, area_;
        float baselineY_ = 0.0f, rightX_ = 0.0f;
        bool  started_   = false;
    };

    // Fill a spectrum area with the house vertical gradient: `topAlpha` at the plot
    // top fading to `bottomAlpha` at its foot, in `colour`.
    inline void fillSpectrumArea (juce::Graphics& g, const juce::Path& area, juce::Colour colour,
                                  juce::Rectangle<float> plot, float topAlpha, float bottomAlpha)
    {
        juce::ColourGradient grad (colour.withAlpha (topAlpha),    0.0f, plot.getY(),
                                   colour.withAlpha (bottomAlpha), 0.0f, plot.getBottom(), false);
        g.setGradientFill (grad);
        g.fillPath (area);
    }
} // namespace factory_ui
