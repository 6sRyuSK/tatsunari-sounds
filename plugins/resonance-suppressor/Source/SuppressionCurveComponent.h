#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "NodePanel.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <array>
#include <cmath>

//
// The centrepiece: a spectrum display showing the output (post-processing)
// magnitude, the live per-frequency gain reduction (teal "curtain" — what the
// suppressor is cutting right now), and the soothe-style reduction-profile curve
// with fixed nodes: a low cut and a high cut (drag frequency) plus four typed
// bands (drag frequency/sensitivity). Selecting a node opens a small bright
// inline editor (On + slope/type + freq [+ sens]) over the analyser bottom.
// Disabled nodes stay dimmed (never hidden). Edits go through the APVTS.
// GUI-thread only.
//
class SuppressionCurveComponent : public juce::Component,
                                  private juce::Timer
{
public:
    SuppressionCurveComponent (ResonanceSuppressorAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s), panel (s)
    {
        addChildComponent (panel); // shown when a node is selected
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        factory_ui::paintCard (g, getLocalBounds().toFloat(), 10.0f);
        plot = plotRect();

        drawGrid (g);
        drawAnalyzer (g);
        drawReduction (g);
        drawProfile (g);
        drawNodes (g);

        // Soft shadow so the selected-node editor floats above the analyser.
        if (panel.isVisible())
            factory_ui::dropShadowFor (g, panel.getBounds(), 12.0f);
    }

    void resized() override
    {
        if (panel.isVisible()) positionPanel();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = nodeAt (e.position);
        if (hit < 0) { selectNode (-1); return; } // click empty space -> close editor
        selectNode (hit);                          // selecting a node opens its editor
        dragging = hit;
        beginGesture (hit);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const float f = xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x));
        setParam (pid (dragging, "freq"), f);
        if (! isCut (dragging)) // bands carry a sensitivity (Y); cuts sit on the 0 line
            setParam (pid (dragging, "sens"),
                      yToSens (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) endGesture (dragging);
        dragging = -1;
    }

