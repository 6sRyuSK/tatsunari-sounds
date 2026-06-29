#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"
#include "factory_core/Filters.h"

#include <array>
#include <complex>

//
// The centrepiece: a spectrum analyzer behind the combined EQ response curve,
// with one draggable node per band (x = frequency, y = gain). Clicking a node
// selects that band; dragging edits it via the APVTS. GUI-thread only.
//
class EqCurveComponent : public juce::Component,
                         private juce::Timer
{
public:
    std::function<void (int)> onBandSelected;

    EqCurveComponent (DynamicEqAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s)
    {
        startTimerHz (30);
    }

    int getSelectedBand() const noexcept { return selectedBand; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 6.0f);

        plot = r.reduced (8.0f);
        drawGrid (g);
        drawAnalyzer (g);
        drawResponse (g);
        drawNodes (g);

        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = nodeAt (e.position);
        if (hit >= 0)
        {
            selectedBand = hit;
            dragging = hit;
            if (onBandSelected) onBandSelected (hit);
            beginGesture (hit);
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const float f = xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x));
        const float gdb = yToGain (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y));
        setParam (dragging, "freq", f);
        if (! isCutType (dragging))
            setParam (dragging, "gain", gdb);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) endGesture (dragging);
        dragging = -1;
    }

private:
    void timerCallback() override { repaint(); }

    // ---- mappings ----
    float freqToX (float f) const
    {
        const float t = std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / std::log (1000.0f);
        return plot.getX() + t * plot.getWidth();
    }
    float xToFreq (float x) const
    {
        const float t = (x - plot.getX()) / plot.getWidth();
        return 20.0f * std::pow (1000.0f, t);
    }
    float gainToY (float gdb) const
    {
        return plot.getY() + ((kMaxGain - gdb) / (2.0f * kMaxGain)) * plot.getHeight();
    }
    float yToGain (float y) const
    {
        return kMaxGain - ((y - plot.getY()) / plot.getHeight()) * (2.0f * kMaxGain);
    }

    bool isCutType (int band) const
    {
        const int t = (int) apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "type"))->load();
        return t == (int) factory_core::BandType::HighPass || t == (int) factory_core::BandType::LowPass;
    }

    void drawGrid (juce::Graphics& g)
    {
        g.setColour (FactoryLookAndFeel::track().withAlpha (0.6f));
        for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
            g.drawVerticalLine ((int) freqToX (f), plot.getY(), plot.getBottom());
        for (float gdb = -kMaxGain + 6.0f; gdb < kMaxGain; gdb += 6.0f)
        {
            const float y = gainToY (gdb);
            g.setColour (FactoryLookAndFeel::track().withAlpha (gdb == 0.0f ? 0.9f : 0.4f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
    }

    void drawAnalyzer (juce::Graphics& g)
    {
        std::array<float, kFftSize * 2> fftData {};
        processor.copyAnalyzerSamples (fftData.data(), kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            fftData[(size_t) i] *= window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const double sr = processor.getSampleRateForDisplay();
        juce::Path path;
        bool started = false;
        for (int bin = 1; bin < kFftSize / 2; ++bin)
        {
            const float freq = (float) (bin * sr / kFftSize);
            if (freq < 20.0f || freq > 20000.0f) continue;
            const float mag = fftData[(size_t) bin] / (kFftSize * 0.5f);
            const float db = juce::Decibels::gainToDecibels (mag, -120.0f);
            const float x = freqToX (freq);
            const float y = juce::jmap (juce::jlimit (-100.0f, 0.0f, db), -100.0f, 0.0f,
                                        plot.getBottom(), plot.getY());
            if (! started) { path.startNewSubPath (x, plot.getBottom()); path.lineTo (x, y); started = true; }
            else path.lineTo (x, y);
        }
        if (started)
        {
            path.lineTo (plot.getRight(), plot.getBottom());
            path.closeSubPath();
            g.setColour (FactoryLookAndFeel::accent().withAlpha (0.18f));
            g.fillPath (path);
        }
    }

    std::complex<double> bandH (int band, double w) const
    {
        if (apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "on"))->load() < 0.5f)
            return { 1.0, 0.0 };
        const auto type = static_cast<factory_core::BandType> (
            (int) apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "type"))->load());
        const double f = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "freq"))->load();
        const double gdb = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "gain"))->load();
        const double q = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "q"))->load();
        const auto c = factory_core::designFilter (type, f, gdb, q, processor.getSampleRateForDisplay());
        const std::complex<double> z1 = std::exp (std::complex<double> (0.0, -w));
        const std::complex<double> z2 = std::exp (std::complex<double> (0.0, -2.0 * w));
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    void drawResponse (juce::Graphics& g)
    {
        const double sr = processor.getSampleRateForDisplay();
        juce::Path curve;
        const int steps = (int) plot.getWidth();
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot.getX() + (float) i;
            const float freq = xToFreq (x);
            const double w = 2.0 * juce::MathConstants<double>::pi * freq / sr;
            std::complex<double> h (1.0, 0.0);
            for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
                h *= bandH (b, w);
            const float db = (float) juce::Decibels::gainToDecibels (std::abs (h), -120.0);
            const float y = gainToY (juce::jlimit (-kMaxGain, kMaxGain, db));
            if (i == 0) curve.startNewSubPath (x, y);
            else        curve.lineTo (x, y);
        }
        g.setColour (FactoryLookAndFeel::accent());
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Point<float> nodePos (int band) const
    {
        const float f = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "freq"))->load();
        const float gdb = isCutType (band) ? 0.0f
                          : apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "gain"))->load();
        return { freqToX (f), gainToY (gdb) };
    }

    void drawNodes (juce::Graphics& g)
    {
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
        {
            const bool on = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (b, "on"))->load() > 0.5f;
            const auto p = nodePos (b);
            const float rad = (b == selectedBand) ? 8.0f : 6.0f;
            const auto col = on ? FactoryLookAndFeel::accent() : FactoryLookAndFeel::textDim();
            g.setColour (col.withAlpha (on ? 0.9f : 0.5f));
            g.fillEllipse (juce::Rectangle<float> (rad * 2, rad * 2).withCentre (p));
            if (b == selectedBand)
            {
                g.setColour (FactoryLookAndFeel::text());
                g.drawEllipse (juce::Rectangle<float> (rad * 2 + 4, rad * 2 + 4).withCentre (p), 1.5f);
            }
            g.setColour (FactoryLookAndFeel::background());
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (rad * 2, rad * 2).withCentre (p),
                        juce::Justification::centred);
        }
    }

    int nodeAt (juce::Point<float> pos) const
    {
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
            if (nodePos (b).getDistanceFrom (pos) <= 12.0f)
                return b;
        return -1;
    }

    void setParam (int band, const char* suffix, float value)
    {
        if (auto* p = apvts.getParameter (DynamicEqAudioProcessor::pid (band, suffix)))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    }
    void beginGesture (int band)
    {
        if (auto* p = apvts.getParameter (DynamicEqAudioProcessor::pid (band, "freq"))) p->beginChangeGesture();
        if (auto* p = apvts.getParameter (DynamicEqAudioProcessor::pid (band, "gain"))) p->beginChangeGesture();
    }
    void endGesture (int band)
    {
        if (auto* p = apvts.getParameter (DynamicEqAudioProcessor::pid (band, "freq"))) p->endChangeGesture();
        if (auto* p = apvts.getParameter (DynamicEqAudioProcessor::pid (band, "gain"))) p->endChangeGesture();
    }

    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize = 1 << kFftOrder; // 2048
    static constexpr float kMaxGain = 24.0f;

    DynamicEqAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;
    int selectedBand = 0;
    int dragging = -1;

    juce::dsp::FFT fft { kFftOrder };
    std::array<float, kFftSize> window = [] {
        std::array<float, kFftSize> w {};
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            w.data(), kFftSize, juce::dsp::WindowingFunction<float>::hann, false);
        return w;
    }();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveComponent)
};
