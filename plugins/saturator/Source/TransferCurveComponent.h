#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"

//
// Live display of the saturator's transfer curve y = f(x). Polls the processor
// for a shaper configured from the current parameters and redraws at ~30 fps.
// Pure GUI-thread code.
//
class TransferCurveComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit TransferCurveComponent (SaturatorAudioProcessor& p) : processor (p)
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

        auto plot = r.reduced (10.0f);

        // Grid + axes.
        g.setColour (FactoryLookAndFeel::track());
        for (int i = 1; i < 4; ++i)
        {
            const float fx = plot.getX() + plot.getWidth()  * i / 4.0f;
            const float fy = plot.getY() + plot.getHeight() * i / 4.0f;
            g.drawVerticalLine   ((int) fx, plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) fy, plot.getX(), plot.getRight());
        }
        g.setColour (FactoryLookAndFeel::textDim());
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        g.drawVerticalLine   ((int) plot.getCentreX(), plot.getY(), plot.getBottom());

        // Reference unity line (y = x), dashed.
        {
            juce::Path unity;
            unity.startNewSubPath (plot.getX(), plot.getBottom());
            unity.lineTo (plot.getRight(), plot.getY());
            const float dashes[] = { 4.0f, 4.0f };
            g.setColour (FactoryLookAndFeel::textDim().withAlpha (0.5f));
            juce::Path dashed;
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, unity, dashes, 2);
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }

        // The curve. x spans [-range, range]; y is clamped to the same window.
        constexpr double range = 1.4;
        const auto shaper = processor.makeDisplayShaper();

        juce::Path curve;
        const int steps = juce::jmax (2, (int) plot.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const double t = (double) i / steps;        // 0..1
            const double x = -range + 2.0 * range * t;  // -range..range
            const double y = shaper.processSample (x);

            const float px = plot.getX() + (float) t * plot.getWidth();
            const float ny = (float) juce::jlimit (-range, range, y);
            const float py = plot.getCentreY() - (ny / (float) range) * plot.getHeight() * 0.5f;

            if (i == 0) curve.startNewSubPath (px, py);
            else        curve.lineTo (px, py);
        }
        g.setColour (FactoryLookAndFeel::accent());
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    void timerCallback() override { repaint(); }

    SaturatorAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransferCurveComponent)
};
