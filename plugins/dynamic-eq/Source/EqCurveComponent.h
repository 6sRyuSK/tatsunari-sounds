#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"
#include "factory_core/Biquad.h"
#include "factory_core/Filters.h"

#include <array>
#include <complex>

//
// The centrepiece: a spectrum analyzer behind the combined EQ response curve,
// with one draggable, per-band-coloured node (x = frequency, y = gain).
// Modern-EQ styling (Pro-Q 4 / Kirchhoff / ZL): each band draws its own pastel
// filled curve, the analyzer is a smoothed gradient trace with peak-hold, and
// dynamic bands "breathe" in real time by following the processor's published
// live gain. Clicking a node selects that band; dragging edits freq/gain; the
// mouse wheel over a node edits Q. GUI-thread only.
//
class EqCurveComponent : public juce::Component,
                         private juce::Timer
{
public:
    std::function<void (int)> onBandSelected;

    EqCurveComponent (DynamicEqAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s)
    {
        smoothDb.fill (-120.0f);
        peakDb.fill (-120.0f);
        startTimerHz (30);
    }

    int getSelectedBand() const noexcept { return selectedBand; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();

        // Card background with a gentle top-light gradient.
        juce::ColourGradient bgGrad (FactoryLookAndFeel::panel(), r.getCentreX(), r.getY(),
                                     FactoryLookAndFeel::panelLo(), r.getCentreX(), r.getBottom(), false);
        g.setGradientFill (bgGrad);
        g.fillRoundedRectangle (r, 10.0f);

        plot = r.reduced (12.0f);
        plot.removeFromBottom (14.0f); // room for Hz labels

        drawGrid (g);
        drawAnalyzer (g);
        drawBandsAndResponse (g);
        drawNodes (g);

        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
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

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = nodeAt (e.position);
        if (h != hoveredBand) { hoveredBand = h; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredBand != -1) { hoveredBand = -1; repaint(); }
    }

    // Double-click an empty spot to add a band (type follows the horizontal
    // position: HP | LowShelf | Bell | HighShelf | LP); double-click a node to
    // remove that band.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int hit = nodeAt (e.position);
        if (hit >= 0)
        {
            setParam (hit, "on", 0.0f); // remove
            hoveredBand = -1;
            repaint();
            return;
        }

        const int slot = firstFreeBand();
        if (slot < 0) return; // all bands in use

        const float x = juce::jlimit (plot.getX(), plot.getRight(), e.position.x);
        const float frac = (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth());
        const int type = typeForFraction (frac);

        setParam (slot, "byp", 0.0f); // a re-used slot may have been left bypassed
        setParam (slot, "type", (float) type);
        setParam (slot, "freq", xToFreq (x));
        setParam (slot, "q", 0.707f);
        if (type != (int) factory_core::BandType::HighPass
            && type != (int) factory_core::BandType::LowPass)
        {
            const float g = yToGain (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y));
            setParam (slot, "gain", juce::jlimit (-kMaxGain, kMaxGain, g));
        }
        setParam (slot, "on", 1.0f); // activate last

        selectedBand = slot;
        if (onBandSelected) onBandSelected (slot);
        repaint();
    }

    // Wheel over a node adjusts its Q (Pro-Q style).
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int band = (hoveredBand >= 0) ? hoveredBand
                        : (nodeAt (e.position) >= 0 ? nodeAt (e.position) : -1);
        if (band < 0) return;
        const float q = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "q"))->load();
        const float nq = juce::jlimit (0.1f, 18.0f, q * std::exp (w.deltaY * 1.2f));
        setParam (band, "q", nq);
        repaint();
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

    int bandTypeInt (int band) const
    {
        return (int) apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "type"))->load();
    }
    bool isCutType (int band) const
    {
        const int t = bandTypeInt (band);
        return t == (int) factory_core::BandType::HighPass || t == (int) factory_core::BandType::LowPass;
    }
    // Number of Butterworth sections for an HP/LP band (1 otherwise).
    int bandStages (int band) const
    {
        if (! isCutType (band)) return 1;
        const int slope = (int) apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "slope"))->load();
        return juce::jlimit (1, factory_core::DynamicEqBand::kMaxStages, slope + 1);
    }
    bool bandOn (int band) const
    {
        return apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "on"))->load() > 0.5f;
    }
    bool bandDynamic (int band) const
    {
        return apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "dyn"))->load() > 0.5f;
    }
    bool bandBypassed (int band) const
    {
        return apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "byp"))->load() > 0.5f;
    }
    // Effective display gain: active dynamic bands follow the live (post-dynamics)
    // gain so the curve/node breathe; static or bypassed bands use the param.
    float bandGainDb (int band) const
    {
        if (bandDynamic (band) && ! bandBypassed (band)) return processor.getLiveGainDb (band);
        return apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "gain"))->load();
    }

    // ---- grid + labels ----
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
        for (float gdb = -kMaxGain + 6.0f; gdb < kMaxGain; gdb += 6.0f)
        {
            const float y = gainToY (gdb);
            const bool zero = juce::approximatelyEqual (gdb, 0.0f);
            g.setColour (FactoryLookAndFeel::track().withAlpha (zero ? 0.95f : 0.45f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            if (zero || juce::approximatelyEqual (std::fmod (gdb, 12.0f), 0.0f))
            {
                g.setColour (FactoryLookAndFeel::textDim());
                g.drawText ((gdb > 0 ? "+" : "") + juce::String ((int) gdb),
                            juce::Rectangle<float> (plot.getX() + 2.0f, y - 11.0f, 30.0f, 11.0f),
                            juce::Justification::topLeft);
            }
        }
    }

    // ---- spectrum analyzer (smoothed, gradient fill, peak-hold, tilted) ----
    void drawAnalyzer (juce::Graphics& g)
    {
        std::array<float, kFftSize * 2> fftData {};
        processor.copyAnalyzerSamples (fftData.data(), kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            fftData[(size_t) i] *= window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const double sr = processor.getSampleRateForDisplay();
        constexpr float tiltPerOct = 3.0f; // flatten pink-ish spectra for display

        auto tiltedDb = [tiltPerOct] (float db, float freq)
        {
            return db + tiltPerOct * std::log2 (juce::jmax (1.0f, freq) / 1000.0f);
        };

        juce::Path fill, peak;
        bool startedFill = false, startedPeak = false;
        for (int bin = 1; bin < kFftSize / 2; ++bin)
        {
            const float freq = (float) (bin * sr / kFftSize);
            if (freq < 20.0f || freq > 20000.0f) continue;

            const float mag  = fftData[(size_t) bin] / (kFftSize * 0.5f);
            const float inst = tiltedDb (juce::Decibels::gainToDecibels (mag, -120.0f), freq);

            float& sm = smoothDb[(size_t) bin];
            sm = (inst > sm) ? inst : sm + (inst - sm) * 0.25f; // fast up, slow down
            float& pk = peakDb[(size_t) bin];
            pk = (sm > pk) ? sm : juce::jmax (sm, pk - 0.6f);    // hold then fall

            const float x  = freqToX (freq);
            const float ys = juce::jmap (juce::jlimit (-100.0f, 0.0f, sm), -100.0f, 0.0f,
                                         plot.getBottom(), plot.getY());
            const float yp = juce::jmap (juce::jlimit (-100.0f, 0.0f, pk), -100.0f, 0.0f,
                                         plot.getBottom(), plot.getY());

            if (! startedFill) { fill.startNewSubPath (x, plot.getBottom()); fill.lineTo (x, ys); startedFill = true; }
            else fill.lineTo (x, ys);

            if (! startedPeak) { peak.startNewSubPath (x, yp); startedPeak = true; }
            else peak.lineTo (x, yp);
        }

        if (startedFill)
        {
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.closeSubPath();
            juce::ColourGradient grad (FactoryLookAndFeel::accent().withAlpha (0.30f), 0.0f, plot.getY(),
                                       FactoryLookAndFeel::accent().withAlpha (0.02f), 0.0f, plot.getBottom(), false);
            g.setGradientFill (grad);
            g.fillPath (fill);
        }
        if (startedPeak)
        {
            g.setColour (FactoryLookAndFeel::textDim().withAlpha (0.5f));
            g.strokePath (peak, juce::PathStrokeType (1.0f));
        }
    }

    static std::complex<double> evalH (const factory_core::BiquadCoeffs& c,
                                       std::complex<double> z1, std::complex<double> z2)
    {
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    // ---- per-band fills + combined response (with glow) ----
    void drawBandsAndResponse (juce::Graphics& g)
    {
        const double sr = processor.getSampleRateForDisplay();

        // Precompute each band's coefficients once for this frame. HP/LP bands
        // are a cascade of `stages` Butterworth sections (12..96 dB/oct).
        constexpr int kMaxStages = factory_core::DynamicEqBand::kMaxStages;
        std::array<std::array<factory_core::BiquadCoeffs, kMaxStages>,
                   DynamicEqAudioProcessor::kNumBands> coeffs;
        std::array<int, DynamicEqAudioProcessor::kNumBands> stages {};
        std::array<bool, DynamicEqAudioProcessor::kNumBands> on {};
        std::array<bool, DynamicEqAudioProcessor::kNumBands> byp {};
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
        {
            on[(size_t) b] = bandOn (b);
            byp[(size_t) b] = bandBypassed (b);
            if (! on[(size_t) b]) continue;
            const auto type = static_cast<factory_core::BandType> (bandTypeInt (b));
            const double f = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (b, "freq"))->load();
            const double q = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (b, "q"))->load();
            if (isCutType (b))
            {
                const int n = bandStages (b);
                stages[(size_t) b] = n;
                for (int s = 0; s < n; ++s)
                    coeffs[(size_t) b][(size_t) s] = factory_core::designHpLpStage (type, f, q, s, n, sr);
            }
            else
            {
                stages[(size_t) b] = 1;
                coeffs[(size_t) b][0] = factory_core::designFilter (type, f, bandGainDb (b), q, sr);
            }
        }

        const float y0 = gainToY (0.0f);
        const int steps = juce::jmax (2, (int) plot.getWidth());
        std::array<juce::Path, DynamicEqAudioProcessor::kNumBands> bandFill;
        std::array<bool, DynamicEqAudioProcessor::kNumBands> started {};
        juce::Path total;

        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot.getX() + (float) i * plot.getWidth() / steps;
            const float freq = xToFreq (x);
            const double wd = 2.0 * juce::MathConstants<double>::pi * freq / sr;
            const std::complex<double> z1 = std::exp (std::complex<double> (0.0, -wd));
            const std::complex<double> z2 = std::exp (std::complex<double> (0.0, -2.0 * wd));

            std::complex<double> h (1.0, 0.0);
            for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
            {
                if (! on[(size_t) b]) continue;
                std::complex<double> hb (1.0, 0.0);
                for (int s = 0; s < stages[(size_t) b]; ++s)
                    hb *= evalH (coeffs[(size_t) b][(size_t) s], z1, z2);
                if (! byp[(size_t) b]) h *= hb; // bypassed bands are not in the combined response

                const float dbB = (float) juce::Decibels::gainToDecibels (std::abs (hb), -120.0);
                const float yB = gainToY (juce::jlimit (-kMaxGain, kMaxGain, dbB));
                auto& fp = bandFill[(size_t) b];
                if (! started[(size_t) b]) { fp.startNewSubPath (x, y0); fp.lineTo (x, yB); started[(size_t) b] = true; }
                else fp.lineTo (x, yB);
            }

            const float dbT = (float) juce::Decibels::gainToDecibels (std::abs (h), -120.0);
            const float yT = gainToY (juce::jlimit (-kMaxGain, kMaxGain, dbT));
            if (i == 0) total.startNewSubPath (x, yT);
            else        total.lineTo (x, yT);
        }

        // Per-band translucent fills.
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
        {
            if (! started[(size_t) b]) continue;
            const bool active = (b == selectedBand || b == hoveredBand);

            if (byp[(size_t) b])
            {
                // Bypassed: no fill, a grey dashed outline so the band reads as
                // present-but-inactive.
                juce::Path dashed;
                const float dl[] = { 4.0f, 3.0f };
                juce::PathStrokeType (active ? 1.4f : 1.0f).createDashedStroke (dashed, bandFill[(size_t) b], dl, 2);
                g.setColour (FactoryLookAndFeel::textDim().withAlpha (active ? 0.85f : 0.55f));
                g.fillPath (dashed);
                continue;
            }

            auto fp = bandFill[(size_t) b];
            fp.lineTo (plot.getRight(), y0);
            fp.closeSubPath();
            const auto col = FactoryLookAndFeel::bandColour (b);
            g.setColour (col.withAlpha (active ? 0.26f : 0.14f));
            g.fillPath (fp);
            g.setColour (col.withAlpha (active ? 0.9f : 0.55f));
            g.strokePath (bandFill[(size_t) b], juce::PathStrokeType (active ? 1.6f : 1.0f));
        }

        // Combined response: soft glow under a crisp coral stroke.
        g.setColour (FactoryLookAndFeel::accent().withAlpha (0.18f));
        g.strokePath (total, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (FactoryLookAndFeel::accent());
        g.strokePath (total, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Point<float> nodePos (int band) const
    {
        const float f = apvts.getRawParameterValue (DynamicEqAudioProcessor::pid (band, "freq"))->load();
        const float gdb = isCutType (band) ? 0.0f : bandGainDb (band);
        return { freqToX (f), gainToY (juce::jlimit (-kMaxGain, kMaxGain, gdb)) };
    }

    void drawNodes (juce::Graphics& g)
    {
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
        {
            const bool on = bandOn (b);
            if (! on) continue; // only added (active) bands have a node
            const auto p = nodePos (b);
            const bool sel = (b == selectedBand);
            const bool hov = (b == hoveredBand);
            const float rad = sel ? 9.0f : (hov ? 8.0f : 7.0f);
            const bool byp = bandBypassed (b);
            const auto col = byp ? FactoryLookAndFeel::textDim() : FactoryLookAndFeel::bandColour (b);

            // Glow halo (active, non-bypassed).
            if (! byp && (sel || hov))
            {
                g.setColour (col.withAlpha (0.30f));
                g.fillEllipse (juce::Rectangle<float> (rad * 4.0f, rad * 4.0f).withCentre (p));
            }
            // White rim for a soft sticker look.
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (rad * 2.0f + 4.0f, rad * 2.0f + 4.0f).withCentre (p));
            g.setColour (col.withAlpha (byp ? 0.55f : 1.0f));
            g.fillEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (p));
            if (sel)
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (p), 2.0f);
            }
            // Bypassed bands get a grey dashed ring; dynamic bands a thin badge.
            if (byp)
            {
                juce::Path ring, dring;
                ring.addEllipse (juce::Rectangle<float> (rad * 2.0f + 6.0f, rad * 2.0f + 6.0f).withCentre (p));
                const float dl[] = { 3.0f, 2.5f };
                juce::PathStrokeType (1.3f).createDashedStroke (dring, ring, dl, 2);
                g.setColour (FactoryLookAndFeel::textDim());
                g.fillPath (dring);
            }
            else if (bandDynamic (b))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawEllipse (juce::Rectangle<float> (rad * 2.0f + 7.0f, rad * 2.0f + 7.0f).withCentre (p), 1.2f);
            }

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (p),
                        juce::Justification::centred);
        }
    }

    int nodeAt (juce::Point<float> pos) const
    {
        int best = -1;
        float bestD = 14.0f;
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
        {
            if (! bandOn (b)) continue; // only active bands are selectable
            const float d = nodePos (b).getDistanceFrom (pos);
            if (d <= bestD) { bestD = d; best = b; }
        }
        return best;
    }

    int firstFreeBand() const
    {
        for (int b = 0; b < DynamicEqAudioProcessor::kNumBands; ++b)
            if (! bandOn (b)) return b;
        return -1;
    }

    // Horizontal position -> band type (symmetric): HP | LowShelf | Bell | HighShelf | LP.
    int typeForFraction (float frac) const
    {
        if (frac < 0.12f) return (int) factory_core::BandType::HighPass;
        if (frac < 0.25f) return (int) factory_core::BandType::LowShelf;
        if (frac > 0.88f) return (int) factory_core::BandType::LowPass;
        if (frac > 0.75f) return (int) factory_core::BandType::HighShelf;
        return (int) factory_core::BandType::Bell;
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

    // Larger FFT so low-frequency bins stay fine even at high sample rates
    // (at 192 kHz a 2048-point FFT only resolves ~94 Hz/bin, blanking the
    // sub-100 Hz region; 8192 gives ~23 Hz/bin there).
    static constexpr int kFftOrder = 13;
    static constexpr int kFftSize = 1 << kFftOrder; // 8192
    static constexpr float kMaxGain = 24.0f;

    DynamicEqAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;
    int selectedBand = 0;
    int dragging = -1;
    int hoveredBand = -1;

    juce::dsp::FFT fft { kFftOrder };
    std::array<float, kFftSize / 2> smoothDb {};
    std::array<float, kFftSize / 2> peakDb {};
    std::array<float, kFftSize> window = [] {
        std::array<float, kFftSize> w {};
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            w.data(), kFftSize, juce::dsp::WindowingFunction<float>::hann, false);
        return w;
    }();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveComponent)
};
