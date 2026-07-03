#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"

#include <array>
#include <memory>
#include <vector>

//
// The centrepiece: three overlaid spectra — pre (thin dim), post (line) and the
// delta / added-harmonics bus (coral fill) — behind five band zones divided by
// four draggable crossover handles. The display FFT order tracks the sample rate
// (processor.displayFftOrder), so the analyser resolution stays constant in Hz.
// GUI-thread only.
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
        drawSpectrum (g, MultibandEnhancerAudioProcessor::RingPre,   FactoryLookAndFeel::textDim(), false, 1.0f);
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
        if (o != fftOrder) rebuild (o);
        repaint();
    }

    void rebuild (int order)
    {
        fftOrder = order;
        N = 1 << order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        window.resize ((size_t) N);
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            window.data(), (size_t) N, juce::dsp::WindowingFunction<float>::hann, false);
        scratch.assign ((size_t) (N * 2), 0.0f);
        for (int s = 0; s < 3; ++s) { smooth[s].assign ((size_t) (N / 2), -120.0f); peak[s].assign ((size_t) (N / 2), -120.0f); }
    }

    // ---- mappings ----
    float freqToX (float f) const
    {
        const float t = std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / std::log (1000.0f);
        return plot.getX() + t * plot.getWidth();
    }
    float xToFreq (float x) const
    {
        const float t = (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth());
        return 20.0f * std::pow (1000.0f, t);
    }
    float dbToY (float db) const
    {
        return juce::jmap (juce::jlimit (-96.0f, 0.0f, db), -96.0f, 0.0f, plot.getBottom(), plot.getY());
    }

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
        if (fft == nullptr) return;
        processor.copyRing (scratch.data(), N, source);
        for (int i = 0; i < N; ++i) scratch[(size_t) i] *= window[(size_t) i];
        for (int i = N; i < N * 2; ++i) scratch[(size_t) i] = 0.0f;
        fft->performFrequencyOnlyForwardTransform (scratch.data());

        const double sr = processor.getSampleRateForDisplay();
        auto& sm = smooth[source];
        auto& pk = peak[source];

        juce::Path path;
        bool started = false;
        for (int bin = 1; bin < N / 2; ++bin)
        {
            const float freq = (float) (bin * sr / N);
            if (freq < 20.0f || freq > 20000.0f) continue;
            const float mag  = scratch[(size_t) bin] / (float) (N * 0.5);
            const float inst = juce::Decibels::gainToDecibels (mag, -120.0f);
            float& s = sm[(size_t) bin];
            s = (inst > s) ? inst : s + (inst - s) * 0.25f;
            float& p = pk[(size_t) bin];
            p = (s > p) ? s : juce::jmax (s, p - 0.6f);

            const float x = freqToX (freq);
            const float y = dbToY (s);
            if (! started) { if (filled) { path.startNewSubPath (x, plot.getBottom()); path.lineTo (x, y); } else path.startNewSubPath (x, y); started = true; }
            else path.lineTo (x, y);
        }
        if (! started) return;

        if (filled)
        {
            path.lineTo (plot.getRight(), plot.getBottom());
            path.closeSubPath();
            juce::ColourGradient grad (colour.withAlpha (0.30f), 0.0f, plot.getY(),
                                       colour.withAlpha (0.02f), 0.0f, plot.getBottom(), false);
            g.setGradientFill (grad);
            g.fillPath (path);
            g.setColour (colour.withAlpha (0.7f));
            g.strokePath (path, juce::PathStrokeType (lineW));
        }
        else
        {
            g.setColour (colour.withAlpha (0.8f));
            g.strokePath (path, juce::PathStrokeType (lineW));
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

    MultibandEnhancerAudioProcessor& processor;
    juce::Rectangle<float> plot;

    int fftOrder = 13, N = 8192;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, scratch;
    std::array<std::vector<float>, 3> smooth, peak;

    int dragHandle = -1, hoverHandle = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerComponent)
};
