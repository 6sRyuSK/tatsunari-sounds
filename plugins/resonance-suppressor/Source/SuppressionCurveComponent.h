#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "NodePanel.h"
#include "RsTheme.h"
#include "AnalyzerStyle.h"
#include "AnalyzerDevPanel.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"
#include "factory_ui/SpectrumDisplay.h"

#include <array>
#include <cmath>

//
// The centrepiece: a spectrum display showing the output (post-processing)
// magnitude, the live per-frequency gain reduction (teal "curtain" — what the
// suppressor is cutting right now), and the soothe-style reduction-profile curve
// with fixed nodes: a low cut and a high cut (drag frequency) plus eight typed
// bands (drag frequency/sensitivity, width via wheel/knob). Selecting a node
// opens a small bright inline editor (On + Listen + slope/type + freq [+ sens
// [+ width]]) over the analyser bottom. Edits go through the APVTS.
//
// Phase 5a-2 (soothe-class editor UX):
//   - Double-click empty space enables the lowest-index OFF band at that
//     freq/sens; double-click a band node resets its sensitivity to 0.
//   - Right-click opens an async PopupMenu (On/Off, Listen, Type/Slope, Reset,
//     Remove); Delete/Backspace removes the selected band; arrow keys nudge
//     freq/sens; the mouse wheel adjusts width (bands) / slope (cuts);
//     Shift+drag scales the drag delta by 0.15 for fine adjustment.
//   - Off BANDS are hidden (not dimmed) so a cluttered profile stays readable
//     — LC/HC stay dimmed-but-visible as before (always reachable). nodeAt()
//     excludes off bands from hit-testing to match.
//   - Listen (processor.setListenNode) is toggled from the node panel or the
//     context menu; selecting a different node (or closing the panel) always
//     drops it first (see selectNode()) — the editor's destructor drops it too
//     (PluginEditor.cpp).
//   - The analyser gained a Pre/Post/Both mode (small cycling label), a Freeze
//     toggle (holds the last frame) and a GR badge (peak reduction, ~1 s hold).
// GUI-thread only.
//
class SuppressionCurveComponent : public juce::Component,
                                  private juce::Timer
{
public:
    SuppressionCurveComponent (ResonanceSuppressorAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s), panel (p, s)
    {
        addChildComponent (panel); // shown when a node is selected
        panel.onCloseRequested = [this] { selectNode (-1); }; // ✕ -> fully deselect (drops Listen too, see selectNode())
        setWantsKeyboardFocus (true); // Delete/Backspace + arrow-key nudge on the selected node
        startTimerHz (30);

        // DEV panel: hidden until Alt+clicked in the top-right gate (see mouseDown).
        addChildComponent (devPanel);
        devPanel.onStyleChanged = [this] (const rs::AnalyzerStyle& st)
        {
            setAnalyzerStyle (st);                               // P3a: repaint with the new look
            processor.setDisplaySmoothMs (st.tempoSmoothingMs);  // P3b-A: RT-safe live display smoothing
            saveDevSettings();                                   // persist (below)
        };
        devPanel.onClose = [this] { devPanel.setVisible (false); repaint(); };

        // Persist DEV style in a user-global PropertiesFile -- deliberately NOT the
        // plugin's APVTS state/preset, so state/preset compatibility is untouched.
        juce::PropertiesFile::Options o;
        o.applicationName     = "ResonanceTatSuppressor";
        o.filenameSuffix      = "settings";
        o.folderName          = "TatsunariSounds";
        o.osxLibrarySubFolder = "Application Support";
        devProps.setStorageParameters (o);
        if (auto* us = devProps.getUserSettings())
        {
            const auto saved = us->getValue ("analyzerDevStyle", juce::String());
            if (saved.isNotEmpty())
                devPanel.setStateString (saved); // fires onStyleChanged -> applies style+tempo (redundant save is harmless)
        }
    }

    void paint (juce::Graphics& g) override
    {
        // Analyzer container / plot background (demo-analysis SS3.1): warm peach
        // vertical gradient + a 1 px inset hairline (replaces the old white card).
        auto full = getLocalBounds().toFloat();
        juce::ColourGradient plotBg (rs::colour::plotTop(), 0.0f, full.getY(),
                                     rs::colour::plotBottom(), 0.0f, full.getBottom(), false);
        g.setGradientFill (plotBg);
        g.fillRoundedRectangle (full, 14.0f);
        g.setColour (rs::colour::border());
        g.drawRoundedRectangle (full.reduced (0.5f), 14.0f, 1.0f);

        auto r = full.reduced (12.0f);
        controlsRow = r.removeFromTop (22.0f); // mode/freeze chips + GR badge
        r.removeFromTop (14.0f);               // Hz labels sit above the plot
        plot = r;
        layoutControlsRow();

        drawGrid (g);
        drawAnalyzer (g);
        drawReduction (g);
        drawProfile (g);
        drawNodes (g);
        drawHeaderControls (g);
        drawTooltip (g);

        // Soft shadow so the selected-node editor floats above the analyser.
        if (panel.isVisible())
            factory_ui::dropShadowFor (g, panel.getBounds(), 12.0f);

        if (devPanel.isVisible())
            factory_ui::dropShadowFor (g, devPanel.getBounds(), 12.0f);
    }

    void resized() override
    {
        if (panel.isVisible()) positionPanel();

        auto full = getLocalBounds().toFloat();
        constexpr float gate = 30.0f;
        devGateBounds = juce::Rectangle<float> (full.getRight() - gate - 6.0f, full.getY() + 6.0f, gate, gate);
        if (devPanel.isVisible()) positionDevPanel();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showContextMenu (e.position); return; }
        if (e.mods.isAltDown() && devGateBounds.contains (e.position)) { toggleDevPanel(); return; }

        if (hitsModeChip (e.position))   { cycleAnalyzerMode(); repaint(); return; }
        if (hitsFreezeChip (e.position)) { freeze = ! freeze;   repaint(); return; }

        const int hit = nodeAt (e.position);
        if (hit < 0) { selectNode (-1); return; } // click empty space -> close editor
        selectNode (hit);                          // selecting a node opens its editor
        dragging    = hit;
        dragAnchor  = e.position;
        dragVirtual = nodePos (hit); // seed at the node centre, not the click point,
        // so a grab up to the hit radius (14 px) off-centre doesn't snap on the
        // first drag — deltas accumulate from where the node actually sits.
        beginGesture (hit);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;

        // Shift = fine adjustment: scale the incremental mouse delta (from the
        // last event, not the drag start) by 0.15 before accumulating into the
        // virtual position used for the freq/sens mapping — held-Shift toggling
        // mid-drag is fine, the virtual position just tracks whatever scale was
        // active for each increment.
        const auto delta = e.position - dragAnchor;
        dragAnchor = e.position;
        const float scale = e.mods.isShiftDown() ? 0.15f : 1.0f;
        dragVirtual += delta * scale;
        dragVirtual.x = juce::jlimit (plot.getX(), plot.getRight(),  dragVirtual.x);
        dragVirtual.y = juce::jlimit (plot.getY(), plot.getBottom(), dragVirtual.y);

        setParam (pid (dragging, "freq"), xToFreq (dragVirtual.x));
        if (! isCut (dragging)) // bands carry a sensitivity (Y); cuts sit on the 0 line
            setParam (pid (dragging, "sens"), yToSens (dragVirtual.y));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) endGesture (dragging);
        dragging = -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = nodeAt (e.position);
        if (h != hoverNode) { hoverNode = h; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverNode >= 0) { hoverNode = -1; repaint(); }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (hitsModeChip (e.position) || hitsFreezeChip (e.position)) return; // already handled as a single click

        const int hit = nodeAt (e.position);
        if (hit >= 0)
        {
            if (! isCut (hit)) setParamGestured (pid (hit, "sens"), 0.0f); // sens-only reset on a node
            return;
        }
        addBandAt (e.position); // empty space -> enable the lowest-index OFF band here
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        int id = nodeAt (e.position);
        if (id < 0) id = selectedNode;
        if (id < 0 || wheel.deltaY == 0.0f) return;

        const bool up = wheel.deltaY > 0.0f;
        if (isCut (id))
        {
            const int cur  = (int) apvts.getRawParameterValue (pid (id, "slope"))->load();
            const int next = juce::jlimit (0, 3, cur + (up ? 1 : -1));
            if (next != cur) setParamGestured (pid (id, "slope"), (float) next);
        }
        else
        {
            const float cur  = apvts.getRawParameterValue (pid (id, "width"))->load();
            const float next = juce::jlimit (0.10f, 2.00f, cur * (up ? 1.15f : (1.0f / 1.15f)));
            if (next != cur) setParamGestured (pid (id, "width"), next); // no empty gesture at the clamp (matches the slope path)
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (selectedNode < 0) return false;
        const int id = selectedNode;

        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (isCut (id)) return false; // cuts can only be toggled On/Off, not removed
            removeBand (id);
            return true;
        }
        if (key == juce::KeyPress::leftKey)  { nudgeFreq (id, -1); return true; }
        if (key == juce::KeyPress::rightKey) { nudgeFreq (id, +1); return true; }
        if (! isCut (id))
        {
            if (key == juce::KeyPress::upKey)   { nudgeSens (id, +0.5f); return true; }
            if (key == juce::KeyPress::downKey) { nudgeSens (id, -0.5f); return true; }
        }
        return false;
    }

    // Phase P3a: the DEV panel (P3b) drives the analyzer's look entirely
    // through this -- swapping `style` between kV201Style/kDemoStyle/a lerp()
    // blend is the whole mechanism; drawAnalyzer/drawReduction/drawProfile
    // read nothing hardcoded that this doesn't cover. No caller exists yet
    // (that's P3b's DEV panel).
    void setAnalyzerStyle (const rs::AnalyzerStyle& s) { style = s; repaint(); }

