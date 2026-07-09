#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "RsTheme.h"
#include "RsLookAndFeel.h"
#include "RsWidgets.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <array>
#include <cmath>
#include <memory>

//
// A small, bright inline editor for the currently-selected reduction node, shown
// as a floating popover over the analyser (bottom-centre) when a node is
// selected. It rebinds its APVTS attachments to that node on selection
// (Pro-Q-style). Demo-style layout (Phase P2b, rs-ui-work/demo-analysis.md
// SS2.6): a header row (band/cut dot + name + ON badge + Listen badge, with a
// close X pinned to the card's top-right corner) above a TYPE row (6 filter-
// type icon buttons, bands) or SLOPE row (6/12/24/48 dB/oct, cuts), with the
// FREQ [+ SENS [+ WIDTH]] mini-knobs filling the card's right column (SENS/
// WIDTH only for bands). Frequencies read in kHz at/above 1 kHz. Uses
// RsLookAndFeel (demo rotary look) + the rs:: theme (RsTheme.h) throughout;
// the badges/buttons are bespoke juce::Button subclasses local to this file
// (RsWidgets.h is out of scope for this phase -- same hand-painted idiom as
// its RsPillToggle/RsSegmented/RsIconButton). GUI-thread only.
//
// Phase 5a-2: Listen (processor.setListenNode, NOT an APVTS parameter -- see
// PluginProcessor.h) solos this node's removed signal while its toggle is on.
// The toggle is re-synced from the processor on setNode() (a fresh selection
// always starts with Listen off -- see SuppressionCurveComponent::selectNode)
// and periodically via refreshListenState() so a Listen change made elsewhere
// (the curve's right-click menu) stays reflected while the panel is open.
//
class NodePanel : public juce::Component
{
public:
    NodePanel (ResonanceSuppressorAudioProcessor& proc, juce::AudioProcessorValueTreeState& s)
        : processor (proc), apvts (s)
    {
        setLookAndFeel (&lnf); // demo-style rotary for the FREQ/SENS/WIDTH mini-knobs

        // Off'ing this band while it is soloed must drop Listen, or processBlock
        // keeps soloing a now-hidden, nominal-profile node (silent dead state).
        // The APVTS ButtonAttachment already wrote the new On state by the time
        // onClick fires, so getToggleState() reads the post-click value.
        onButton.setClickingTogglesState (true);
        onButton.onClick = [this]
        {
            if (! onButton.getToggleState() && processor.getListenNode() == nodeId)
            {
                processor.setListenNode (-1);
                refreshListenState();
            }
        };
        addAndMakeVisible (onButton);

        listenButton.setClickingTogglesState (true);
        listenButton.setTooltip ("Solo this node's removed signal (output = exactly what it cuts).");
        listenButton.onClick = [this]
        {
            processor.setListenNode (listenButton.getToggleState() ? nodeId : -1);
        };
        addAndMakeVisible (listenButton);

        closeButton.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible (closeButton);

        // Hidden value model (ComboBoxAttachment target) driving the visible
        // TYPE icon / SLOPE label buttons below -- same "hidden combo" pattern
        // as RsWidgets.h's RsSegmented.
        addChildComponent (choiceBox);
        choiceBox.onChange = [this] { syncChoiceButtons(); };

        static const char* kTypeNames[] = { "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" };
        for (int i = 0; i < (int) typeButtons.size(); ++i)
        {
            auto& b = typeButtons[(size_t) i];
            b.setGlyph (eqGlyph (i));
            b.setTooltip (kTypeNames[i]);
            b.onClick = [this, i] { choiceBox.setSelectedItemIndex (i, juce::sendNotificationSync); };
            addAndMakeVisible (b);
        }
        static const char* kSlopeLabels[] = { "6", "12", "24", "48" };
        static const char* kSlopeTips[]   = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
        for (int i = 0; i < (int) slopeButtons.size(); ++i)
        {
            auto& b = slopeButtons[(size_t) i];
            b.setLabelText (kSlopeLabels[i]);
            b.setTooltip (kSlopeTips[i]);
            b.onClick = [this, i] { choiceBox.setSelectedItemIndex (i, juce::sendNotificationSync); };
            addAndMakeVisible (b);
        }

        freqKnob.setup  ("FREQ",  rs::colour::orange(), false, {});
        sensKnob.setup  ("SENS",  rs::colour::orange(), false, nbsp() + "dB");
        widthKnob.setup ("WIDTH", rs::colour::orange(), false, nbsp() + "oct");
        addAndMakeVisible (freqKnob);
        addAndMakeVisible (sensKnob);
        addAndMakeVisible (widthKnob);
    }

