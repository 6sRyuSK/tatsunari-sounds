#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/SpectrumAnalyzerModel.h"
#include "factory_ui/SpectrumDisplay.h"

#include <vector>

//
// The centrepiece: three overlaid spectra — pre / dry (thin pale steel blue), post
// (line) and the delta / added-harmonics bus (coral fill) — behind five band zones
// divided by four draggable crossover handles. The display FFT order tracks the
// sample rate (processor.displayFftOrder), so the analyser resolution stays
// constant in Hz. GUI-thread only.
//
// The dry/pre trace uses a pale steel blue (colours are caller parameters of the
// shared SpectrumDisplay API) so it reads as the untouched reference against the
// warm-white card without competing with the coral post/delta traces.
//
class AnalyzerComponent : public juce::Component,
                          private juce::Timer
{
public:
    AnalyzerComponent (MultibandEnhancerAudioProcessor& p)
        : processor (p)
    {
        rebuild (processor.displayFftOrder());
        startTimerHz (30);
    }

    ~AnalyzerComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bgGrad (FactoryLookAndFeel::panel(), r.getCentreX(), r.getY(),
                                     FactoryLookAndFeel::panelLo(), r.getCentreX(), r.getBottom(), false);
        g.setGradientFill (bgGrad);
        g.fillRoundedRectangle (r, 10.0f);

        plot = r.reduced (12.0f);
        plot.removeFromBottom (14.0f); // room for Hz labels

        drawBandZones (g);
        drawGrid (g);
        drawSpectrum (g, MultibandEnhancerAudioProcessor::RingPre,   kDryColour,                    false, 1.0f);
        drawSpectrum (g, MultibandEnhancerAudioProcessor::RingPost,  FactoryLookAndFeel::text(),    false, 1.4f);
        drawSpectrum (g, MultibandEnhancerAudioProcessor::RingDelta, FactoryLookAndFeel::accent(),  true,  1.6f);
        drawCrossovers (g);

        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragHandle = handleAt (e.position);
        if (dragHandle >= 0)
            if (auto* pp = param (dragHandle)) pp->beginChangeGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragHandle < 0) return;
        const float f = xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x));
        // Clamp to a 1/3-octave minimum spacing from the neighbours (matches the
        // engine's own clamp) so the handles cannot cross.
        float lo = 20.0f, hi = 20000.0f;
        if (dragHandle > 0) lo = xoverParam (dragHandle - 1) * 1.26f;
        if (dragHandle < 3) hi = xoverParam (dragHandle + 1) / 1.26f;
        const float clamped = juce::jlimit (lo, hi, f);
        if (auto* pp = param (dragHandle))
            pp->setValueNotifyingHost (pp->convertTo0to1 (clamped));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragHandle >= 0)
            if (auto* pp = param (dragHandle)) pp->endChangeGesture();
        dragHandle = -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = handleAt (e.position);
        if (h != hoverHandle) { hoverHandle = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

private:
    void timerCallback() override
    {
        const int o = processor.displayFftOrder();
        if (o != model.order()) rebuild (o);
        repaint();
    }

    void rebuild (int order)
    {
        model.setup (3, order);
        scratch.assign ((size_t) (model.size() * 2), 0.0f);
    }

    // ---- mappings (shared factory_ui axes: standard 20 Hz–20 kHz log x, −96..0 dB y) ----
    float freqToX (float f)  const { return factory_ui::LogFreqAxis { plot }.freqToX (f); }
    float xToFreq (float x)  const { return factory_ui::LogFreqAxis { plot }.xToFreq (x); }
    float dbToY   (float db) const { return factory_ui::VerticalAxis { plot, 0.0f, -96.0f }.toY (db); }

    float xoverParam (int i) const
    {
        return processor.apvts.getRawParameterValue (MultibandEnhancerAudioProcessor::xoverId (i))->load();
    }
    juce::RangedAudioParameter* param (int i) const
    {
        return processor.apvts.getParameter (MultibandEnhancerAudioProcessor::xoverId (i));
    }

    void drawBandZones (juce::Graphics& g)
    {
        float edges[6];
        edges[0] = plot.getX();
        for (int i = 0; i < 4; ++i) edges[i + 1] = freqToX ((float) processor.effectiveCrossoverHz (i));
        edges[5] = plot.getRight();
        for (int b = 0; b < 5; ++b)
        {
            const float x0 = juce::jlimit (plot.getX(), plot.getRight(), edges[b]);
            const float x1 = juce::jlimit (plot.getX(), plot.getRight(), edges[b + 1]);
            if (x1 <= x0) continue;
            g.setColour (FactoryLookAndFeel::bandColour (b).withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, plot.getHeight()));
        }
    }

    void drawGrid (juce::Graphics& g)
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        struct FL { float f; const char* s; };
        for (auto fl : { FL{50,"50"}, FL{100,"100"}, FL{200,"200"}, FL{500,"500"},
                         FL{1000,"1k"}, FL{2000,"2k"}, FL{5000,"5k"}, FL{10000,"10k"} })
        {
            const float x = freqToX (fl.f);
            g.setColour (FactoryLookAndFeel::track().withAlpha (0.6f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (FactoryLookAndFeel::textDim());
            g.drawText (fl.s, juce::Rectangle<float> (x - 18.0f, plot.getBottom() + 1.0f, 36.0f, 12.0f),
                        juce::Justification::centred);
        }
        for (float db = -24.0f; db > -96.0f; db -= 24.0f)
        {
            const float y = dbToY (db);
            g.setColour (FactoryLookAndFeel::track().withAlpha (0.4f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
    }

    void drawSpectrum (juce::Graphics& g, int source, juce::Colour colour, bool filled, float lineW)
    {
        const int N = model.size();
        if (N <= 0) return;
        processor.copyRing (scratch.data(), N, source);
        model.process (source, scratch.data(), processor.getSampleRateForDisplay());

        const double sr = processor.getSampleRateForDisplay();
        factory_ui::SpectrumTrace trace;
        trace.begin (plot.getBottom(), plot.getRight());
        for (int bin = 1; bin < N / 2; ++bin)
        {
            const float freq = factory_ui::SpectrumAnalyzerModel::binFrequency (bin, sr, N);
            if (freq < 20.0f || freq > 20000.0f) continue;
            trace.addPoint (freqToX (freq), dbToY (model.smoothedDb (source, bin)));
        }
        if (trace.isEmpty()) return;

        if (filled)
        {
            const auto area = trace.area();
            factory_ui::fillSpectrumArea (g, area, colour, plot, 0.30f, 0.02f);
            g.setColour (colour.withAlpha (0.7f));
            g.strokePath (area, juce::PathStrokeType (lineW));
        }
        else
        {
            g.setColour (colour.withAlpha (0.8f));
            g.strokePath (trace.line(), juce::PathStrokeType (lineW));
        }
    }

    void drawCrossovers (juce::Graphics& g)
    {
        for (int i = 0; i < 4; ++i)
        {
            const float f = xoverParam (i);
            const float x = freqToX (f);
            const bool active = (i == dragHandle || i == hoverHandle);
            g.setColour (FactoryLookAndFeel::accent().withAlpha (active ? 0.9f : 0.5f));
            g.drawLine (x, plot.getY(), x, plot.getBottom(), active ? 2.0f : 1.2f);

            // Handle grip + Hz label near the top.
            auto grip = juce::Rectangle<float> (10.0f, 16.0f).withCentre ({ x, plot.getY() + 8.0f });
            g.setColour (FactoryLookAndFeel::accent().withAlpha (active ? 1.0f : 0.75f));
            g.fillRoundedRectangle (grip, 3.0f);
            if (active)
            {
                g.setColour (FactoryLookAndFeel::text());
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                const juce::String label = f >= 1000.0f ? juce::String (f / 1000.0f, 2) + "k" : juce::String ((int) f);
                g.drawText (label + " Hz", juce::Rectangle<float> (x - 30.0f, plot.getY() + 18.0f, 60.0f, 12.0f),
                            juce::Justification::centred);
            }
        }
    }

    int handleAt (juce::Point<float> pos) const
    {
        int best = -1; float bestD = 7.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float d = std::abs (freqToX (xoverParam (i)) - pos.x);
            if (d <= bestD) { bestD = d; best = i; }
        }
        return best;
    }

    // Pale steel blue for the dry/pre spectrum — sits calmly on the warm-white
    // card and stays distinct from the coral post/delta traces.
    static constexpr juce::uint32 kDryColourARGB = 0xff7fa8c9;
    inline static const juce::Colour kDryColour { kDryColourARGB };

    MultibandEnhancerAudioProcessor& processor;
    juce::Rectangle<float> plot;

    // Three overlaid spectra (pre / post / delta) share one windowed FFT; the
    // display order tracks the sample rate via rebuild().
    factory_ui::SpectrumAnalyzerModel model;
    std::vector<float> scratch; // 2*N FFT work buffer, refilled from the ring each frame

    int dragHandle = -1, hoverHandle = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerComponent)
};
