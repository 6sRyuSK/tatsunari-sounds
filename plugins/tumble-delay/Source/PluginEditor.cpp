#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

TumbleDelayAudioProcessorEditor::TumbleDelayAudioProcessorEditor (TumbleDelayAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("TUMBLE DELAY", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    factory_ui::styleKnob (outputSlider, outputLabel, "Output", " dB");
    addAndMakeVisible (outputSlider);
    addAndMakeVisible (outputLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    auto& s = processor.apvts;
    outputAtt = std::make_unique<SliderAttachment> (s, "output", outputSlider);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (460, 300);
}

TumbleDelayAudioProcessorEditor::~TumbleDelayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void TumbleDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void TumbleDelayAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (14);
    auto knob = r.removeFromTop (110).removeFromLeft (r.getWidth() / 3);
    outputLabel.setBounds (knob.removeFromTop (18));
    outputSlider.setBounds (knob.reduced (6, 0));
}