    ~NodePanel() override { setLookAndFeel (nullptr); }

    // ✕ -> the owner deselects (SuppressionCurveComponent::selectNode(-1),
    // which also drops Listen); wired one level up, see that file's constructor.
    std::function<void()> onCloseRequested;

    int  currentNode()    const noexcept { return nodeId; }
    bool isCutNode()      const noexcept { return isCut; }
    int  preferredWidth() const noexcept { return isCut ? 350 : 500; } // + Listen badge, bands + SENS/WIDTH knobs
    static constexpr int kHeight = 112;

    // Rebind to node `id` (0 = low cut, 1 = high cut, 2.. = band). Drops the old
    // attachments first so syncing the new ones can't write back to the old node.
    void setNode (int id)
    {
        nodeId = id;
        isCut  = (id < 2);

        onAtt.reset(); choiceAtt.reset(); freqAtt.reset(); sensAtt.reset(); widthAtt.reset();

        nameText  = nodeName();
        accentCol = nodeColour();

        choiceBox.clear (juce::dontSendNotification);
        if (isCut) choiceBox.addItemList ({ "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 1);
        else       choiceBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" }, 1);

        for (auto& b : typeButtons)  b.setVisible (! isCut);
        for (auto& b : slopeButtons) b.setVisible (isCut);
        sensKnob.setVisible (! isCut);
        widthKnob.setVisible (! isCut);

        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        onAtt     = std::make_unique<BA> (apvts, pid ("on"),                     onButton);
        choiceAtt = std::make_unique<CA> (apvts, pid (isCut ? "slope" : "type"), choiceBox);
        freqAtt   = std::make_unique<SA> (apvts, pid ("freq"),                   freqKnob.slider());
        if (! isCut)
        {
            sensAtt  = std::make_unique<SA> (apvts, pid ("sens"),  sensKnob.slider());
            widthAtt = std::make_unique<SA> (apvts, pid ("width"), widthKnob.slider());
        }

        // Freq reads in kHz at/above 1 kHz, Hz below. Must run after the
        // attachment, which installs its own textFromValueFunction (see #26).
        freqKnob.slider().textFromValueFunction = [] (double v)
        {
            if (v >= 1000.0)
            {
                const double k = v / 1000.0;
                const bool whole = std::abs (k - std::round (k)) < 0.05;
                return (whole ? juce::String ((int) std::round (k)) : juce::String (k, 1)) + nbsp() + "kHz";
            }
            return juce::String (juce::roundToInt (v)) + nbsp() + "Hz";
        };
        freqKnob.slider().setTextValueSuffix ({}); // the lambda already prints Hz/kHz — avoid a doubled suffix
        freqKnob.slider().updateText();
        if (! isCut)
        {
            factory_ui::setSliderDecimals (sensKnob.slider(), 2);  // dB to 2 dp
            factory_ui::setSliderDecimals (widthKnob.slider(), 2); // oct to 2 dp (#23)
        }

        syncChoiceButtons();
        refreshListenState();

        resized();
        repaint();
    }

    // Sync the Listen toggle to the processor's live Listen target. Called once
    // from setNode() (a fresh selection always reads Listen off — the curve
    // already dropped it before rebinding, see selectNode()) and on a cadence
    // from the curve's timer so a menu-driven Listen change is reflected while
    // this panel stays open on the same node.
    void refreshListenState()
    {
        listenButton.setToggleState (processor.getListenNode() == nodeId, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        auto full = getLocalBounds().toFloat();
        g.setColour (rs::colour::white().withAlpha (0.97f)); // near-opaque (backdrop-blur has no Graphics equivalent)
        g.fillRoundedRectangle (full, rs::radius::popover);
        g.setColour (rs::colour::border());
        g.drawRoundedRectangle (full.reduced (0.5f), rs::radius::popover, 1.0f);

        // Header dot: node identity colour, with the demo's pale ring (SS2.6/3.5).
        g.setColour (kDotRing);
        g.fillEllipse (dotBounds.expanded (3.0f));
        g.setColour (accentCol);
        g.fillEllipse (dotBounds);

        g.setColour (rs::colour::text());
        g.setFont (rs::font (rs::FontKind::Display, 15.0f, 800));
        g.drawText (nameText, nameArea, juce::Justification::centredLeft);

        g.setColour (kTypeCaption);
        g.setFont (rs::font (rs::FontKind::Ui, 10.0f, 800, 0.08f));
        g.drawText (isCut ? "SLOPE" : "TYPE", captionArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto full = getLocalBounds();
        closeButton.setBounds (full.getRight() - 28, 8, 18, 18);

        auto r = full.reduced (14, 12);

        // Right: FREQ (+ SENS + WIDTH) mini-knobs. Trim clear of the close
        // button's corner first (top-right, right column only -- the left
        // column below never reaches that far right, see the width budget).
        constexpr int knobW = 52, kgap = 10;
        auto knobs = r.removeFromRight (isCut ? knobW : knobW * 3 + kgap * 2);
        r.removeFromRight (16); // gap before the knobs
        knobs.removeFromTop (18);

        // Left column, row 1: dot + name + ON badge + Listen badge.
        auto headerRow = r.removeFromTop (26);
        auto dotCell = headerRow.removeFromLeft (18);
        dotBounds = dotCell.toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        headerRow.removeFromLeft (4);
        nameArea = headerRow.removeFromLeft (76);
        headerRow.removeFromLeft (8);
        onButton.setBounds (headerRow.removeFromLeft (40).withSizeKeepingCentre (40, 22));
        headerRow.removeFromLeft (6);
        const int listenW = juce::jmin (90, headerRow.getWidth());
        listenButton.setBounds (headerRow.removeFromLeft (listenW).withSizeKeepingCentre (listenW, 22));

        r.removeFromTop (18);

        // Left column, row 2: TYPE (6 icons, bands) or SLOPE (4 labels, cuts).
        auto typeRow = r.removeFromTop (30);
        captionArea = typeRow.removeFromLeft (isCut ? 52 : 38);
        typeRow.removeFromLeft (8);
        if (isCut)
        {
            constexpr int bw = 40, bgap = 4;
            for (auto& b : slopeButtons)
            {
                b.setBounds (typeRow.removeFromLeft (bw).withSizeKeepingCentre (bw, 27));
                typeRow.removeFromLeft (bgap);
            }
        }
        else
        {
            constexpr int bw = 32, bgap = 4;
            for (auto& b : typeButtons)
            {
                b.setBounds (typeRow.removeFromLeft (bw).withSizeKeepingCentre (bw, 27));
                typeRow.removeFromLeft (bgap);
            }
        }

        auto col = [] (juce::Rectangle<int> c, rs::RsKnob& k) { k.setBounds (c); };
        if (isCut) col (knobs, freqKnob);
        else
        {
            col (knobs.removeFromLeft (knobW), freqKnob);
            knobs.removeFromLeft (kgap);
            col (knobs.removeFromLeft (knobW), sensKnob);
            knobs.removeFromLeft (kgap);
            col (knobs, widthKnob);
        }
    }

private:
    // -- bespoke popover widgets, local to this file (RsWidgets.h is out of
    // scope for this phase) -- same hand-painted juce::Button idiom as
    // RsWidgets.h's RsPillToggle / RsSegmented / RsIconButton.

    // ON/OFF state pill (ButtonAttachment target): teal + white "ON" vs
    // cream + border + muted "OFF" (demo-analysis SS2.6).
    struct OnBadge : juce::Button
    {
        OnBadge() : juce::Button ({}) {}
        void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
        {
            auto r = getLocalBounds().toFloat();
            const bool on = getToggleState();
            g.setColour (on ? rs::colour::teal() : rs::colour::footerBg());
            g.fillRoundedRectangle (r, rs::radius::badge);
            if (! on)
            {
                g.setColour (rs::colour::border());
                g.drawRoundedRectangle (r.reduced (0.5f), rs::radius::badge, 1.0f);
            }
            if (highlighted)
            {
                g.setColour (juce::Colours::white.withAlpha (on ? 0.15f : 0.5f));
                g.fillRoundedRectangle (r, rs::radius::badge);
            }
            g.setColour (on ? rs::colour::white() : rs::colour::textSecondary());
            g.setFont (rs::font (rs::FontKind::Ui, 11.0f, 800));
            g.drawText (on ? "ON" : "OFF", r, juce::Justification::centred);
        }
    };

    // Listen (solo the removed signal) pill -- not APVTS-attached, driven by
    // processor.setListenNode via onClick (see the constructor and the Phase
    // 5a-2 header comment above). Active = teal, matching the curve's Listen
    // ring (SuppressionCurveComponent::drawNodes).
    struct ListenBadge : juce::Button
    {
        ListenBadge() : juce::Button ({}) {}
        void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
        {
            auto r = getLocalBounds().toFloat();
            const bool on = getToggleState();
            g.setColour (on ? rs::colour::teal() : rs::colour::footerBg());
            g.fillRoundedRectangle (r, rs::radius::badge);
            g.setColour (on ? rs::colour::teal() : rs::colour::border());
            g.drawRoundedRectangle (r.reduced (0.5f), rs::radius::badge, 1.0f);
            if (highlighted)
            {
                g.setColour (juce::Colours::white.withAlpha (on ? 0.15f : 0.5f));
                g.fillRoundedRectangle (r, rs::radius::badge);
            }
            auto inner = r.reduced (8.0f, 0.0f);
            g.setColour (on ? rs::colour::white() : rs::colour::textSecondary());
            g.fillEllipse (inner.removeFromLeft (10.0f).withSizeKeepingCentre (7.0f, 7.0f));
            inner.removeFromLeft (4.0f);
            g.setFont (rs::font (rs::FontKind::Ui, 11.0f, 800));
            g.drawText ("Listen", inner, juce::Justification::centredLeft);
        }
    };

    // A single TYPE icon (bands) or SLOPE label (cuts) button, radio-style:
    // NodePanel drives the active flag from choiceBox's selected index (see
    // syncChoiceButtons()), not the button's own click-toggle machinery. Active
    // fill is the demo's fixed popover accent (#ff9472); the header dot instead
    // carries the node's own identity colour (see nodeColour()).
    struct ChoiceButton : juce::Button
    {
        ChoiceButton() : juce::Button ({}) {}
        void setGlyph (juce::Path p)              { glyph = std::move (p); hasGlyph = true; }
        void setLabelText (const juce::String& t) { label = t; hasGlyph = false; }

        void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
        {
            auto r = getLocalBounds().toFloat();
            const bool active = getToggleState();
            g.setColour (active ? rs::colour::orange() : rs::colour::footerBg());
            g.fillRoundedRectangle (r, 8.0f);
            if (! active)
            {
                g.setColour (rs::colour::border());
                g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
            }
            if (highlighted)
            {
                g.setColour (juce::Colours::white.withAlpha (active ? 0.15f : 0.5f));
                g.fillRoundedRectangle (r, 8.0f);
            }
            const auto fg = active ? rs::colour::white() : rs::colour::iconInactive();
            if (hasGlyph)
            {
                constexpr float boxW = 24.0f, boxH = 14.0f;
                auto area = r.reduced (6.0f, 7.0f);
                const float sc = juce::jmin (area.getWidth() / boxW, area.getHeight() / boxH);
                auto p = glyph;
                p.applyTransform (juce::AffineTransform::scale (sc)
                                      .translated (area.getCentreX() - boxW * sc * 0.5f,
                                                   area.getCentreY() - boxH * sc * 0.5f));
                g.setColour (fg);
                g.strokePath (p, juce::PathStrokeType (2.2f * sc, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            else
            {
                g.setColour (fg);
                g.setFont (rs::font (rs::FontKind::Ui, 11.0f, 800));
                g.drawText (label, r, juce::Justification::centred);
            }
        }

    private:
        juce::Path glyph;
        juce::String label;
        bool hasGlyph = true;
    };

    // ✕ close -> onCloseRequested() (deselect; wired one level up in
    // SuppressionCurveComponent's constructor).
    struct CloseButton : juce::Button
    {
        CloseButton() : juce::Button ({}) {}
        void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
        {
            auto r = getLocalBounds().toFloat().reduced (highlighted ? 4.0f : 5.0f);
            g.setColour (rs::colour::textFaint());
            juce::Path x;
            x.startNewSubPath (r.getTopLeft());    x.lineTo (r.getBottomRight());
            x.startNewSubPath (r.getBottomLeft()); x.lineTo (r.getTopRight());
            g.strokePath (x, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    };

    // The 6 filter-type glyphs, verbatim from demo-analysis SS3.6 (viewBox
    // 0 0 24 14; SVG "C x1 y1,x2 y2,x y" -> cubicTo(x1,y1,x2,y2,x,y), same
    // convention as RsWidgets.h). rs::icons::paintGlyph assumes a square
    // viewBox (RsWidgets.h is out of scope this phase anyway), so these are
    // painted directly by ChoiceButton::paintButton above instead.
    static juce::Path eqGlyph (int typeIndex)
    {
        juce::Path p;
        switch (typeIndex)
        {
            case 0: // Bell
                p.startNewSubPath (2, 12); p.cubicTo (8, 12, 8, 3, 12, 3); p.cubicTo (16, 3, 16, 12, 22, 12);
                break;
            case 1: // Low Shelf
                p.startNewSubPath (2, 4); p.lineTo (9, 4); p.cubicTo (12, 4, 12, 11, 15, 11); p.lineTo (22, 11);
                break;
            case 2: // High Shelf
                p.startNewSubPath (2, 11); p.lineTo (9, 11); p.cubicTo (12, 11, 12, 4, 15, 4); p.lineTo (22, 4);
                break;
            case 3: // Band Shelf
                p.startNewSubPath (2, 11); p.lineTo (6, 11); p.cubicTo (8, 11, 8, 4, 10, 4); p.lineTo (14, 4);
                p.cubicTo (16, 4, 16, 11, 18, 11); p.lineTo (22, 11);
                break;
            case 4: // Band Reject
                p.startNewSubPath (2, 4); p.lineTo (8, 4); p.cubicTo (10, 4, 10, 12, 12, 12); p.cubicTo (14, 12, 14, 4, 16, 4); p.lineTo (22, 4);
                break;
            default: // 5: Tilt
                p.startNewSubPath (3, 12); p.lineTo (21, 4);
                break;
        }
        return p;
    }

    // Non-breaking space: RsKnob::paint() strips plain " " from the value text
    // it reads back (factory tightens "62 %" -> "62%" for the footer knobs), but
    // the demo's FREQ/SENS/WIDTH values keep a number/unit space ("2.6 kHz"),
    // so the unit suffixes below use this instead of " " to survive that strip.
    static juce::String nbsp() { return juce::String::charToString (juce::juce_wchar (0x00A0)); }

    juce::String pid (const char* s) const
    {
        return isCut ? ResonanceSuppressorAudioProcessor::cutPid  (nodeId, s)
                     : ResonanceSuppressorAudioProcessor::bandPid (nodeId - 2, s);
    }
    juce::String nodeName() const
    {
        return isCut ? (nodeId == 0 ? "Low Cut" : "High Cut")
                     : "Band " + juce::String (nodeId - 1);
    }
    // The header dot's identity colour: LC/HC use the same ring colours as the
    // plot's cut handles (SuppressionCurveComponent::drawNodes), bands use the
    // shared per-band palette (also what colours the node dot on the curve).
    juce::Colour nodeColour() const
    {
        return isCut ? (nodeId == 0 ? rs::colour::orange() : kHighCutRing)
                     : FactoryLookAndFeel::bandColour (nodeId - 2);
    }

    // Reflect choiceBox's selected index onto both button rows (harmless to
    // also set the currently-hidden row -- see setNode()).
    void syncChoiceButtons()
    {
        const int sel = choiceBox.getSelectedItemIndex();
        for (int i = 0; i < (int) typeButtons.size(); ++i)
            typeButtons[(size_t) i].setToggleState (i == sel, juce::dontSendNotification);
        for (int i = 0; i < (int) slopeButtons.size(); ++i)
            slopeButtons[(size_t) i].setToggleState (i == sel, juce::dontSendNotification);
        repaint();
    }

    // A few demo hex values (demo-analysis SS1.3/SS2.6) with no rs::colour role
    // of their own (RsTheme.h is out of scope for this phase) -- kept local,
    // same pattern as SuppressionCurveComponent's kHighCutRing/kGrBg/etc.
    inline static const juce::Colour kHighCutRing { juce::Colour (0xff79b8efu) }; // HC dot -- matches the plot's HC ring
    inline static const juce::Colour kDotRing     { juce::Colour (0xffffe4dbu) }; // header dot's pale ring
    inline static const juce::Colour kTypeCaption { juce::Colour (0xffc39a8cu) }; // "TYPE"/"SLOPE" caption

    RsLookAndFeel lnf;
    ResonanceSuppressorAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    int  nodeId = 0;
    bool isCut  = true;
    juce::String nameText;
    juce::Colour accentCol { FactoryLookAndFeel::bandColour (0) }; // node identity colour (header dot); set for real in setNode()
    juce::Rectangle<int> nameArea, captionArea;
    juce::Rectangle<float> dotBounds;

    OnBadge     onButton;
    ListenBadge listenButton;
    CloseButton closeButton;
    juce::ComboBox choiceBox;
    std::array<ChoiceButton, 6> typeButtons;  // Bell/LowShelf/HighShelf/BandShelf/BandReject/Tilt (bands)
    std::array<ChoiceButton, 4> slopeButtons; // 6/12/24/48 dB/oct (cuts)
    rs::RsKnob freqKnob, sensKnob, widthKnob;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   onAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> choiceAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   freqAtt, sensAtt, widthAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodePanel)
};
