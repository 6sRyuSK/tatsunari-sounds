#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AnalyzerStyle.h"
#include "RsTheme.h"
#include "RsWidgets.h"
#include "RsLookAndFeel.h"

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

//
// rs::AnalyzerDevPanel -- Phase P3b-B: a floating, NodePanel-style card that
// live-edits an rs::AnalyzerStyle across its four independently-blendable
// "faces" (Input / Delta / Curve / Smooth -- the same split as
// rs::composePerFace() in AnalyzerStyle.h, added alongside this file). Each
// face gets a v2.0.1<->Demo blend slider plus a collapsible "Advanced" list of
// that face's raw fields for direct tuning.
//
// This is purely a settings surface: it owns no analyzer data, draws no
// spectrum, and knows nothing about SuppressionCurveComponent. Embedding it
// (gating when it's shown, positioning it, persisting getStateString() to
// disk) is SuppressionCurveComponent's job -- a separate phase (P3b-C). The
// public API below (ctor/dtor, onStyleChanged/onClose, style(),
// getStateString()/setStateString(), paint()/resized(),
// preferredWidth()/preferredHeight()) is the fixed contract P3b-C wires
// against -- see that section for the exact signatures.
//
// Two behaviours worth flagging for that integration:
//  - Dragging ANY face's blend slider recomposes the WHOLE style via
//    rs::composePerFace(tInput,tDelta,tCurve,tSmooth) (spec-P3.md's
//    documented behaviour: "その面のtだけ更新 -> composePerFace で current 再合成"),
//    which regenerates *every* face's fields from that pure interpolation --
//    including any other face's "Advanced" edits made since the last blend
//    touch. This panel resyncs every control (not just the touched face's)
//    whenever that happens, so the UI never goes stale relative to `current`.
//    Net effect: a per-field Advanced tweak is a "preview until the next
//    blend drag (on ANY face) or preset click" affordance -- composePerFace()
//    is a pure function of the four t's with no memory of prior overrides.
//  - The preset selector's highlighted segment is a simple function of the
//    four t's (v2.0.1 = all 0, 50/50 = all 0.5, Demo = all 1, anything else
//    shows no selection) -- it does not attempt to detect a per-field
//    Advanced override that happens to leave the t's at a clean preset value.
//
// Card chrome (white .97 / border / radius::popover), the LookAndFeel
// ownership pattern (setLookAndFeel in the ctor, cleared in the dtor) and the
// X CloseButton are transcribed from NodePanel.h -- same hand-painted idiom.
// Header-only, GUI-thread only; no PopupMenu/async callback is used anywhere
// in this file, so there's nothing that needs a Component::SafePointer.
//
namespace rs
{
    class AnalyzerDevPanel : public juce::Component
    {
    public:
        AnalyzerDevPanel();
        ~AnalyzerDevPanel() override;

        // Fired on any control edit (blend drag, Advanced edit, preset click,
        // or setStateString()), passing the freshly-composed style.
        std::function<void (const AnalyzerStyle&)> onStyleChanged;
        // Fired when the header X is clicked.
        std::function<void()> onClose;

        const AnalyzerStyle& style() const noexcept { return current; }

        // Serializes the four faces' blend t's plus every AnalyzerStyle field
        // (round-trips via setStateString()) as a compact JSON object string.
        // Persistence itself (PropertiesFile, load/save timing) is P3b-C's job.
        juce::String getStateString() const;
        // Restores from a getStateString() string. Missing/invalid keys fall
        // back to kV201Style's own value field-by-field; a malformed/empty
        // string simply leaves every field at kV201Style (never throws or
        // crashes -- juce::JSON::parse() itself never throws, it returns a
        // void var on failure). Syncs every control (blend sliders, Advanced
        // rows, preset highlight) and fires onStyleChanged().
        void setStateString (const juce::String& s);

        void paint (juce::Graphics&) override;
        void resized() override;

        static constexpr int kWidth  = 380;
        static constexpr int kHeight = 560;
        int preferredWidth()  const noexcept { return kWidth; }
        int preferredHeight() const noexcept { return kHeight; }

