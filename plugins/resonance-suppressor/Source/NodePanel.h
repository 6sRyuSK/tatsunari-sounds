#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <cmath>
#include <memory>

//
// A small, bright inline editor for the currently-selected reduction node, shown
// over the analyser (bottom-centre) when a node is selected. It rebinds its APVTS
// attachments to that node on selection (Pro-Q-style). Layout: On + Listen
// (top-left/top-right) with the node name painted between them, the slope/type
// selector below, and the Freq [+ Sens [+ Width]] knobs filling the full height
// on the right (Width only for bands). Frequencies read in kHz at/above 1 kHz.
// Horizontal "kawaii" card styling (factory_ui). GUI-thread only.
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
        setLookAndFeel (&lnf); // knob captions / value boxes / combo text at 11 px

        onButton.setButtonText ("On");
        // Off'ing this band while it is soloed must drop Listen, or processBlock
        // keeps soloing a now-hidden, nominal-profile node (silent dead state).
        // The APVTS ButtonAttachment already wrote the new On state by the time
        // onClick fires, so getToggleState() reads the post-click value.
        onButton.onClick = [this]
        {
            if (! onButton.getToggleState() && processor.getListenNode() == nodeId)
            {
                processor.setListenNode (-1);
                refreshListenState();
            }
        };
        addAndMakeVisible (onButton);

        listenButton.setButtonText ("Listen");
        listenButton.setColour (juce::ToggleButton::tickColourId, kTeal);
        listenButton.setTooltip ("Solo this node's removed signal (output = exactly what it cuts).");
        listenButton.onClick = [this]
        {
            processor.setListenNode (listenButton.getToggleState() ? nodeId : -1);
        };
        addAndMakeVisible (listenButton);

        choiceBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
        choiceBox.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (choiceBox);

        factory_ui::styleKnob (freqSlider, freqLabel, "Freq", " Hz");
        addAndMakeVisible (freqSlider); addAndMakeVisible (freqLabel);
        factory_ui::styleKnob (sensSlider, sensLabel, "Sens", " dB");
        addAndMakeVisible (sensSlider); addAndMakeVisible (sensLabel);
        factory_ui::styleKnob (widthSlider, widthLabel, "Width", " oct");
        addAndMakeVisible (widthSlider); addAndMakeVisible (widthLabel);
    }

    ~NodePanel() override { setLookAndFeel (nullptr); }

    int  currentNode()    const noexcept { return nodeId; }
    bool isCutNode()      const noexcept { return isCut; }
    int  preferredWidth() const noexcept { return isCut ? 320 : 480; } // + Listen toggle, bands + Width knob
    static constexpr int kHeight = 104;

    // Rebind to node `id` (0 = low cut, 1 = high cut, 2.. = band). Drops the old
    // attachments first so syncing the new ones can't write back to the old node.
    void setNode (int id)
    {
        nodeId = id;
        isCut  = (id < 2);

        onAtt.reset(); choiceAtt.reset(); freqAtt.reset(); sensAtt.reset(); widthAtt.reset();

        nameText = nodeName();
        nameCol  = nodeColour();

        choiceBox.clear (juce::dontSendNotification);
        if (isCut) choiceBox.addItemList ({ "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 1);
        else       choiceBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" }, 1);

        sensSlider.setVisible (! isCut);
        sensLabel.setVisible (! isCut);
        widthSlider.setVisible (! isCut);
        widthLabel.setVisible (! isCut);

        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        onAtt     = std::make_unique<BA> (apvts, pid ("on"),                     onButton);
        choiceAtt = std::make_unique<CA> (apvts, pid (isCut ? "slope" : "type"), choiceBox);
        freqAtt   = std::make_unique<SA> (apvts, pid ("freq"),                   freqSlider);
        if (! isCut)
        {
            sensAtt  = std::make_unique<SA> (apvts, pid ("sens"),  sensSlider);
            widthAtt = std::make_unique<SA> (apvts, pid ("width"), widthSlider);
        }

        // Freq reads in kHz at/above 1 kHz, Hz below. Must run after the
        // attachment, which installs its own textFromValueFunction (see #26).
        freqSlider.textFromValueFunction = [] (double v)
        {
            if (v >= 1000.0)
            {
                const double k = v / 1000.0;
                const bool whole = std::abs (k - std::round (k)) < 0.05;
                return (whole ? juce::String ((int) std::round (k)) : juce::String (k, 1)) + " kHz";
            }
            return juce::String (juce::roundToInt (v)) + " Hz";
        };
        freqSlider.setTextValueSuffix ({}); // the lambda already prints Hz/kHz — avoid a doubled suffix
        freqSlider.updateText();
        if (! isCut)
        {
            factory_ui::setSliderDecimals (sensSlider, 2);  // dB to 2 dp
            factory_ui::setSliderDecimals (widthSlider, 2); // oct to 2 dp (#23)
        }

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
        factory_ui::paintCard (g, getLocalBounds().toFloat(), 12.0f);
        // Floating node name, painted between the On toggle and the Listen toggle.
        g.setColour (nameCol);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawText (nameText, nameArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);

        // Right: Freq (+ Sens + Width) knobs filling the full height.
        constexpr int knobW = 64, kgap = 8;
        auto knobs = r.removeFromRight (isCut ? knobW : knobW * 3 + kgap * 2);
        r.removeFromRight (12); // gap before the knobs

        // Left-top: On (top-left) + node name + Listen (top-right of this row).
        auto topRow = r.removeFromTop (24);
        onButton.setBounds (topRow.removeFromLeft (46).withSizeKeepingCentre (46, 22));
        topRow.removeFromLeft (6);
        // Wide enough for the pill (~34px) + its "Listen" label (FactoryLookAndFeel
        // draws the text to the pill's right in the button's own bounds) — a
        // tighter box silently ellipsises the label away (see Phase 5a-2).
        auto listenArea = topRow.removeFromRight (108);
        listenButton.setBounds (listenArea.withSizeKeepingCentre (104, 22));
        topRow.removeFromRight (6);
        nameArea = topRow; // name painted here
        r.removeFromTop (8);
        choiceBox.setBounds (r.removeFromTop (26));

        auto col = [] (juce::Rectangle<int> c, juce::Slider& s, juce::Label& l)
        {
            l.setBounds (c.removeFromTop (13));
            s.setBounds (c); // rotary + value box fill the rest (full height)
        };
        if (isCut) col (knobs, freqSlider, freqLabel);
        else
        {
            col (knobs.removeFromLeft (knobW), freqSlider, freqLabel);
            knobs.removeFromLeft (kgap);
            col (knobs.removeFromLeft (knobW), sensSlider, sensLabel);
            knobs.removeFromLeft (kgap);
            col (knobs, widthSlider, widthLabel);
        }
    }

private:
    // FactoryLookAndFeel but with slightly smaller (11 px) label text for the knob
    // captions / value boxes / combo — FactoryLookAndFeel::getLabelFont is fixed
    // at 13 px, so we override just that.
    struct PanelLnF : FactoryLookAndFeel
    {
        juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (11.0f)); }
    };

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
    juce::Colour nodeColour() const
    {
        return isCut ? FactoryLookAndFeel::accent() : FactoryLookAndFeel::bandColour (nodeId - 2);
    }

    inline static const juce::Colour kTeal { juce::Colour (0xff45b8acu) }; // matches the curve's reduction teal

    PanelLnF lnf;
    ResonanceSuppressorAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    int  nodeId = 0;
    bool isCut  = true;
    juce::String nameText;
    juce::Colour nameCol { FactoryLookAndFeel::accent() };
    juce::Rectangle<int> nameArea;

    juce::ToggleButton onButton, listenButton;
    juce::ComboBox choiceBox;
    juce::Slider freqSlider, sensSlider, widthSlider;
    juce::Label  freqLabel,  sensLabel,  widthLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   onAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> choiceAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   freqAtt, sensAtt, widthAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodePanel)
};