private:
    void timerCallback() override { repaint(); }

    static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;    // analyser axis
    static constexpr float kSensMin = -30.0f, kSensMax = 30.0f; // profile (sens) axis
    static constexpr int   kNumNodes = 2 + ResonanceSuppressorAudioProcessor::kNumBands; // LC, HC, bands

    juce::Rectangle<float> plotRect() const
    {
        auto r = getLocalBounds().toFloat().reduced (12.0f);
        r.removeFromTop (14.0f); // Hz labels sit above the plot
        return r;
    }

    // Node id: 0 = low cut, 1 = high cut, 2.. = band 0..
    static bool isCut (int id) { return id < 2; }
    static juce::String pid (int id, const char* s)
    {
        return isCut (id) ? ResonanceSuppressorAudioProcessor::cutPid (id, s)
                          : ResonanceSuppressorAudioProcessor::bandPid (id - 2, s);
    }

    // Non-uniform frequency axis: 500 Hz–9 kHz gets the middle 55% of the width
    // (uniform log), 20–500 Hz the left 30% and 9–20 kHz the right 15%, both
    // log-compressed — so the low end is gentle, the top is tight and 2 kHz lands
    // just right of centre (where 1.5 kHz used to sit).
    static float freqToT (float f)
    {
        f = juce::jlimit (20.0f, 20000.0f, f);
        const float lf = std::log (f);
        auto seg = [lf] (float f0, float f1, float t0, float t1)
        { return t0 + (t1 - t0) * (lf - std::log (f0)) / (std::log (f1) - std::log (f0)); };
        if (f <= 500.0f)  return seg (20.0f,   500.0f,   0.0f,  0.30f);
        if (f <= 9000.0f) return seg (500.0f,  9000.0f,  0.30f, 0.85f);
        return                   seg (9000.0f, 20000.0f, 0.85f, 1.0f);
    }
    static float tToFreq (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        auto seg = [t] (float t0, float t1, float f0, float f1)
        { return std::exp (std::log (f0) + (std::log (f1) - std::log (f0)) * (t - t0) / (t1 - t0)); };
        if (t <= 0.30f) return seg (0.0f,  0.30f, 20.0f,   500.0f);
        if (t <= 0.85f) return seg (0.30f, 0.85f, 500.0f,  9000.0f);
        return                seg (0.85f, 1.0f,  9000.0f, 20000.0f);
    }
    float freqToX (float f) const { return plot.getX() + freqToT (f) * plot.getWidth(); }
    float xToFreq (float x) const { return tToFreq ((x - plot.getX()) / plot.getWidth()); }
    float dbToY (float db) const
    {
        return plot.getY() + (kMaxDb - juce::jlimit (kMinDb, kMaxDb, db)) / (kMaxDb - kMinDb) * plot.getHeight();
    }
    float sensToY (float s) const
    {
        return plot.getBottom() - (juce::jlimit (kSensMin, kSensMax, s) - kSensMin) / (kSensMax - kSensMin) * plot.getHeight();
    }
    float yToSens (float y) const
    {
        return kSensMin + (plot.getBottom() - y) / plot.getHeight() * (kSensMax - kSensMin);
    }

    bool  nodeOn   (int id) const { return apvts.getRawParameterValue (pid (id, "on"))->load() > 0.5f; }
    float nodeFreq (int id) const { return apvts.getRawParameterValue (pid (id, "freq"))->load(); }
    float nodeSens (int id) const { return isCut (id) ? 0.0f : apvts.getRawParameterValue (pid (id, "sens"))->load(); }

    juce::Point<float> nodePos (int id) const { return { freqToX (nodeFreq (id)), sensToY (nodeSens (id)) }; }

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
            g.drawText (fl.s, juce::Rectangle<float> (x - 18.0f, plot.getY() - 13.0f, 36.0f, 12.0f),
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

    // A ReductionNodes config with only node `id` enabled (its own contribution).
    static factory_core::ReductionNodes singleNode (const factory_core::ReductionNodes& all, int id)
    {
        factory_core::ReductionNodes one; // all off by default
        if      (id == 0) one.lowCut  = all.lowCut;
        else if (id == 1) one.highCut = all.highCut;
        else              one.bands[(size_t) (id - 2)] = all.bands[(size_t) (id - 2)];
        return one;
    }

    // The reduction-profile curve — the SAME shape functions the audio rasteriser
    // uses (factory_core::reductionProfileDbAt), so what you see is what runs.
    // Modern-EQ styling (like the dynamic EQ): each active node draws its own
    // translucent filled curve, over which the combined response is a bright
    // stroke with a soft glow.
    void drawProfile (juce::Graphics& g)
    {
        const auto nodes = ResonanceSuppressorAudioProcessor::readNodes (apvts);
        const int steps = juce::jmax (2, (int) plot.getWidth());
        const float y0 = sensToY (0.0f); // nominal (0 dB) line

        std::array<factory_core::ReductionNodes, kNumNodes> single;
        std::array<juce::Path, kNumNodes> nodePath;
        std::array<bool, kNumNodes> started {};
        for (int id = 0; id < kNumNodes; ++id) single[(size_t) id] = singleNode (nodes, id);

        juce::Path combined;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot.getX() + (float) i * plot.getWidth() / steps;
            const float f = xToFreq (x);
            const float yT = sensToY ((float) factory_core::reductionProfileDbAt (f, nodes));
            if (i == 0) combined.startNewSubPath (x, yT);
            else        combined.lineTo (x, yT);

            for (int id = 0; id < kNumNodes; ++id)
            {
                if (! nodeOn (id)) continue;
                const float yB = sensToY ((float) factory_core::reductionProfileDbAt (f, single[(size_t) id]));
                if (! started[(size_t) id]) { nodePath[(size_t) id].startNewSubPath (x, yB); started[(size_t) id] = true; }
                else nodePath[(size_t) id].lineTo (x, yB);
            }
        }

        // Per-node translucent fill (from the nominal line) + coloured stroke.
        for (int id = 0; id < kNumNodes; ++id)
        {
            if (! started[(size_t) id]) continue;
            const auto col = isCut (id) ? FactoryLookAndFeel::textDim() : FactoryLookAndFeel::bandColour (id - 2);
            auto fp = nodePath[(size_t) id];
            fp.lineTo (plot.getRight(), y0);
            fp.lineTo (plot.getX(), y0);
            fp.closeSubPath();
            g.setColour (col.withAlpha (0.12f));
            g.fillPath (fp);
            g.setColour (col.withAlpha (0.7f));
            g.strokePath (nodePath[(size_t) id], juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        }

        // Combined response: soft coral glow under a crisp coral stroke.
        g.setColour (FactoryLookAndFeel::accent().withAlpha (0.22f));
        g.strokePath (combined, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (FactoryLookAndFeel::accent());
        g.strokePath (combined, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawNodes (juce::Graphics& g)
    {
        for (int id = 0; id < kNumNodes; ++id)
        {
            // Disabled nodes stay visible (dimmed), never hidden, so they can
            // always be grabbed / re-enabled — a hidden node is unreachable.
            const float a = nodeOn (id) ? 1.0f : 0.3f;
            const auto p = nodePos (id);
            const auto col = isCut (id) ? FactoryLookAndFeel::textDim() : FactoryLookAndFeel::bandColour (id - 2);
            g.setColour (juce::Colours::white.withAlpha (a));
            g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (p));
            g.setColour (col.withAlpha (a));
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p));
            if (id == selectedNode) // ring the node whose editor is open
            {
                g.setColour (FactoryLookAndFeel::text().withAlpha (0.9f));
                g.drawEllipse (juce::Rectangle<float> (21.0f, 21.0f).withCentre (p), 1.6f);
            }
            g.setColour (juce::Colours::white.withAlpha (a));
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            const juce::String label = isCut (id) ? (id == 0 ? "LC" : "HC") : juce::String (id - 1);
            g.drawText (label, juce::Rectangle<float> (16.0f, 16.0f).withCentre (p), juce::Justification::centred);
        }
    }

    int nodeAt (juce::Point<float> pos) const
    {
        int best = -1; float bestD = 14.0f;
        for (int id = 0; id < kNumNodes; ++id) // includes disabled nodes (they stay grabbable)
        {
            const float d = nodePos (id).getDistanceFrom (pos);
            if (d <= bestD) { bestD = d; best = id; }
        }
        return best;
    }

    // Selecting a node opens the inline editor bound to it; -1 closes it.
    void selectNode (int id)
    {
        selectedNode = id;
        if (id >= 0) { panel.setNode (id); panel.setVisible (true); positionPanel(); }
        else         panel.setVisible (false);
        repaint();
    }

    // Park the editor at the analyser's bottom-centre, just above the Hz labels.
    void positionPanel()
    {
        const auto pr = plotRect();
        const int w = panel.preferredWidth();
        const int h = NodePanel::kHeight;
        int x = juce::roundToInt (pr.getCentreX() - w * 0.5f);
        x = juce::jlimit ((int) pr.getX(), juce::jmax ((int) pr.getX(), (int) pr.getRight() - w), x);
        const int y = juce::roundToInt (pr.getBottom()) - h - 6;
        panel.setBounds (x, y, w, h);
    }

    void setParam (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (value));
    }
    void beginGesture (int id)
    {
        if (auto* p = apvts.getParameter (pid (id, "freq"))) p->beginChangeGesture();
        if (! isCut (id)) if (auto* p = apvts.getParameter (pid (id, "sens"))) p->beginChangeGesture();
    }
    void endGesture (int id)
    {
        if (auto* p = apvts.getParameter (pid (id, "freq"))) p->endChangeGesture();
        if (! isCut (id)) if (auto* p = apvts.getParameter (pid (id, "sens"))) p->endChangeGesture();
    }

    inline static const juce::Colour kTeal { juce::Colour (0xff45b8acu) };

    ResonanceSuppressorAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    NodePanel panel;
    juce::Rectangle<float> plot;
    int dragging = -1;
    int selectedNode = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuppressionCurveComponent)
};