    private:
        // ---- chrome: the X close button, transcribed from NodePanel.h ----
        struct CloseButton : juce::Button
        {
            CloseButton() : juce::Button ({}) {}
            void paintButton (juce::Graphics& g, bool highlighted, bool) override
            {
                auto r = getLocalBounds().toFloat().reduced (highlighted ? 4.0f : 5.0f);
                g.setColour (colour::textFaint());
                juce::Path x;
                x.startNewSubPath (r.getTopLeft());    x.lineTo (r.getBottomRight());
                x.startNewSubPath (r.getBottomLeft()); x.lineTo (r.getTopRight());
                g.strokePath (x, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        };

        // ---- one of the four Input/Delta/Curve/Smooth groups: a blend
        // slider + a collapsible list of raw-field rows. Knows nothing about
        // AnalyzerStyle -- AnalyzerDevPanel wires each Advanced row's get/set
        // through addSliderRow()/addSegmentedRow(), so this is a reusable
        // "blend + N rows" shell instantiated once per face (see Content
        // below). onBlendChanged/onExpandedChanged are its only outputs.
        class FaceGroup : public juce::Component
        {
        public:
            void configure (const juce::String& titleIn, juce::Colour accentIn)
            {
                title  = titleIn;
                accent = accentIn;

                blend.setSliderStyle (juce::Slider::LinearHorizontal);
                blend.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                blend.setRange (0.0, 1.0, 0.0);
                blend.setValue (0.0, juce::dontSendNotification);
                blend.setColour (juce::Slider::backgroundColourId, colour::toggleOffBg());
                blend.setColour (juce::Slider::trackColourId, accent);
                blend.setColour (juce::Slider::thumbColourId, colour::white());
                blend.onValueChange = [this] { if (onBlendChanged) onBlendChanged ((float) blend.getValue()); };
                addAndMakeVisible (blend);

                details.setOnColour (accent);
                details.setPillSize (28, 15);
                details.onClick = [this]
                {
                    expanded = details.getToggleState();
                    for (auto& row : rows) row.control->setVisible (expanded);
                    if (onExpandedChanged) onExpandedChanged();
                };
                addAndMakeVisible (details);
            }

            // Continuous field -> a styled Slider (textbox shown, for exact entry).
            juce::Slider& addSliderRow (const juce::String& caption, double lo, double hi, double initial)
            {
                auto s = std::make_unique<juce::Slider>();
                s->setSliderStyle (juce::Slider::LinearHorizontal);
                s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
                s->setRange (lo, hi, 0.0);
                s->setValue (initial, juce::dontSendNotification);
                s->setColour (juce::Slider::backgroundColourId, colour::toggleOffBg());
                s->setColour (juce::Slider::trackColourId, accent);
                s->setColour (juce::Slider::thumbColourId, colour::white());
                s->setVisible (expanded);
                addAndMakeVisible (*s);
                juce::Slider& ref = *s;
                rows.push_back ({ caption, std::move (s), kRowH, {} });
                return ref;
            }

            // enum/bool field -> an RsSegmented (2 or 3 choices).
            RsSegmented& addSegmentedRow (const juce::String& caption, const juce::StringArray& labels, int initialIndex)
            {
                auto seg = std::make_unique<RsSegmented>();
                seg->setSegments (labels);
                seg->setAccent (accent);
                seg->setSelectedIndex (initialIndex, juce::dontSendNotification);
                seg->setVisible (expanded);
                addAndMakeVisible (*seg);
                RsSegmented& ref = *seg;
                rows.push_back ({ caption, std::move (seg), kRowH, {} });
                return ref;
            }

            void setBlendValue (float t) { blend.setValue ((double) t, juce::dontSendNotification); }

            int getRequiredHeight() const
            {
                int h = kPad + kTitleH + kGap + kBlendH + kBlendCapH + kGap + kDetailsH;
                if (expanded)
                    for (auto& row : rows)
                        h += kGap + row.height;
                return h + kPad;
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (kPad, 0);
                r.removeFromTop (kPad);
                titleArea = r.removeFromTop (kTitleH);
                r.removeFromTop (kGap);
                blend.setBounds (r.removeFromTop (kBlendH));
                blendCapArea = r.removeFromTop (kBlendCapH);
                r.removeFromTop (kGap);
                details.setBounds (r.removeFromTop (kDetailsH));

                if (expanded)
                {
                    for (auto& row : rows)
                    {
                        r.removeFromTop (kGap);
                        auto rr = r.removeFromTop (row.height);
                        row.captionArea = rr.removeFromLeft (kCaptionW);
                        rr.removeFromLeft (6);
                        row.control->setBounds (rr);
                    }
                }
            }

            void paint (juce::Graphics& g) override
            {
                auto full = getLocalBounds().toFloat();
                g.setColour (colour::white());
                g.fillRoundedRectangle (full, radius::box);
                g.setColour (colour::border());
                g.drawRoundedRectangle (full.reduced (0.5f), radius::box, 1.0f);

                g.setColour (colour::text());
                g.setFont (font (FontKind::Ui, 12.0f, 800, 0.03f));
                g.drawText (title, titleArea, juce::Justification::centredLeft);

                g.setFont (font (FontKind::Ui, 9.0f, 700));
                g.setColour (colour::textFaint());
                g.drawText ("v2.0.1", blendCapArea, juce::Justification::centredLeft);
                g.drawText ("Demo",   blendCapArea, juce::Justification::centredRight);

                g.setFont (font (FontKind::Ui, 10.5f, 700));
                g.setColour (colour::textSecondary());
                g.drawText ("Advanced", details.getBounds(), juce::Justification::centredLeft);

                if (expanded)
                {
                    g.setFont (font (FontKind::Ui, 10.0f, 700));
                    g.setColour (colour::textSecondary());
                    for (auto& row : rows)
                        g.drawText (row.caption, row.captionArea, juce::Justification::centredLeft);
                }
            }

            std::function<void (float)> onBlendChanged;    // fired live as the blend slider drags
            std::function<void()>       onExpandedChanged; // fired when Advanced is toggled (owner must relayout)

            juce::Slider  blend;
            RsPillToggle  details;
            bool          expanded = false;

        private:
            struct Row
            {
                juce::String caption;
                std::unique_ptr<juce::Component> control;
                int height = 0;
                juce::Rectangle<int> captionArea;
            };

            static constexpr int kPad = 10, kTitleH = 16, kGap = 6, kBlendH = 20, kBlendCapH = 12,
                                  kDetailsH = 18, kRowH = 24, kCaptionW = 84;

            juce::String title;
            juce::Colour  accent { colour::accent() };
            juce::Rectangle<int> titleArea, blendCapArea;
            std::vector<Row> rows;
        };

        // ---- scrollable body: the four FaceGroups, stacked vertically.
        // Sized by the owner (via requiredHeight()) to fit inside a
        // juce::Viewport -- see AnalyzerDevPanel::relayoutContent().
        class Content : public juce::Component
        {
        public:
            Content()
            {
                addAndMakeVisible (input);
                addAndMakeVisible (delta);
                addAndMakeVisible (curve);
                addAndMakeVisible (smooth);
            }

            int requiredHeight() const
            {
                int y = kPad;
                for (auto* f : { &input, &delta, &curve, &smooth })
                    y += f->getRequiredHeight() + kGap;
                return y + kPad - kGap;
            }

            void resized() override
            {
                int y = kPad;
                const int w = juce::jmax (0, getWidth() - kPad * 2);
                for (auto* f : { &input, &delta, &curve, &smooth })
                {
                    const int h = f->getRequiredHeight();
                    f->setBounds (kPad, y, w, h);
                    y += h + kGap;
                }
            }

            FaceGroup input, delta, curve, smooth;

        private:
            static constexpr int kPad = 12, kGap = 12;
        };

        // ---- per-row wiring helpers: build a control on `face`, seed it
        // from `field`, wire user edits back into `field` (+ notify), and
        // register a resync closure (used whenever `current` is replaced
        // wholesale -- see the class-level comment above about blend/preset/
        // setStateString resyncing every control, not just the touched face).
        void addFloatRow (FaceGroup& face, const juce::String& caption, double lo, double hi, float& field)
        {
            auto& s = face.addSliderRow (caption, lo, hi, (double) field);
            s.onValueChange = [this, &s, &field] { field = (float) s.getValue(); notifyStyleChanged(); };
            syncers.push_back ([&s, &field] { s.setValue ((double) field, juce::dontSendNotification); });
        }

        void addBoolRow (FaceGroup& face, const juce::String& caption,
                         const juce::String& v201Label, const juce::String& demoLabel,
                         bool v201Value, bool& field)
        {
            const int initial = (field == v201Value) ? 0 : 1;
            auto& seg = face.addSegmentedRow (caption, { v201Label, demoLabel }, initial);
            seg.onSelect = [this, &field, v201Value] (int idx) { field = (idx == 0 ? v201Value : ! v201Value); notifyStyleChanged(); };
            syncers.push_back ([&seg, &field, v201Value] { seg.setSelectedIndex (field == v201Value ? 0 : 1, juce::dontSendNotification); });
        }

        template <typename EnumT>
        void addEnumRow (FaceGroup& face, const juce::String& caption, const juce::StringArray& labels, EnumT& field)
        {
            auto& seg = face.addSegmentedRow (caption, labels, (int) field);
            seg.onSelect = [this, &field] (int idx) { field = (EnumT) idx; notifyStyleChanged(); };
            syncers.push_back ([&seg, &field] { seg.setSelectedIndex ((int) field, juce::dontSendNotification); });
        }

        void addColourRow (FaceGroup& face, const juce::String& caption,
                           juce::Colour v201Value, juce::Colour demoValue, juce::Colour& field)
        {
            const int initial = (field == demoValue) ? 1 : 0;
            auto& seg = face.addSegmentedRow (caption, { "v2.0.1", "Demo" }, initial);
            seg.onSelect = [this, &field, v201Value, demoValue] (int idx) { field = (idx == 0 ? v201Value : demoValue); notifyStyleChanged(); };
            syncers.push_back ([&seg, &field, v201Value] { seg.setSelectedIndex (field == v201Value ? 0 : 1, juce::dontSendNotification); });
        }

        void notifyStyleChanged() { if (onStyleChanged) onStyleChanged (current); }

        void relayoutContent();
        void recomposeAll();
        void syncAllControlsFromCurrent();
        void syncPresetHighlightFromState();
        void buildInputRows();
        void buildDeltaRows();
        void buildCurveRows();
        void buildSmoothRows();

        float tInput = 0.0f, tDelta = 0.0f, tCurve = 0.0f, tSmooth = 0.0f;
        AnalyzerStyle current = kV201Style;
        std::vector<std::function<void()>> syncers;

        RsLookAndFeel lnf;
        CloseButton   closeButton;
        RsSegmented   presetSelector;
        juce::Rectangle<int> titleArea;
        juce::Viewport viewport;
        Content content;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerDevPanel)
    };