private:
    void timerCallback() override
    {
        if (! freeze)
        {
            const int bins = processor.binsForDisplay();
            snapBins = bins;
            snapSr   = processor.getSampleRateForDisplay(); // captured WITH snapBins so a
            // frozen frame keeps a consistent bin<->Hz mapping even if the host
            // sample rate later changes (drawAnalyzer/drawReduction use snapSr too).
            const int N = 2 * (bins - 1);
            float worst = 0.0f;
            for (int k = 0; k < bins; ++k)
            {
                lastPost[(size_t) k] = processor.displayMagDb (k);
                lastPre [(size_t) k] = processor.displayMagPreDb (k);
                const float red = processor.displayRedDb (k);
                lastRed [(size_t) k] = red;
                // GR badge tracks only the displayed 20 Hz–20 kHz band, matching
                // the analyser (out-of-band bins must not skew the peak).
                const float f = N > 0 ? (float) ((double) k * snapSr / N) : 0.0f;
                if (f >= 20.0f && f <= 20000.0f)
                    worst = juce::jmin (worst, red);
            }
            updateGrHold (worst);
        }
        if (panel.isVisible()) panel.refreshListenState(); // reflect a menu-driven Listen change
        repaint();
    }

    static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;    // analyser axis
    static constexpr float kSensMin = -30.0f, kSensMax = 30.0f; // profile (sens) axis
    static constexpr int   kNumNodes = 2 + ResonanceSuppressorAudioProcessor::kNumBands; // LC, HC, bands
    // Phase P3a: point count for the combined/profile curve when
    // style.pathProfileCurved == false -- a fixed count (not plot-width
    // dependent), matching the demo's viewBox-space vertex count staying put
    // regardless of window size (rs-ui-work/demo-analysis.md SS3.2 Layer 4).
    static constexpr int kSparseProfileSteps = 48;

    enum class AnalyzerMode { Both, Pre, Post };

    juce::Rectangle<float> plotRect() const
    {
        auto r = getLocalBounds().toFloat().reduced (12.0f);
        r.removeFromTop (22.0f); // controls row (mode/freeze chips, GR badge) -- must match paint()
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

    bool  nodeOn    (int id) const { return apvts.getRawParameterValue (pid (id, "on"))->load() > 0.5f; }
    float nodeFreq  (int id) const { return apvts.getRawParameterValue (pid (id, "freq"))->load(); }
    float nodeSens  (int id) const { return isCut (id) ? 0.0f : apvts.getRawParameterValue (pid (id, "sens"))->load(); }
    float nodeWidth (int id) const { return isCut (id) ? 0.0f : apvts.getRawParameterValue (pid (id, "width"))->load(); }

    juce::Point<float> nodePos (int id) const { return { freqToX (nodeFreq (id)), sensToY (nodeSens (id)) }; }

    // demo-analysis SS3.1: decade freq lines (100/1k/10k) + the 0 dB reference
    // line are the "strong" grid tier (#f2ddd4); everything else is "faint"
    // (#f7e7e0). Same freqToX / label set / dB step as before -- colour only.
    void drawGrid (juce::Graphics& g)
    {
        g.setFont (rs::font (rs::FontKind::Ui, 10.0f, 600));
        struct FL { float f; const char* s; bool strong; };
        for (auto fl : { FL{50,"50",false}, FL{100,"100",true}, FL{200,"200",false}, FL{500,"500",false},
                         FL{1000,"1k",true}, FL{2000,"2k",false}, FL{5000,"5k",false}, FL{10000,"10k",true} })
        {
            const float x = freqToX (fl.f);
            g.setColour (fl.strong ? rs::colour::border() : rs::colour::borderLight());
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (fl.strong ? rs::colour::textMuted() : rs::colour::textFaint());
            g.drawText (fl.s, juce::Rectangle<float> (x - 18.0f, plot.getY() - 13.0f, 36.0f, 12.0f),
                        juce::Justification::centred);
        }
        for (float db = 0.0f; db >= -60.0f; db -= 12.0f)
        {
            const float y = dbToY (db);
            g.setColour (db == 0.0f ? rs::colour::border() : rs::colour::borderLight());
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
    }

    // Phase P3a: 1/N-octave moving-average smoothing across frequency bins,
    // applied only to the GUI's PRE/POST/delta display traces (GUI-thread
    // only, small temporary buffers -- never touches processBlock). `octFrac
    // <= 0` is a no-op (0 == raw == exact pre-P3a behaviour). Deliberately a
    // simple per-bin box average in dB space over the bins within +/-
    // octFrac/2 octaves of each bin's frequency -- a DEV-panel cosmetic knob,
    // not a measurement filter.
    static void smoothOctaveBins (std::array<float, ResonanceSuppressorAudioProcessor::kMaxBins>& data,
                                  int numBins, double sampleRate, int fftN, float octFrac)
    {
        if (octFrac <= 0.0f || numBins <= 2 || fftN <= 0) return;
        const auto src = data; // every output bin must read unsmoothed inputs
        const double halfOctMul = std::pow (2.0, 0.5 * (double) octFrac);
        for (int k = 1; k < numBins; ++k)
        {
            const double fk = (double) k * sampleRate / fftN;
            if (fk <= 0.0) continue;
            const int kLo = juce::jlimit (1, numBins - 1, (int) std::floor (fk / halfOctMul * fftN / sampleRate));
            const int kHi = juce::jlimit (kLo,  numBins - 1, (int) std::ceil  (fk * halfOctMul * fftN / sampleRate));
            double sum = 0.0;
            for (int j = kLo; j <= kHi; ++j) sum += src[(size_t) j];
            data[(size_t) k] = (float) (sum / (double) (kHi - kLo + 1));
        }
    }

    // Pre (input) / Post (output) spectra. Pre draws as a pale filled area, Post
    // as a crisp line, so Both mode reads like soothe's dual trace; Freeze holds
    // the last-captured frame (see timerCallback / lastPre / lastPost).
    void drawAnalyzer (juce::Graphics& g)
    {
        const double sr = snapSr; // frozen together with snapBins (see timerCallback)
        const int N = 2 * (snapBins - 1);

        // style.freqSmoothingOct == 0 (kV201Style) takes the early-out inside
        // smoothOctaveBins() and leaves these byte-identical to lastPre/lastPost.
        auto smPre  = lastPre;
        auto smPost = lastPost;
        smoothOctaveBins (smPre,  snapBins, sr, N, style.freqSmoothingOct);
        smoothOctaveBins (smPost, snapBins, sr, N, style.freqSmoothingOct);

        auto buildTrace = [&] (const std::array<float, ResonanceSuppressorAudioProcessor::kMaxBins>& data)
        {
            factory_ui::SpectrumTrace trace;
            trace.begin (plot.getBottom(), plot.getRight());
            for (int k = 1; k < snapBins; ++k)
            {
                const float f = (float) ((double) k * sr / N);
                if (f < 20.0f || f > 20000.0f) continue;
                trace.addPoint (freqToX (f), dbToY (data[(size_t) k]));
            }
            return trace;
        };

        // style.inputMode has exactly one value today (FilledArea); kept as
        // an enum in the schema for a future DEV mode, not switched on here.
        const auto joint = style.traceRoundJoin ? juce::PathStrokeType::curved : juce::PathStrokeType::mitered;

        if (analyzerMode != AnalyzerMode::Post)
        {
            auto trace = buildTrace (smPre);
            if (! trace.isEmpty())
            {
                factory_ui::fillSpectrumArea (g, trace.area(), style.inputColour, plot,
                                              style.inputFillTopAlpha, style.inputFillBotAlpha);
                if (style.inputLineWidth > 0.0f && style.inputLineAlpha > 0.0f)
                {
                    g.setColour (style.inputColour.withAlpha (style.inputLineAlpha));
                    g.strokePath (trace.line(), juce::PathStrokeType (style.inputLineWidth, joint));
                }
            }
        }
        if (analyzerMode != AnalyzerMode::Pre)
        {
            auto trace = buildTrace (smPost);
            if (! trace.isEmpty())
            {
                g.setColour (style.postColour.withAlpha (style.postLineAlpha));
                g.strokePath (trace.line(), juce::PathStrokeType (style.postLineWidth, joint));
            }
        }
    }

    // Live gain reduction hanging from the 0 dB line (style.deltaMode ==
    // AreaFromZero, v2.0.1) or from the plot's physical top edge, optionally
    // clamped to style.deltaClampFrac of the plot height (CurtainFromTop, the
    // demo's Layer 2) -- the suppressor at work.
    void drawReduction (juce::Graphics& g)
    {
        const double sr = snapSr; // frozen together with snapBins (see timerCallback)
        const int N = 2 * (snapBins - 1);

        auto smRed = lastRed;
        smoothOctaveBins (smRed, snapBins, sr, N, style.freqSmoothingOct);

        const bool  fromTop = style.deltaMode == rs::AnalyzerStyle::DeltaMode::CurtainFromTop;
        const float top     = fromTop ? plot.getY() : dbToY (0.0f);
        const float maxY    = top + style.deltaClampFrac * plot.getHeight(); // 1.0 == unclamped (see below)

        factory_ui::SpectrumTrace trace;
        trace.begin (top, plot.getRight());
        for (int k = 1; k < snapBins; ++k)
        {
            const float f = (float) ((double) k * sr / N);
            if (f < 20.0f || f > 20000.0f) continue;
            const float red = juce::jlimit (-60.0f, 0.0f, smRed[(size_t) k]);
            // Same dB->pixel scale as dbToY() in both modes, just re-based so
            // AreaFromZero anchors at the 0 dB line (top == dbToY(0), so this
            // reduces exactly to dbToY(red) -- byte-identical to pre-P3a) and
            // CurtainFromTop anchors at the plot's physical top edge instead.
            const float y = juce::jmin (top + (dbToY (red) - dbToY (0.0f)), maxY);
            trace.addPoint (freqToX (f), y); // red <= 0 -> at/below `top`
        }
        if (! trace.isEmpty())
        {
            const auto fill = trace.area();
            g.setColour (kTeal.withAlpha (style.deltaFillAlpha));
            g.fillPath (fill);
            if (style.deltaStrokeAlpha > 0.0f && style.deltaStrokeWidth > 0.0f)
            {
                g.setColour (kTeal.withAlpha (style.deltaStrokeAlpha));
                g.strokePath (fill, juce::PathStrokeType (style.deltaStrokeWidth));
            }
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
    // style.curveMode picks between v2.0.1's modern-EQ look (each active
    // node's own translucent fill+stroke, under a glowing combined-response
    // stroke) and the demo's look (a single dashed combined curve, no
    // per-node curves at all -- demo-analysis SS3.2 Layer 4 / SS5.2).
    void drawProfile (juce::Graphics& g)
    {
        using CurveMode = rs::AnalyzerStyle::CurveMode;
        const auto nodes = ResonanceSuppressorAudioProcessor::readNodes (apvts);
        // pathProfileCurved: dense ~1-point-per-pixel sampling (current --
        // reads as smooth purely from point density) vs a fixed sparse point
        // count with straight segments (kSparseProfileSteps; closer to the
        // demo's hand-authored, visibly-faceted polyline).
        const int steps = style.pathProfileCurved ? juce::jmax (2, (int) plot.getWidth())
                                                   : kSparseProfileSteps;
        const float y0 = sensToY (0.0f); // nominal (0 dB) line

        const bool wantPerNode  = style.curveMode != CurveMode::CombinedOnly;
        const bool wantCombined = style.curveMode != CurveMode::PerNodeFillsOnly;

        std::array<factory_core::ReductionNodes, kNumNodes> single;
        std::array<juce::Path, kNumNodes> nodePath;
        std::array<bool, kNumNodes> started {};
        if (wantPerNode)
            for (int id = 0; id < kNumNodes; ++id) single[(size_t) id] = singleNode (nodes, id);

        juce::Path combined;
        // Trim "floored" runs from the combined curve: where a low-/high-cut has
        // rolled all the way down to the plot floor, don't draw the flat line out
        // to the analyzer edge -- cut it at the roll-off point. The descent to /
        // ascent from the floor is preserved (a single anchor point) so the curve
        // still visibly meets the floor before it stops.
        const float floorY = plot.getBottom();
        bool combPenDown = false, combHaveAnchor = false;
        juce::Point<float> combAnchor;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot.getX() + (float) i * plot.getWidth() / steps;
            const float f = xToFreq (x);

            if (wantCombined)
            {
                const float yT = sensToY ((float) factory_core::reductionProfileDbAt (f, nodes));
                if (yT >= floorY - 0.5f) // at the floor: hold the pen, don't stroke a horizontal run
                {
                    if (combPenDown) { combined.lineTo (x, yT); combPenDown = false; } // land on the floor, then lift
                    combAnchor = { x, yT }; combHaveAnchor = true;
                }
                else if (! combPenDown) // lifting off the floor: rise out of the last floor point
                {
                    if (combHaveAnchor) { combined.startNewSubPath (combAnchor.x, combAnchor.y); combined.lineTo (x, yT); }
                    else                combined.startNewSubPath (x, yT);
                    combPenDown = true; combHaveAnchor = false;
                }
                else combined.lineTo (x, yT);
            }

            if (wantPerNode)
            {
                for (int id = 0; id < kNumNodes; ++id)
                {
                    if (! nodeOn (id)) continue;
                    const float yB = sensToY ((float) factory_core::reductionProfileDbAt (f, single[(size_t) id]));
                    if (! started[(size_t) id]) { nodePath[(size_t) id].startNewSubPath (x, yB); started[(size_t) id] = true; }
                    else nodePath[(size_t) id].lineTo (x, yB);
                }
            }
        }

        // Per-node translucent fill (from the nominal line) + coloured stroke.
        if (wantPerNode)
        {
            for (int id = 0; id < kNumNodes; ++id)
            {
                if (! started[(size_t) id]) continue;
                const auto col = isCut (id) ? FactoryLookAndFeel::textDim() : FactoryLookAndFeel::bandColour (id - 2);
                if (style.perNodeFillAlpha > 0.0f)
                {
                    auto fp = nodePath[(size_t) id];
                    fp.lineTo (plot.getRight(), y0);
                    fp.lineTo (plot.getX(), y0);
                    fp.closeSubPath();
                    g.setColour (col.withAlpha (style.perNodeFillAlpha));
                    g.fillPath (fp);
                }
                if (style.perNodeStrokeAlpha > 0.0f)
                {
                    g.setColour (col.withAlpha (style.perNodeStrokeAlpha));
                    g.strokePath (nodePath[(size_t) id], juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
                }
            }
        }

        // Combined response: optional soft glow under a crisp stroke, solid
        // or dashed (style.combinedDashLen > 0).
        if (wantCombined)
        {
            if (style.combinedGlowAlpha > 0.0f && style.combinedGlowWidth > 0.0f)
            {
                g.setColour (style.combinedColour.withAlpha (style.combinedGlowAlpha));
                g.strokePath (combined, juce::PathStrokeType (style.combinedGlowWidth,
                              juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour (style.combinedColour.withAlpha (style.combinedStrokeAlpha));
            const juce::PathStrokeType combinedStroke (style.combinedStrokeWidth,
                                                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            if (style.combinedDashLen > 0.0f)
            {
                juce::Path dashed;
                const float dl[] = { style.combinedDashLen, style.combinedDashGap };
                combinedStroke.createDashedStroke (dashed, combined, dl, 2);
                g.fillPath (dashed);
            }
            else
            {
                g.strokePath (combined, combinedStroke);
            }
        }
    }

    // demo-analysis SS3.5: band nodes are plain pastel dots (15px dia., 20px selected)
    // with a white halo + soft drop shadow; cuts are square handles (13x13 r4)
    // with a coloured ring + a dashed vertical guide. Neither carries a text
    // label in the demo (identity now reads from position/colour/the popover's
    // "Band N" header) -- everything else (skip/dim rules, listen ring, the
    // selected node growing) is the same semantics as before, just restyled.
    void drawNodes (juce::Graphics& g)
    {
        const int listen = processor.getListenNode();
        for (int id = 0; id < kNumNodes; ++id)
        {
            const bool on = nodeOn (id);
            // Phase 5a: off BANDS are hidden entirely (not dimmed) so a busy
            // profile stays readable — cuts stay dimmed-but-visible as before
            // so LC/HC are always reachable to re-enable.
            if (! isCut (id) && ! on) continue;
            const float a = on ? 1.0f : 0.3f;
            const auto p = nodePos (id);
            const bool selected = (id == selectedNode);

            if (isCut (id))
            {
                const auto ring = (id == 0) ? rs::colour::orange() : kHighCutRing; // LC / HC
                const float dashes[] = { 4.0f, 4.0f };
                g.setColour (rs::colour::textFaint().withAlpha (a));
                g.drawDashedLine (juce::Line<float> (p.x, plot.getY(), p.x, plot.getBottom()), dashes, 2, 1.0f);

                const float sz = selected ? 18.0f : 13.0f; // demo: 13x13px r4 (grown when selected)
                const auto sq = juce::Rectangle<float> (sz, sz).withCentre (p);

                if (id == listen) // Listen ring, outermost
                {
                    g.setColour (kTeal.withAlpha (0.9f));
                    g.drawRoundedRectangle (sq.expanded (5.0f), rs::radius::cutHandle + 3.0f, 2.0f);
                }
                { // soft drop shadow (approximates the demo's "0 2px 5px rgba(107,87,80,.25)")
                    juce::DropShadow ds (juce::Colour::fromFloatRGBA (107.0f / 255.0f, 87.0f / 255.0f, 80.0f / 255.0f, 0.25f * a),
                                         5, { 0, 2 });
                    juce::Path sp; sp.addRoundedRectangle (sq, rs::radius::cutHandle);
                    ds.drawForPath (g, sp);
                }
                g.setColour (ring.withAlpha (a)); // 2 px colour ring (box-shadow spread, approximated as a fill)
                g.fillRoundedRectangle (sq.expanded (2.0f), rs::radius::cutHandle + 1.5f);
                g.setColour (rs::colour::footerBg().withAlpha (a)); // handle bg #fff4ee
                g.fillRoundedRectangle (sq, rs::radius::cutHandle);
            }
            else
            {
                const auto col = FactoryLookAndFeel::bandColour (id - 2);
                const float dotD  = selected ? 20.0f : 15.0f; // demo: 15px dia., 20px selected
                const float haloD = dotD + 6.0f;               // "0 0 0 3px #fff" halo
                const auto dotR  = juce::Rectangle<float> (dotD, dotD).withCentre (p);
                const auto haloR = juce::Rectangle<float> (haloD, haloD).withCentre (p);

                if (id == listen) // Listen ring, outermost so it doesn't collide with the halo
                {
                    g.setColour (kTeal.withAlpha (0.9f));
                    g.drawEllipse (juce::Rectangle<float> (haloD + 7.0f, haloD + 7.0f).withCentre (p), 2.0f);
                }
                { // soft drop shadow ("0 2px 6px rgba(107,87,80,.28)")
                    juce::DropShadow ds (juce::Colour::fromFloatRGBA (107.0f / 255.0f, 87.0f / 255.0f, 80.0f / 255.0f, 0.28f),
                                         6, { 0, 2 });
                    juce::Path dp; dp.addEllipse (dotR);
                    ds.drawForPath (g, dp);
                }
                g.setColour (rs::colour::white());
                g.fillEllipse (haloR);
                g.setColour (col);
                g.fillEllipse (dotR);
            }
        }
    }

    // Small header chips (Pre/Post/Both, Freeze) + the GR badge. demo-analysis
    // SS2.2: Pre/Post/Both sits top-left (A1); Freeze + GR sit top-right,
    // Freeze left of GR (A2/A3) -- unlike the old layout, which grouped Freeze
    // next to the mode chip on the left.
    void layoutControlsRow()
    {
        auto r = controlsRow;
        modeChipBounds = r.removeFromLeft (132.0f);
        grBadgeBounds  = r.removeFromRight (100.0f);
        r.removeFromRight (6.0f);
        freezeChipBounds = r.removeFromRight (66.0f);
    }

    bool hitsModeChip   (juce::Point<float> pos) const { return modeChipBounds.contains (pos); }
    bool hitsFreezeChip (juce::Point<float> pos) const { return freezeChipBounds.contains (pos); }

    void cycleAnalyzerMode()
    {
        analyzerMode = (analyzerMode == AnalyzerMode::Both) ? AnalyzerMode::Pre
                     : (analyzerMode == AnalyzerMode::Pre)  ? AnalyzerMode::Post
                                                              : AnalyzerMode::Both;
    }

    // demo-analysis SS2.2 A1-A3. The mode chip keeps ONE click region over all
    // 3 segments (hitsModeChip/cycleAnalyzerMode below are unchanged) -- only
    // the paint gains the demo's 3-segment look, with whichever segment equals
    // the live analyzerMode drawn as the active coral pill (matches the demo,
    // where "Both" is the one shown active).
    void drawHeaderControls (juce::Graphics& g)
    {
        auto chipShell = [&] (juce::Rectangle<float> b)
        {
            g.setColour (rs::colour::chipBg());
            g.fillRoundedRectangle (b, rs::radius::badge);
            g.setColour (rs::colour::border());
            g.drawRoundedRectangle (b.reduced (0.5f), rs::radius::badge, 1.0f);
        };

        // A1: Pre / Post / Both.
        chipShell (modeChipBounds);
        {
            static constexpr const char* kSegNames[] = { "Pre", "Post", "Both" };
            static constexpr AnalyzerMode kSegModes[] = { AnalyzerMode::Pre, AnalyzerMode::Post, AnalyzerMode::Both };
            auto inner = modeChipBounds.reduced (3.0f);
            const float segW = inner.getWidth() / 3.0f;
            for (int i = 0; i < 3; ++i)
            {
                const auto seg = juce::Rectangle<float> (inner.getX() + (float) i * segW, inner.getY(), segW, inner.getHeight());
                const bool active = (analyzerMode == kSegModes[i]);
                if (active)
                {
                    g.setColour (rs::colour::accent());
                    g.fillRoundedRectangle (seg.reduced (1.0f), rs::radius::badge - 2.0f);
                }
                g.setColour (active ? rs::colour::white() : rs::colour::textMuted());
                g.setFont (rs::font (rs::FontKind::Ui, 9.5f, 800));
                g.drawText (kSegNames[i], seg, juce::Justification::centred);
            }
        }

        // A2: Freeze.
        chipShell (freezeChipBounds);
        g.setColour (rs::colour::textSecondary());
        g.setFont (rs::font (rs::FontKind::Ui, 9.5f, 800));
        g.drawText (freeze ? "Frozen" : "Freeze", freezeChipBounds, juce::Justification::centred);

        // A3: GR peak-hold badge.
        g.setColour (kGrBg);
        g.fillRoundedRectangle (grBadgeBounds, rs::radius::badge);
        g.setColour (kGrBorder);
        g.drawRoundedRectangle (grBadgeBounds.reduced (0.5f), rs::radius::badge, 1.0f);
        g.setColour (kGrText);
        g.setFont (rs::font (rs::FontKind::Ui, 9.5f, 800));
        g.drawText ("GR " + juce::String (grPeakDb, 1) + " dB", grBadgeBounds, juce::Justification::centred);
    }

    // GR badge peak-hold: jump immediately to a bigger (more negative) cut, hold
    // for ~1 s at 30 Hz, then relax back down towards the instantaneous value.
    void updateGrHold (float instantDb)
    {
        constexpr int   kHoldFrames      = 30;   // ~1 s at the 30 Hz timer
        constexpr float kDecayDbPerFrame = 0.5f;
        if (instantDb <= grPeakDb)
        {
            grPeakDb = instantDb;
            grHoldCounter = kHoldFrames;
        }
        else if (grHoldCounter > 0)
        {
            --grHoldCounter;
        }
        else
        {
            grPeakDb = juce::jmin (0.0f, grPeakDb + kDecayDbPerFrame);
            if (grPeakDb > instantDb) grPeakDb = instantDb; // don't overshoot past the current value
        }
    }

    // Freq/sens/width readout near the cursor while dragging or hovering a node
    // (drawn directly, not a SettableTooltipClient — see the Phase 5a-2 spec).
    void drawTooltip (juce::Graphics& g)
    {
        const int id = dragging >= 0 ? dragging : hoverNode;
        if (id < 0) return;

        const auto anchor = dragging >= 0 ? dragVirtual : nodePos (id);
        const float f = nodeFreq (id);
        juce::String text = f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz"
                                          : juce::String (juce::roundToInt (f)) + " Hz";
        if (! isCut (id))
            text << "   " << juce::String (nodeSens (id), 1) << " dB"
                 << "   " << juce::String (nodeWidth (id), 2) << " oct";

        const juce::Font font (juce::FontOptions (10.5f, juce::Font::bold));
        g.setFont (font);
        const float w = juce::GlyphArrangement::getStringWidth (font, text) + 12.0f;
        auto box = juce::Rectangle<float> (w, 18.0f).withCentre ({ anchor.x, anchor.y - 20.0f });
        box.setX (juce::jlimit (plot.getX(), plot.getRight() - w, box.getX()));
        box.setY (juce::jmax (plot.getY(), box.getY()));

        g.setColour (FactoryLookAndFeel::text().withAlpha (0.88f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (juce::Colours::white);
        g.drawText (text, box, juce::Justification::centred);
    }

    int nodeAt (juce::Point<float> pos) const
    {
        int best = -1; float bestD = 14.0f;
        for (int id = 0; id < kNumNodes; ++id)
        {
            // Phase 5a: off bands are hidden, so they're excluded from hit-testing
            // too — cuts stay hit-testable even when off (see drawNodes).
            if (! isCut (id) && ! nodeOn (id)) continue;
            const float d = nodePos (id).getDistanceFrom (pos);
            if (d <= bestD) { bestD = d; best = id; }
        }
        return best;
    }

    // Selecting a node opens the inline editor bound to it; -1 closes it.
    // Switching to a different node (or closing) always drops Listen first —
    // NodePanel::setNode() then reads Listen as off for a freshly-opened panel
    // (Phase 5a-2: "switching drops Listen").
    void selectNode (int id)
    {
        if (id != selectedNode)
            processor.setListenNode (-1);
        selectedNode = id;
        if (id >= 0) { panel.setNode (id); panel.setVisible (true); positionPanel(); grabKeyboardFocus(); }
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

    void saveDevSettings()
    {
        if (auto* us = devProps.getUserSettings())
        {
            us->setValue ("analyzerDevStyle", devPanel.getStateString());
            us->saveIfNeeded();
        }
    }

    void positionDevPanel()
    {
        // Top-right of the analyzer, clamped to the visible height; the panel's
        // internal Viewport scrolls any overflow (its natural 560 px can exceed a
        // short analyzer). Kept just inside the rounded card + clear of the corner.
        const int w = devPanel.preferredWidth();
        auto full = getLocalBounds().reduced (14);
        const int h = juce::jmin (devPanel.preferredHeight(), full.getHeight());
        devPanel.setBounds (full.getRight() - w, full.getY(), w, h);
    }

    void toggleDevPanel()
    {
        const bool show = ! devPanel.isVisible();
        if (show) { positionDevPanel(); devPanel.setVisible (true); devPanel.toFront (false); }
        else        devPanel.setVisible (false);
        repaint();
    }

    void setParam (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (value));
    }
    // One-shot parameter write with its own begin/end gesture (add/remove,
    // wheel, keyboard nudge, menu actions — anything that isn't a held drag).
    void setParamGestured (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
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

    // Empty-space double-click: enable the lowest-index OFF band at the
    // clicked freq/sens (width/type/on default to 0.5 oct / Bell / on). Soft
    // no-op (documented, not an error) if every band is already on.
    void addBandAt (juce::Point<float> pos)
    {
        int target = -1;
        for (int b = 0; b < ResonanceSuppressorAudioProcessor::kNumBands; ++b)
            if (! nodeOn (2 + b)) { target = 2 + b; break; }
        if (target < 0) return; // all 8 bands already on — nothing to add

        const float f = xToFreq (juce::jlimit (plot.getX(), plot.getRight(), pos.x));
        const float s = yToSens (juce::jlimit (plot.getY(), plot.getBottom(), pos.y));

        setParamGestured (pid (target, "freq"),  f);
        setParamGestured (pid (target, "sens"),  s);
        setParamGestured (pid (target, "width"), 0.5f);
        setParamGestured (pid (target, "type"),  0.0f); // Bell
        setParamGestured (pid (target, "on"),    1.0f);

        selectNode (target); // open its editor immediately, Pro-Q-style
    }

    void removeBand (int id)
    {
        if (isCut (id)) return; // cuts can only be toggled On/Off, never removed
        setParamGestured (pid (id, "on"), 0.0f);
        if (selectedNode == id) selectNode (-1); // its panel just lost its node
    }

    void resetNode (int id)
    {
        if (isCut (id)) return; // cuts have no sens/width to reset
        setParamGestured (pid (id, "sens"),  0.0f);
        setParamGestured (pid (id, "width"), 0.5f);
    }

    void toggleOn (int id)
    {
        const bool turningOff = nodeOn (id);
        setParamGestured (pid (id, "on"), turningOff ? 0.0f : 1.0f);
        // Off'ing a soloed band must drop Listen too, or processBlock keeps
        // soloing a now-hidden, nominal-profile node — a silent dead state with
        // no on-screen indicator (the node vanished). Covers the context-menu /
        // toggleOn path; the NodePanel On toggle covers itself (NodePanel.h).
        if (turningOff && processor.getListenNode() == id)
        {
            processor.setListenNode (-1);
            if (panel.isVisible() && panel.currentNode() == id) panel.refreshListenState();
        }
    }

    void toggleListen (int id)
    {
        processor.setListenNode (processor.getListenNode() == id ? -1 : id);
        if (panel.isVisible() && panel.currentNode() == id) panel.refreshListenState();
    }

    void nudgeFreq (int id, int dir)
    {
        const float next = nodeFreq (id) * std::pow (2.0f, (float) dir / 48.0f);
        setParamGestured (pid (id, "freq"), juce::jlimit (20.0f, 20000.0f, next));
    }
    void nudgeSens (int id, float deltaDb)
    {
        const float next = juce::jlimit (kSensMin, kSensMax, nodeSens (id) + deltaDb);
        setParamGestured (pid (id, "sens"), next);
    }

    // JUCE 8: PopupMenu::show() runs a blocking modal loop and must not be used
    // (see the pluginval-debug / factory-ui pitfalls) — showMenuAsync only.
    void showContextMenu (juce::Point<float> pos)
    {
        const int id = nodeAt (pos);
        if (id < 0) return; // no node under the cursor -> no menu

        selectNode (id); // right-click also selects, so the panel matches the menu's target

        juce::PopupMenu menu;
        menu.addItem (1, nodeOn (id) ? "Off" : "On");
        menu.addItem (2, processor.getListenNode() == id ? "Stop Listen" : "Listen");

        if (isCut (id))
        {
            const int cur = (int) apvts.getRawParameterValue (pid (id, "slope"))->load();
            static constexpr const char* kSlopes[] = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
            juce::PopupMenu slopeMenu;
            for (int s = 0; s < 4; ++s) slopeMenu.addItem (200 + s, kSlopes[s], true, s == cur);
            menu.addSubMenu ("Slope", slopeMenu);
        }
        else
        {
            const int cur = (int) apvts.getRawParameterValue (pid (id, "type"))->load();
            static constexpr const char* kTypes[] = { "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" };
            juce::PopupMenu typeMenu;
            for (int t = 0; t < 6; ++t) typeMenu.addItem (100 + t, kTypes[t], true, t == cur);
            menu.addSubMenu ("Type", typeMenu);
        }

        menu.addItem (3, "Reset");
        if (! isCut (id)) menu.addItem (4, "Remove");

        juce::Component::SafePointer<SuppressionCurveComponent> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, id] (int result)
        {
            if (safeThis == nullptr || result == 0) return;
            safeThis->handleMenuResult (id, result);
        });
    }

    void handleMenuResult (int id, int result)
    {
        if (result == 1) { toggleOn (id); return; }
        if (result == 2) { toggleListen (id); return; }
        if (result == 3) { resetNode (id); return; }
        if (result == 4) { removeBand (id); return; }
        if (result >= 100 && result < 106) { setParamGestured (pid (id, "type"),  (float) (result - 100)); return; }
        if (result >= 200 && result < 204) { setParamGestured (pid (id, "slope"), (float) (result - 200)); return; }
    }

    inline static const juce::Colour kTeal        { juce::Colour (0xff45b8acu) };
    // A few demo hex values (demo-analysis SS1.3) with no rs::colour role of
    // their own (RsTheme.h is out of scope for this phase) -- kept local, same
    // pattern as kTeal above.
    inline static const juce::Colour kHighCutRing { juce::Colour (0xff79b8efu) }; // high-cut handle ring
    inline static const juce::Colour kGrBg        { juce::Colour (0xffeafaf7u) }; // GR badge bg
    inline static const juce::Colour kGrBorder    { juce::Colour (0xffb8e8e2u) }; // GR badge border
    inline static const juce::Colour kGrText      { juce::Colour (0xff2f9488u) }; // GR badge text

    ResonanceSuppressorAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    NodePanel panel;
    rs::AnalyzerDevPanel  devPanel;                 // アナライザーDEVモードのオーバーレイ(既定非表示)
    juce::ApplicationProperties devProps;           // DEV設定の永続化(plugin state/presetとは別・非汚染)
    juce::Rectangle<float> devGateBounds;           // 右上隅の控えめなAlt+クリック開閉ホットゾーン(非表示)
    juce::Rectangle<float> plot;
    int dragging = -1;
    int selectedNode = -1;
    int hoverNode = -1;
    juce::Point<float> dragAnchor, dragVirtual;

    // Phase P3a: the analyzer's configurable render style (see
    // AnalyzerStyle.h) -- defaults to kV201Style so nothing changes until a
    // caller (P3b's DEV panel) calls setAnalyzerStyle().
    rs::AnalyzerStyle style { rs::kV201Style };

    // Analyser display state (Phase 5a-2): last-captured frame (so Freeze can
    // hold it), the mode chip, and the GR peak-hold badge.
    AnalyzerMode analyzerMode = AnalyzerMode::Both;
    bool  freeze = false;
    int   snapBins = 0;
    double snapSr = 48000.0; // sample rate captured with snapBins (see timerCallback / #2)
    std::array<float, ResonanceSuppressorAudioProcessor::kMaxBins> lastPre {}, lastPost {}, lastRed {};
    float grPeakDb = 0.0f;
    int   grHoldCounter = 0;
    juce::Rectangle<float> controlsRow, modeChipBounds, freezeChipBounds, grBadgeBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuppressionCurveComponent)
};
