#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <cmath>

//
// The centrepiece: a spectrum display showing the input magnitude, the live
// per-frequency gain reduction (teal "curtain" — what the suppressor is cutting
// right now), and the user reduction-profile curve with draggable nodes
// (double-click to add at a frequency, double-click a node to remove, drag for
// frequency/amount). Edits go through the APVTS. GUI-thread only.
//
class SuppressionCurveComponent : public juce::Component,
                                  private juce::Timer
{
public:
    SuppressionCurveComponent (ResonanceSuppressorAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        factory_ui::paintCard (g, r, 10.0f);

        plot = r.reduced (12.0f);
        plot.removeFromBottom (14.0f); // Hz labels

        drawGrid (g);
        drawAnalyzer (g);
        drawReduction (g);
        drawProfile (g);
        drawNodes (g);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = nodeAt (e.position);
        if (hit >= 0) { dragging = hit; beginGesture (hit); }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const float f = xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x));
        const float a = yToAmt  (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y));
        setParam (ResonanceSuppressorAudioProcessor::nodePid (dragging, "freq"), f);
        setParam (ResonanceSuppressorAudioProcessor::nodePid (dragging, "amt"),  a);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) endGesture (dragging);
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int hit = nodeAt (e.position);
        if (hit >= 0) { setParam (ResonanceSuppressorAudioProcessor::nodePid (hit, "on"), 0.0f); repaint(); return; }

        int slot = -1;
        for (int n = 0; n < ResonanceSuppressorAudioProcessor::kNumNodes; ++n)
            if (! nodeOn (n)) { slot = n; break; }
        if (slot < 0) return;

        const float x = juce::jlimit (plot.getX(), plot.getRight(), e.position.x);
        setParam (ResonanceSuppressorAudioProcessor::nodePid (slot, "freq"), xToFreq (x));
        setParam (ResonanceSuppressorAudioProcessor::nodePid (slot, "amt"),
                  yToAmt (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y)));
        setParam (ResonanceSuppressorAudioProcessor::nodePid (slot, "on"), 1.0f);
        repaint();
    }