    // ---------------------------------------------------------------- ctor
    inline AnalyzerDevPanel::AnalyzerDevPanel()
    {
        setLookAndFeel (&lnf);

        addAndMakeVisible (closeButton);
        closeButton.onClick = [this] { if (onClose) onClose(); };

        presetSelector.setSegments ({ "v2.0.1", "50/50", "Demo" });
        presetSelector.setAccent (colour::accent());
        presetSelector.setSelectedIndex (0, juce::dontSendNotification);
        presetSelector.onSelect = [this] (int idx)
        {
            const float t = (idx <= 0) ? 0.0f : (idx == 1 ? 0.5f : 1.0f);
            tInput = tDelta = tCurve = tSmooth = t;
            recomposeAll();
        };
        addAndMakeVisible (presetSelector);

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);

        content.input.configure  ("INPUT",  colour::orange());
        content.delta.configure  ("DELTA",  colour::teal());
        content.curve.configure  ("CURVE",  colour::accent());
        content.smooth.configure ("SMOOTH", colour::mint());

        content.input.onBlendChanged  = [this] (float t) { tInput  = t; recomposeAll(); };
        content.delta.onBlendChanged  = [this] (float t) { tDelta  = t; recomposeAll(); };
        content.curve.onBlendChanged  = [this] (float t) { tCurve  = t; recomposeAll(); };
        content.smooth.onBlendChanged = [this] (float t) { tSmooth = t; recomposeAll(); };

