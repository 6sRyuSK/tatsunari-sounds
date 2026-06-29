#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"

//
// Detail controls for the currently selected band. Rebinds its APVTS
// attachments whenever the selection changes (Pro-Q-style: edit one band at a
// time). GUI-thread only.
//
class BandControlPanel : public juce::Component
{
public:
    explicit BandControlPanel (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
        addAndMakeVisible (title);

        enableButton.setButtonText ("On");
        addAndMakeVisible (enableButton);

        typeBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass" }, 1);
        addAndMakeVisible (typeBox);

        dynButton.setButtonText ("Dynamics");
        addAndMakeVisible (dynButton);

        configureKnob (qSlider,   qLabel,   "Q",     {});
        configureKnob (thrSlider, thrLabel, "Thresh", " dB");
        configureKnob (rngSlider, rngLabel, "Range",  " dB");

        setBand (0);
    }

    void setBand (int b)
    {
        band = b;
        title.setText ("BAND " + juce::String (b + 1), juce::dontSendNotification);

        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        auto id = [b] (const char* s) { return DynamicEqAudioProcessor::pid (b, s); };

        enableAtt = std::make_unique<BA> (apvts, id ("on"),   enableButton);
        typeAtt   = std::make_unique<CA> (apvts, id ("type"), typeBox);
        dynAtt    = std::make_unique<BA> (apvts, id ("dyn"),  dynButton);
        qAtt      = std::make_unique<SA> (apvts, id ("q"),    qSlider);
        thrAtt    = std::make_unique<SA> (apvts, id ("thr"),  thrSlider);
        rngAtt    = std::make_unique<SA> (apvts, id ("rng"),  rngSlider);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto left = r.removeFromLeft (150);
        title.setBounds (left.removeFromTop (22));
        left.removeFromTop (4);
        enableButton.setBounds (left.removeFromTop (24));
        left.removeFromTop (4);
        typeBox.setBounds (left.removeFromTop (26));
        left.removeFromTop (6);
        dynButton.setBounds (left.removeFromTop (24));

        r.removeFromLeft (16);
        const int colW = r.getWidth() / 3;
        auto layoutKnob = [] (juce::Rectangle<int> area, juce::Slider& s, juce::Label& l) {
            l.setBounds (area.removeFromTop (16));
            s.setBounds (area);
        };
        layoutKnob (r.removeFromLeft (colW), qSlider,   qLabel);
        layoutKnob (r.removeFromLeft (colW), thrSlider, thrLabel);
        layoutKnob (r,                       rngSlider, rngLabel);
    }

private:
    void configureKnob (juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::String& suffix)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 68, 18);
        slider.setTextValueSuffix (suffix);
        addAndMakeVisible (slider);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
        addAndMakeVisible (label);
    }

    juce::AudioProcessorValueTreeState& apvts;
    int band = 0;

    juce::Label title;
    juce::ToggleButton enableButton, dynButton;
    juce::ComboBox typeBox;
    juce::Slider qSlider, thrSlider, rngSlider;
    juce::Label  qLabel, thrLabel, rngLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   enableAtt, dynAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   qAtt, thrAtt, rngAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandControlPanel)
};
