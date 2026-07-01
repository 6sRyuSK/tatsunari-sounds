#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <cmath>

//
// The centrepiece: a spectrum display showing the output (post-processing)
// magnitude, the live per-frequency gain reduction (teal "curtain" — what the
// suppressor is cutting right now), and the soothe-style reduction-profile
// curve with fixed nodes: a low cut and a high cut (drag frequency; right-click
// for slope or on/off) plus four typed bands (drag frequency/sensitivity;
// right-click for type or on/off). Disabled nodes stay dimmed (never hidden) so
// they can always be re-enabled. Edits go through the APVTS. GUI-thread only.
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
        if (hit < 0) return;

        if (e.mods.isPopupMenu()) { showNodeMenu (hit); return; }

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

    // Node id: 0 = low cut, 1 = high cut, 2.. = band 0..
    static bool isCut (int id) { return id < 2; }
    static juce::String pid (int id, const char* s)
    {
        return isCut (id) ? ResonanceSuppressorAudioProcessor::cutPid (id, s)
                          : ResonanceSuppressorAudioProcessor::bandPid (id - 2, s);
    }

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
            g.drawText (fl.s, juce::Rectangle<float> (x - 18.0f, plot.getBottom() + 1.0f, 36.0f, 12.0f),
                        juce::Justification::centred);
        }
        for (float db = 0.0f; db >= -60.0f; db -= 12.0f)
        {
            const float y = dbToY (db);
            g.setColour (FactoryLookAndFeel::track().withAlpha (db == 0.0f ? 0.9f : 0.4f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
        // Nominal (0 dB sensitivity) reference for the profile curve.
        g.setColour (FactoryLookAndFeel::track().withAlpha (0.35f));
        g.drawHorizontalLine ((int) sensToY (0.0f), plot.getX(), plot.getRight());
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

        // Combined response: soft glow under a crisp stroke.
        g.setColour (FactoryLookAndFeel::text().withAlpha (0.16f));
        g.strokePath (combined, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (FactoryLookAndFeel::text().withAlpha (0.9f));
        g.strokePath (combined, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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

    // Right-click: On/Off + (cut) slope or (band) type. Radio-ticks the current.
    void showNodeMenu (int id)
    {
        static const juce::StringArray slopes { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
        static const juce::StringArray types  { "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" };
        const auto& items = isCut (id) ? slopes : types;
        const char* choiceSuffix = isCut (id) ? "slope" : "type";
        const int current = (int) apvts.getRawParameterValue (pid (id, choiceSuffix))->load();

        juce::PopupMenu m;
        m.addItem (1, "On", true, nodeOn (id));
        m.addSeparator();
        for (int i = 0; i < items.size(); ++i)
            m.addItem (100 + i, items[i], true, i == current);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [this, id, choiceSuffix] (int result)
                         {
                             if (result == 1) setParam (pid (id, "on"), nodeOn (id) ? 0.0f : 1.0f);
                             else if (result >= 100) setParam (pid (id, choiceSuffix), (float) (result - 100));
                             repaint();
                         });
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
    juce::Rectangle<float> plot;
    int dragging = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuppressionCurveComponent)
};