        for (auto* f : { &content.input, &content.delta, &content.curve, &content.smooth })
            f->onExpandedChanged = [this] { relayoutContent(); };

        buildInputRows();
        buildDeltaRows();
        buildCurveRows();
        buildSmoothRows();

        setSize (kWidth, kHeight);
    }

    inline AnalyzerDevPanel::~AnalyzerDevPanel()
    {
        setLookAndFeel (nullptr);
    }

    inline void AnalyzerDevPanel::paint (juce::Graphics& g)
    {
        auto full = getLocalBounds().toFloat();
        g.setColour (colour::white().withAlpha (0.97f));
        g.fillRoundedRectangle (full, radius::popover);
        g.setColour (colour::border());
        g.drawRoundedRectangle (full.reduced (0.5f), radius::popover, 1.0f);

        g.setColour (colour::text());
        g.setFont (font (FontKind::Display, 13.0f, 800, 0.04f));
        g.drawText ("ANALYZER DEV", titleArea, juce::Justification::centredLeft);
    }

    inline void AnalyzerDevPanel::resized()
    {
        auto full = getLocalBounds();
        auto r = full.reduced (14, 10);

        auto headerRow = r.removeFromTop (26);
        closeButton.setBounds (headerRow.removeFromRight (20).withSizeKeepingCentre (18, 18));
        headerRow.removeFromRight (8);
        titleArea = headerRow.removeFromLeft (104);
        headerRow.removeFromLeft (10);
        presetSelector.setBounds (headerRow);

        r.removeFromTop (10);
        viewport.setBounds (r);
        relayoutContent();
    }

