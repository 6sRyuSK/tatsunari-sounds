#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_core/FuzzEngine.h"
#include "factory_ui/FactoryLookAndFeel.h"

//
// The Fuzznari centrepiece: a large transfer-curve display you deform
// directly, drawn over the live output waveform — the "you are bending the
// waveform with your hands" interaction instead of a pedal metaphor.
//
//   - Knee handle (on the curve shoulder): drag horizontally → Drive.
//   - Ceiling handle (right edge, on the positive asymptote): drag vertically
//     → Bias. The asymmetric top/bottom flats become directly visible.
//   - Gate notch handle (on the x axis): drag horizontally → Gate; a shaded
//     dead-zone band shows how much of the input the bias starve swallows.
//   - Stab rail (left edge): drag vertically → Stab. Past the oscillation
//     onset (Squeal on) the curve grows jittering ghost copies and the handle
//     a warning halo — the instability is painted, not implied.
//   - Mouse wheel anywhere → Tone.
//
// All edits go through beginChangeGesture / setValueNotifyingHost /
// endChangeGesture (the EqCurveComponent idiom) so host automation records
// drags. The curve itself is FuzzEngine::shapeStatic at the idle operating
// point — the exact function the DSP applies, not an illustration of it.
//
class FuzzCurveComponent : public juce::Component,
                           private juce::Timer
{
public:
    explicit FuzzCurveComponent (FuzznariAudioProcessor& p) : processor (p)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r, 6.0f, 1.0f);

        const auto plot = plotArea();

        paintGrid (g, plot);
        paintWaveform (g, plot);
        paintGateZone (g, plot);
        paintCurve (g, plot);
        paintStabRail (g, plot);
        paintHandles (g);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto over = hitTest (e.position);
        if (over != hovered)
        {
            hovered = over;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = Handle::none;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = hitTest (e.position);
        if (auto* p = paramFor (dragging))
        {
            p->beginChangeGesture();
            dragStartValue = p->convertFrom0to1 (p->getValue());
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* p = paramFor (dragging);
        if (p == nullptr)
            return;

        const auto plot = plotArea();
        switch (dragging)
        {
            case Handle::knee:
            {
                // Knee x is where the curve reaches 75% of the clip ceiling:
                // tanh(d·x) = 0.75 → x = atanh(0.75)/d. Invert for d.
                const double x = std::clamp ((double) pixelToX (e.position.x, plot), 0.004, 1.0);
                const double driveLin = std::clamp (kKneeAtanh / x, 1.0, 251.19);
                setParam (p, (float) (20.0 * std::log10 (driveLin)));
                break;
            }
            case Handle::ceiling:
                // Positive bias flattens the positive side, so dragging the
                // ceiling down raises bias — the curve follows the hand.
                setParam (p, dragStartValue + (float) e.getDistanceFromDragStartY() * (200.0f / 140.0f));
                break;
            case Handle::gateNotch:
                setParam (p, (float) (100.0 * std::clamp ((double) pixelToX (e.position.x, plot) / kGateZoneSpan, 0.0, 1.0)));
                break;
            case Handle::stab:
            {
                const float t = juce::jlimit (0.0f, 1.0f,
                    (plot.getBottom() - e.position.y) / juce::jmax (1.0f, plot.getHeight()));
                setParam (p, 100.0f * t);
                break;
            }
            case Handle::none:
                break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (auto* p = paramFor (dragging))
            p->endChangeGesture();
        dragging = Handle::none;
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (auto* p = processor.apvts.getParameter ("tone"))
        {
            const float current = p->convertFrom0to1 (p->getValue());
            p->beginChangeGesture();
            setParam (p, current + wheel.deltaY * 40.0f);
            p->endChangeGesture();
        }
    }

private:
    enum class Handle { none, knee, ceiling, gateNotch, stab };

    static constexpr double kXRange       = 1.2;  // input span shown
    static constexpr double kYRange       = 1.4;  // output span shown
    static constexpr double kKneeAtanh    = 0.972955074527657; // atanh(0.75)
    static constexpr double kGateZoneSpan = 0.6;  // gate=1 → dead-zone half-width in x
    static constexpr float  kHitRadius    = 14.0f;

    // ---- Parameter access ----------------------------------------------------
    float paramValue (const char* id) const
    {
        return processor.apvts.getRawParameterValue (id)->load();
    }

    juce::RangedAudioParameter* paramFor (Handle h) const
    {
        switch (h)
        {
            case Handle::knee:      return processor.apvts.getParameter ("drive");
            case Handle::ceiling:   return processor.apvts.getParameter ("bias");
            case Handle::gateNotch: return processor.apvts.getParameter ("gate");
            case Handle::stab:      return processor.apvts.getParameter ("stab");
            case Handle::none:      break;
        }
        return nullptr;
    }

    static void setParam (juce::RangedAudioParameter* p, float value)
    {
        p->setValueNotifyingHost (p->convertTo0to1 (p->getNormalisableRange().snapToLegalValue (value)));
    }

    // The idle operating point (env = 0): what the DSP applies to a signal
    // just poking out of silence — bias plus the full gate starve.
    double idleBias() const
    {
        return factory_core::FuzzEngine::kBiasScale * paramValue ("bias") * 0.01
             + factory_core::FuzzEngine::kGateDepth * paramValue ("gate") * 0.01;
    }

    double driveLin() const { return std::pow (10.0, paramValue ("drive") / 20.0); }

    double shape (double x) const
    {
        return factory_core::FuzzEngine::shapeStatic (x, driveLin(), idleBias());
    }

    bool inOscZone() const
    {
        return paramValue ("osc") > 0.5f && paramValue ("stab") > 45.0f;
    }

    // ---- Geometry --------------------------------------------------------------
    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced (26.0f, 14.0f);
    }

    float xToPixel (double x, juce::Rectangle<float> plot) const
    {
        return plot.getX() + (float) ((x + kXRange) / (2.0 * kXRange)) * plot.getWidth();
    }

    float yToPixel (double y, juce::Rectangle<float> plot) const
    {
        const double clamped = juce::jlimit (-kYRange, kYRange, y);
        return plot.getCentreY() - (float) (clamped / kYRange) * plot.getHeight() * 0.5f;
    }

    double pixelToX (float px, juce::Rectangle<float> plot) const
    {
        return ((px - plot.getX()) / juce::jmax (1.0f, plot.getWidth())) * 2.0 * kXRange - kXRange;
    }

    juce::Point<float> handlePos (Handle h) const
    {
        const auto plot = plotArea();
        switch (h)
        {
            case Handle::knee:
            {
                const double x = juce::jlimit (0.02, 1.0, kKneeAtanh / driveLin());
                return { xToPixel (x, plot), yToPixel (shape (x), plot) };
            }
            case Handle::ceiling:
                return { xToPixel (1.15, plot), yToPixel (shape (1.15), plot) };
            case Handle::gateNotch:
                return { xToPixel (kGateZoneSpan * paramValue ("gate") * 0.01, plot), yToPixel (0.0, plot) };
            case Handle::stab:
                return { plot.getX() + 6.0f,
                         plot.getBottom() - (paramValue ("stab") * 0.01f) * plot.getHeight() };
            case Handle::none:
                break;
        }
        return {};
    }

    Handle hitTest (juce::Point<float> pos) const
    {
        for (auto h : { Handle::knee, Handle::ceiling, Handle::gateNotch, Handle::stab })
            if (handlePos (h).getDistanceFrom (pos) < kHitRadius)
                return h;
        return Handle::none;
    }

    // ---- Painting ----------------------------------------------------------------
    void paintGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        g.setColour (FactoryLookAndFeel::track());
        for (int i = 1; i < 4; ++i)
        {
            const float fx = plot.getX() + plot.getWidth()  * (float) i / 4.0f;
            const float fy = plot.getY() + plot.getHeight() * (float) i / 4.0f;
            g.drawVerticalLine   ((int) fx, plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) fy, plot.getX(), plot.getRight());
        }
        g.setColour (FactoryLookAndFeel::textDim());
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        g.drawVerticalLine   ((int) plot.getCentreX(), plot.getY(), plot.getBottom());
    }

    void paintWaveform (juce::Graphics& g, juce::Rectangle<float> plot)
    {
        const int numSamples = (int) scope.size();
        processor.copyScopeSamples (scope.data(), numSamples);

        // Min/max column reduction: sputter and squeal become visible shapes.
        g.setColour (FactoryLookAndFeel::accentDim().withAlpha (0.55f));
        const int cols = juce::jmax (1, (int) plot.getWidth());
        for (int c = 0; c < cols; ++c)
        {
            const int i0 = c * numSamples / cols;
            const int i1 = juce::jmax (i0 + 1, (c + 1) * numSamples / cols);
            float lo = 1.0e9f, hi = -1.0e9f;
            for (int i = i0; i < i1; ++i)
            {
                lo = juce::jmin (lo, scope[(size_t) i]);
                hi = juce::jmax (hi, scope[(size_t) i]);
            }
            const float px = plot.getX() + (float) c;
            g.drawVerticalLine ((int) px, yToPixel (hi, plot), juce::jmax (yToPixel (lo, plot), yToPixel (hi, plot) + 1.0f));
        }
    }

    void paintGateZone (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        const double half = kGateZoneSpan * paramValue ("gate") * 0.01;
        if (half <= 0.0)
            return;
        const float x0 = xToPixel (-half, plot);
        const float x1 = xToPixel (half, plot);
        g.setColour (FactoryLookAndFeel::accentDim().withAlpha (0.30f));
        g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, plot.getHeight()));
    }

    juce::Path curvePath (juce::Rectangle<float> plot, float yOffsetPx) const
    {
        juce::Path path;
        const int steps = juce::jmax (2, (int) plot.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const double x = -kXRange + 2.0 * kXRange * i / steps;
            const float px = plot.getX() + (float) i / (float) steps * plot.getWidth();
            const float py = yToPixel (shape (x), plot) + yOffsetPx;
            if (i == 0) path.startNewSubPath (px, py);
            else        path.lineTo (px, py);
        }
        return path;
    }

    void paintCurve (juce::Graphics& g, juce::Rectangle<float> plot)
    {
        // Dashed unity reference.
        {
            juce::Path unity;
            unity.startNewSubPath (xToPixel (-1.0, plot), yToPixel (-1.0, plot));
            unity.lineTo (xToPixel (1.0, plot), yToPixel (1.0, plot));
            const float dashes[] = { 4.0f, 4.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, unity, dashes, 2);
            g.setColour (FactoryLookAndFeel::textDim().withAlpha (0.5f));
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }

        // Ghost copies jittering with Stab: the curve itself looks unstable
        // once the loop can sing.
        if (inOscZone())
        {
            const float amount = (paramValue ("stab") * 0.01f - 0.45f) * 22.0f;
            g.setColour (FactoryLookAndFeel::accent().withAlpha (0.22f));
            for (int ghost = 0; ghost < 2; ++ghost)
            {
                const float jitter = (rng.nextFloat() * 2.0f - 1.0f) * amount;
                g.strokePath (curvePath (plot, jitter), juce::PathStrokeType (2.0f));
            }
        }

        g.setColour (FactoryLookAndFeel::accent());
        g.strokePath (curvePath (plot, 0.0f),
                      juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void paintStabRail (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        const float x = plot.getX() + 6.0f;
        g.setColour (FactoryLookAndFeel::track());
        g.drawLine (x, plot.getY() + 4.0f, x, plot.getBottom() - 4.0f, 2.0f);
    }

    void paintHandles (juce::Graphics& g)
    {
        struct Spec { Handle h; const char* caption; };
        for (const auto& spec : { Spec { Handle::knee, "DRIVE" },
                                  Spec { Handle::ceiling, "BIAS" },
                                  Spec { Handle::gateNotch, "GATE" },
                                  Spec { Handle::stab, "STAB" } })
        {
            const auto pos = handlePos (spec.h);
            const bool active = hovered == spec.h || dragging == spec.h;
            const float radius = active ? 8.0f : 6.0f;

            if (spec.h == Handle::stab && inOscZone())
            {
                // Warning halo: this handle is holding a live oscillator.
                g.setColour (FactoryLookAndFeel::accent().withAlpha (0.30f));
                g.fillEllipse (juce::Rectangle<float> (radius * 3.2f, radius * 3.2f).withCentre (pos));
            }

            if (active)
            {
                g.setColour (FactoryLookAndFeel::accentDim());
                g.fillEllipse (juce::Rectangle<float> (radius * 2.6f, radius * 2.6f).withCentre (pos));
            }
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (pos));
            g.setColour (FactoryLookAndFeel::accent());
            g.fillEllipse (juce::Rectangle<float> (radius * 1.2f, radius * 1.2f).withCentre (pos));

            if (active)
            {
                g.setColour (FactoryLookAndFeel::text());
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (spec.caption,
                            juce::Rectangle<float> (48.0f, 12.0f).withCentre (pos.translated (0.0f, -16.0f)),
                            juce::Justification::centred);
            }
        }
    }

    void timerCallback() override { repaint(); }

    FuzznariAudioProcessor& processor;
    std::array<float, 2048> scope {};
    Handle hovered  = Handle::none;
    Handle dragging = Handle::none;
    float  dragStartValue = 0.0f;
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FuzzCurveComponent)
};