private:
    void timerCallback() override { repaint(); }

    static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;
    static constexpr float kAmtMin = -1.0f, kAmtMax = 2.0f;

    float freqToX (float f) const
    {
        const float t = std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / std::log (1000.0f);
        return plot.getX() + t * plot.getWidth();
    }
    float xToFreq (float x) const
    {
        return 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth());
    }
    float dbToY (float db) const
    {
        return plot.getY() + (kMaxDb - juce::jlimit (kMinDb, kMaxDb, db)) / (kMaxDb - kMinDb) * plot.getHeight();
    }
    float amtToY (float a) const
    {
        return plot.getBottom() - (juce::jlimit (kAmtMin, kAmtMax, a) - kAmtMin) / (kAmtMax - kAmtMin) * plot.getHeight();
    }
    float yToAmt (float y) const
    {
        return kAmtMin + (plot.getBottom() - y) / plot.getHeight() * (kAmtMax - kAmtMin);
    }

    bool  nodeOn (int n) const { return apvts.getRawParameterValue (ResonanceSuppressorAudioProcessor::nodePid (n, "on"))->load() > 0.5f; }
    float nodeFreq (int n) const { return apvts.getRawParameterValue (ResonanceSuppressorAudioProcessor::nodePid (n, "freq"))->load(); }
    float nodeAmt (int n) const { return apvts.getRawParameterValue (ResonanceSuppressorAudioProcessor::nodePid (n, "amt"))->load(); }

    void drawGrid (juce::Graphics& g)
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        struct FL { float f; const char* s; };
        for (auto fl : { FL{50,"50"}, FL{100,"100"}, FL{200,"200"}, FL{500,"500"},
                         FL{1000,"1k"}, FL{2000,"2k"}, FL{5000,"5k"}, FL{10000,"10k"} })
        {
            const float x = freqToX (fl.f);
            g.setColour (FactoryLookAndFeel::track().withAlpha (0.7f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (FactoryLookAndFeel::textDim());
            g.drawText (fl.s, juce::Rectangle<float> (x - 18.0f, plot.getBottom() + 1.0f, 36.0f, 12.0f),
                        juce::Justification::centred);
        }
        for (float db = 0.0f; db >= -60.0f; db -= 12.0f)
        {
            const float y = dbToY (db);
            g.setColour (FactoryLookAndFeel::track().withAlpha (db == 0.0f ? 0.9f : 0.4f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
    }

    void drawAnalyzer (juce::Graphics& g)
    {
        const int bins = processor.binsForDisplay();
        const double sr = processor.getSampleRateForDisplay();
        const int N = 2 * (bins - 1);
        juce::Path fill;
        bool started = false;
        for (int k = 1; k < bins; ++k)
        {
            const float f = (float) ((double) k * sr / N);
            if (f < 20.0f || f > 20000.0f) continue;
            const float x = freqToX (f);
            const float y = dbToY (processor.displayMagDb (k));
            if (! started) { fill.startNewSubPath (x, plot.getBottom()); fill.lineTo (x, y); started = true; }
            else fill.lineTo (x, y);
        }
        if (started)
        {
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.closeSubPath();
            juce::ColourGradient grad (FactoryLookAndFeel::accent().withAlpha (0.28f), 0.0f, plot.getY(),
                                       FactoryLookAndFeel::accent().withAlpha (0.02f), 0.0f, plot.getBottom(), false);
            g.setGradientFill (grad);
            g.fillPath (fill);
        }
    }

    // Live gain reduction hanging from the 0 dB line — the suppressor at work.
    void drawReduction (juce::Graphics& g)
    {
        const int bins = processor.binsForDisplay();
        const double sr = processor.getSampleRateForDisplay();
        const int N = 2 * (bins - 1);
        const float top = dbToY (0.0f);
        juce::Path fill;
        bool started = false;
        for (int k = 1; k < bins; ++k)
        {
            const float f = (float) ((double) k * sr / N);
            if (f < 20.0f || f > 20000.0f) continue;
            const float x = freqToX (f);
            const float red = juce::jlimit (-60.0f, 0.0f, processor.displayRedDb (k));
            const float y = dbToY (red); // red <= 0 -> below the top line
            if (! started) { fill.startNewSubPath (x, top); fill.lineTo (x, y); started = true; }
            else fill.lineTo (x, y);
        }
        if (started)
        {
            fill.lineTo (plot.getRight(), top);
            fill.closeSubPath();
            g.setColour (kTeal.withAlpha (0.28f));
            g.fillPath (fill);
            g.setColour (kTeal.withAlpha (0.8f));
            g.strokePath (fill, juce::PathStrokeType (1.0f));
        }
    }

    void drawProfile (juce::Graphics& g)
    {
        // Smooth profile multiplier across frequency (same model as the processor).
        const int steps = juce::jmax (2, (int) plot.getWidth());
        juce::Path curve;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot.getX() + (float) i * plot.getWidth() / steps;
            const float f = xToFreq (x);
            const float lf = std::log (juce::jmax (10.0f, f));
            float mul = 1.0f;
            for (int n = 0; n < ResonanceSuppressorAudioProcessor::kNumNodes; ++n)
                if (nodeOn (n))
                {
                    const float d = (lf - std::log (juce::jmax (10.0f, nodeFreq (n)))) / 0.30f;
                    mul += nodeAmt (n) * std::exp (-0.5f * d * d);
                }
            const float y = amtToY (juce::jlimit (kAmtMin, kAmtMax, mul));
            if (i == 0) curve.startNewSubPath (x, y);
            else        curve.lineTo (x, y);
        }
        g.setColour (FactoryLookAndFeel::text().withAlpha (0.55f));
        g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
    }

    void drawNodes (juce::Graphics& g)
    {
        for (int n = 0; n < ResonanceSuppressorAudioProcessor::kNumNodes; ++n)
        {
            if (! nodeOn (n)) continue;
            const juce::Point<float> p (freqToX (nodeFreq (n)), amtToY (nodeAmt (n)));
            const auto col = FactoryLookAndFeel::bandColour (n);
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (p));
            g.setColour (col);
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p));
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (n + 1), juce::Rectangle<float> (14.0f, 14.0f).withCentre (p), juce::Justification::centred);
        }
    }

    int nodeAt (juce::Point<float> pos) const
    {
        int best = -1; float bestD = 14.0f;
        for (int n = 0; n < ResonanceSuppressorAudioProcessor::kNumNodes; ++n)
        {
            if (! nodeOn (n)) continue;
            const juce::Point<float> p (freqToX (nodeFreq (n)), amtToY (nodeAmt (n)));
            const float d = p.getDistanceFrom (pos);
            if (d <= bestD) { bestD = d; best = n; }
        }
        return best;
    }

    void setParam (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (value));
    }
    void beginGesture (int n)
    {
        if (auto* p = apvts.getParameter (ResonanceSuppressorAudioProcessor::nodePid (n, "freq"))) p->beginChangeGesture();
        if (auto* p = apvts.getParameter (ResonanceSuppressorAudioProcessor::nodePid (n, "amt")))  p->beginChangeGesture();
    }
    void endGesture (int n)
    {
        if (auto* p = apvts.getParameter (ResonanceSuppressorAudioProcessor::nodePid (n, "freq"))) p->endChangeGesture();
        if (auto* p = apvts.getParameter (ResonanceSuppressorAudioProcessor::nodePid (n, "amt")))  p->endChangeGesture();
    }

    inline static const juce::Colour kTeal { juce::Colour (0xff45b8acu) };

    ResonanceSuppressorAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;
    int dragging = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuppressionCurveComponent)
};