    inline void AnalyzerDevPanel::relayoutContent()
    {
        const int w = juce::jmax (100, viewport.getWidth() - viewport.getScrollBarThickness());
        content.setSize (w, content.requiredHeight());
        content.resized();
    }

    inline void AnalyzerDevPanel::recomposeAll()
    {
        current = composePerFace (tInput, tDelta, tCurve, tSmooth);
        syncAllControlsFromCurrent();
        notifyStyleChanged();
    }

    inline void AnalyzerDevPanel::syncAllControlsFromCurrent()
    {
        content.input.setBlendValue  (tInput);
        content.delta.setBlendValue  (tDelta);
        content.curve.setBlendValue  (tCurve);
        content.smooth.setBlendValue (tSmooth);
        for (auto& sync : syncers) sync();
        syncPresetHighlightFromState();
    }

    inline void AnalyzerDevPanel::syncPresetHighlightFromState()
    {
        auto approxEqual = [] (float a, float b) { return std::abs (a - b) < 0.001f; };
        int idx = -1;
        if (approxEqual (tInput, tDelta) && approxEqual (tDelta, tCurve) && approxEqual (tCurve, tSmooth))
        {
            if      (approxEqual (tInput, 0.0f)) idx = 0;
            else if (approxEqual (tInput, 0.5f)) idx = 1;
            else if (approxEqual (tInput, 1.0f)) idx = 2;
        }
        presetSelector.setSelectedIndex (idx, juce::dontSendNotification);
    }

    inline juce::String AnalyzerDevPanel::getStateString() const
    {
        auto* obj = new juce::DynamicObject();
        juce::var v (obj);

        obj->setProperty ("version", 1);
        obj->setProperty ("tInput",  tInput);
        obj->setProperty ("tDelta",  tDelta);
        obj->setProperty ("tCurve",  tCurve);
        obj->setProperty ("tSmooth", tSmooth);

        obj->setProperty ("inputFillTopAlpha", current.inputFillTopAlpha);
        obj->setProperty ("inputFillBotAlpha", current.inputFillBotAlpha);
        obj->setProperty ("inputColour",       current.inputColour.toString());
        obj->setProperty ("inputLineWidth",    current.inputLineWidth);
        obj->setProperty ("inputLineAlpha",    current.inputLineAlpha);

        obj->setProperty ("postColour",    current.postColour.toString());
        obj->setProperty ("postLineWidth", current.postLineWidth);
        obj->setProperty ("postLineAlpha", current.postLineAlpha);

        obj->setProperty ("deltaMode",        (int) current.deltaMode);
        obj->setProperty ("deltaFillAlpha",   current.deltaFillAlpha);
        obj->setProperty ("deltaStrokeAlpha", current.deltaStrokeAlpha);
        obj->setProperty ("deltaStrokeWidth", current.deltaStrokeWidth);
        obj->setProperty ("deltaClampFrac",   current.deltaClampFrac);

        obj->setProperty ("curveMode",           (int) current.curveMode);
        obj->setProperty ("perNodeFillAlpha",    current.perNodeFillAlpha);
        obj->setProperty ("perNodeStrokeAlpha",  current.perNodeStrokeAlpha);
        obj->setProperty ("combinedGlowAlpha",   current.combinedGlowAlpha);
        obj->setProperty ("combinedGlowWidth",   current.combinedGlowWidth);
        obj->setProperty ("combinedStrokeWidth", current.combinedStrokeWidth);
        obj->setProperty ("combinedStrokeAlpha", current.combinedStrokeAlpha);
        obj->setProperty ("combinedDashLen",     current.combinedDashLen);
        obj->setProperty ("combinedDashGap",     current.combinedDashGap);
        obj->setProperty ("combinedColour",      current.combinedColour.toString());

        obj->setProperty ("tempoSmoothingMs",  current.tempoSmoothingMs);
        obj->setProperty ("freqSmoothingOct",  current.freqSmoothingOct);
        obj->setProperty ("pathProfileCurved", current.pathProfileCurved);
        obj->setProperty ("traceRoundJoin",    current.traceRoundJoin);

        return juce::JSON::toString (v, true);
    }

