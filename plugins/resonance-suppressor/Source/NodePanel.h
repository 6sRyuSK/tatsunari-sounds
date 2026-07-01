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
// attachments to that node on selection (Pro-Q-style). The node name sits centred
// on top; below it a row of controls: cut nodes show On + Slope + Freq, band
// nodes show On + Type + Freq + Sens. Frequencies read in kHz at/above 1 kHz.
// Horizontal "kawaii" card styling (factory_ui). GUI-thread only.
//
class NodePanel : public juce::Component
{
public:
    explicit NodePanel (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        title.setJustificationType (juce::Justification::centred);
        title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        onButton.setButtonText ("On");
        addAndMakeVisible (onButton);

        choiceBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
        choiceBox.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (choiceBox);

        factory_ui::styleKnob (freqSlider, freqLabel, "Freq", " Hz");
        addAndMakeVisible (freqSlider); addAndMakeVisible (freqLabel);
        factory_ui::styleKnob (sensSlider, sensLabel, "Sens", " dB");
        addAndMakeVisible (sensSlider); addAndMakeVisible (sensLabel);
    }

    int  currentNode()    const noexcept { return nodeId; }
    bool isCutNode()      const noexcept { return isCut; }
    int  preferredWidth() const noexcept { return isCut ? 330 : 400; }
    static constexpr int kHeight = 94;

    // Rebind to node `id` (0 = low cut, 1 = high cut, 2.. = band). Drops the old
    // attachments first so syncing the new ones can't write back to the old node.
    void setNode (int id)
    {
        nodeId = id;
        isCut  = (id < 2);

        onAtt.reset(); choiceAtt.reset(); freqAtt.reset(); sensAtt.reset();

        title.setText (nodeName(), juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, nodeColour());

        choiceBox.clear (juce::dontSendNotification);
        if (isCut) choiceBox.addItemList ({ "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 1);
        else       choiceBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" }, 1);

        sensSlider.setVisible (! isCut);
        sensLabel.setVisible (! isCut);

        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        onAtt     = std::make_unique<BA> (apvts, pid ("on"),                     onButton);
        choiceAtt = std::make_unique<CA> (apvts, pid (isCut ? "slope" : "type"), choiceBox);
        freqAtt   = std::make_unique<SA> (apvts, pid ("freq"),                   freqSlider);
        if (! isCut) sensAtt = std::make_unique<SA> (apvts, pid ("sens"), sensSlider);

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
        freqSlider.updateText();
        if (! isCut) factory_ui::setSliderDecimals (sensSlider, 1);

        resized();
    }

    void paint (juce::Graphics& g) override
    {
        factory_ui::paintCard (g, getLocalBounds().toFloat(), 12.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        // Centre the control row within the card.
        constexpr int on = 54, combo = 118, knobW = 68, gap = 10;
        const int total = on + gap + combo + gap + knobW + (isCut ? 0 : gap + knobW);
        r.removeFromLeft (juce::jmax (0, (r.getWidth() - total) / 2));

        onButton.setBounds  (r.removeFromLeft (on).withSizeKeepingCentre (on, 24));
        r.removeFromLeft (gap);
        choiceBox.setBounds (r.removeFromLeft (combo).withSizeKeepingCentre (combo, 28));
        r.removeFromLeft (gap);

        auto knob = [&r] (juce::Slider& s, juce::Label& l)
        {
            auto c = r.removeFromLeft (knobW);
            l.setBounds (c.removeFromTop (14));
            s.setBounds (c);
            r.removeFromLeft (gap);
        };
        knob (freqSlider, freqLabel);
        if (! isCut) knob (sensSlider, sensLabel);
    }

private:
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

    juce::AudioProcessorValueTreeState& apvts;
    int  nodeId = 0;
    bool isCut  = true;

    juce::Label title;
    juce::ToggleButton onButton;
    juce::ComboBox choiceBox;
    juce::Slider freqSlider, sensSlider;
    juce::Label  freqLabel,  sensLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   onAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> choiceAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   freqAtt, sensAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodePanel)
};