    inline void AnalyzerDevPanel::setStateString (const juce::String& s)
    {
        auto parsed = juce::JSON::parse (s);
        auto* obj = parsed.getDynamicObject();

        auto getNum = [&] (const char* key, double fallback) -> double
        {
            if (obj == nullptr) return fallback;
            const auto& v = obj->getProperty (key);
            return v.isVoid() ? fallback : static_cast<double> (v);
        };
        auto getBool = [&] (const char* key, bool fallback) -> bool
        {
            if (obj == nullptr) return fallback;
            const auto& v = obj->getProperty (key);
            return v.isVoid() ? fallback : static_cast<bool> (v);
        };
        auto getColour = [&] (const char* key, juce::Colour fallback) -> juce::Colour
        {
            if (obj == nullptr) return fallback;
            const auto& v = obj->getProperty (key);
            return v.isString() ? juce::Colour::fromString (v.toString()) : fallback;
        };

        tInput  = juce::jlimit (0.0f, 1.0f, (float) getNum ("tInput",  0.0));
        tDelta  = juce::jlimit (0.0f, 1.0f, (float) getNum ("tDelta",  0.0));
        tCurve  = juce::jlimit (0.0f, 1.0f, (float) getNum ("tCurve",  0.0));
        tSmooth = juce::jlimit (0.0f, 1.0f, (float) getNum ("tSmooth", 0.0));

        AnalyzerStyle s2 = kV201Style;
        s2.inputFillTopAlpha = (float) getNum ("inputFillTopAlpha", (double) kV201Style.inputFillTopAlpha);
        s2.inputFillBotAlpha = (float) getNum ("inputFillBotAlpha", (double) kV201Style.inputFillBotAlpha);
        s2.inputColour       = getColour ("inputColour", kV201Style.inputColour);
        s2.inputLineWidth    = (float) getNum ("inputLineWidth", (double) kV201Style.inputLineWidth);
        s2.inputLineAlpha    = (float) getNum ("inputLineAlpha", (double) kV201Style.inputLineAlpha);

        s2.postColour    = getColour ("postColour", kV201Style.postColour);
        s2.postLineWidth = (float) getNum ("postLineWidth", (double) kV201Style.postLineWidth);
        s2.postLineAlpha = (float) getNum ("postLineAlpha", (double) kV201Style.postLineAlpha);

        s2.deltaMode = static_cast<AnalyzerStyle::DeltaMode> (
            juce::jlimit (0, 1, (int) getNum ("deltaMode", (double) static_cast<int> (kV201Style.deltaMode))));
        s2.deltaFillAlpha   = (float) getNum ("deltaFillAlpha", (double) kV201Style.deltaFillAlpha);
        s2.deltaStrokeAlpha = (float) getNum ("deltaStrokeAlpha", (double) kV201Style.deltaStrokeAlpha);
        s2.deltaStrokeWidth = (float) getNum ("deltaStrokeWidth", (double) kV201Style.deltaStrokeWidth);
        s2.deltaClampFrac   = (float) getNum ("deltaClampFrac", (double) kV201Style.deltaClampFrac);

        s2.curveMode = static_cast<AnalyzerStyle::CurveMode> (
            juce::jlimit (0, 2, (int) getNum ("curveMode", (double) static_cast<int> (kV201Style.curveMode))));
        s2.perNodeFillAlpha    = (float) getNum ("perNodeFillAlpha", (double) kV201Style.perNodeFillAlpha);
        s2.perNodeStrokeAlpha  = (float) getNum ("perNodeStrokeAlpha", (double) kV201Style.perNodeStrokeAlpha);
        s2.combinedGlowAlpha   = (float) getNum ("combinedGlowAlpha", (double) kV201Style.combinedGlowAlpha);
        s2.combinedGlowWidth   = (float) getNum ("combinedGlowWidth", (double) kV201Style.combinedGlowWidth);
        s2.combinedStrokeWidth = (float) getNum ("combinedStrokeWidth", (double) kV201Style.combinedStrokeWidth);
        s2.combinedStrokeAlpha = (float) getNum ("combinedStrokeAlpha", (double) kV201Style.combinedStrokeAlpha);
        s2.combinedDashLen     = (float) getNum ("combinedDashLen", (double) kV201Style.combinedDashLen);
        s2.combinedDashGap     = (float) getNum ("combinedDashGap", (double) kV201Style.combinedDashGap);
        s2.combinedColour      = getColour ("combinedColour", kV201Style.combinedColour);

        s2.tempoSmoothingMs  = (float) getNum ("tempoSmoothingMs", (double) kV201Style.tempoSmoothingMs);
        s2.freqSmoothingOct  = (float) getNum ("freqSmoothingOct", (double) kV201Style.freqSmoothingOct);
        s2.pathProfileCurved = getBool ("pathProfileCurved", kV201Style.pathProfileCurved);
        s2.traceRoundJoin    = getBool ("traceRoundJoin", kV201Style.traceRoundJoin);

        current = s2;
        syncAllControlsFromCurrent();
        notifyStyleChanged();
    }

    inline void AnalyzerDevPanel::buildInputRows()
    {
        auto& f = content.input;
        addFloatRow  (f, "Fill Top",   0.0, 1.0, current.inputFillTopAlpha);
        addFloatRow  (f, "Fill Bot",   0.0, 1.0, current.inputFillBotAlpha);
        addFloatRow  (f, "Line Width", 0.0, 4.0, current.inputLineWidth);
        addFloatRow  (f, "Line Alpha", 0.0, 1.0, current.inputLineAlpha);
        addFloatRow  (f, "Post Width", 0.0, 4.0, current.postLineWidth);
        addFloatRow  (f, "Post Alpha", 0.0, 1.0, current.postLineAlpha);
        addColourRow (f, "Colour", kV201Style.inputColour, kDemoStyle.inputColour, current.inputColour);
    }

    inline void AnalyzerDevPanel::buildDeltaRows()
    {
        auto& f = content.delta;
        addEnumRow  (f, "Mode", { "0dB", "Top" }, current.deltaMode);
        addFloatRow (f, "Fill Alpha",   0.0, 1.0, current.deltaFillAlpha);
        addFloatRow (f, "Stroke Alpha", 0.0, 1.0, current.deltaStrokeAlpha);
        addFloatRow (f, "Stroke Width", 0.0, 4.0, current.deltaStrokeWidth);
        addFloatRow (f, "Clamp",        0.0, 1.0, current.deltaClampFrac);
    }

    inline void AnalyzerDevPanel::buildCurveRows()
    {
        auto& f = content.curve;
        addEnumRow   (f, "Mode", { "Node+Cmb", "CombOnly", "NodeOnly" }, current.curveMode);
        addFloatRow  (f, "Node Fill",    0.0, 1.0,  current.perNodeFillAlpha);
        addFloatRow  (f, "Node Stroke",  0.0, 1.0,  current.perNodeStrokeAlpha);
        addFloatRow  (f, "Glow Alpha",   0.0, 1.0,  current.combinedGlowAlpha);
        addFloatRow  (f, "Glow Width",   0.0, 10.0, current.combinedGlowWidth);
        addFloatRow  (f, "Stroke Width", 0.0, 6.0,  current.combinedStrokeWidth);
        addFloatRow  (f, "Stroke Alpha", 0.0, 1.0,  current.combinedStrokeAlpha);
        addFloatRow  (f, "Dash Len",     0.0, 12.0, current.combinedDashLen);
        addFloatRow  (f, "Dash Gap",     0.0, 12.0, current.combinedDashGap);
        addColourRow (f, "Colour", kV201Style.combinedColour, kDemoStyle.combinedColour, current.combinedColour);
    }

    inline void AnalyzerDevPanel::buildSmoothRows()
    {
        auto& f = content.smooth;
        addFloatRow (f, "Tempo (ms)", 0.0, 200.0, current.tempoSmoothingMs);
        addFloatRow (f, "Freq (oct)", 0.0, 1.0,   current.freqSmoothingOct);
        addBoolRow  (f, "Path", "Curved", "Straight", kV201Style.pathProfileCurved, current.pathProfileCurved);
        addBoolRow  (f, "Join", "Miter",  "Round",    kV201Style.traceRoundJoin,    current.traceRoundJoin);
    }
} // namespace rs
