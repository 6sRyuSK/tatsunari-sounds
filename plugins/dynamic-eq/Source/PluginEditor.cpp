#include "PluginEditor.h"

DynamicEqAudioProcessorEditor::DynamicEqAudioProcessorEditor (DynamicEqAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      curve (p, p.apvts), panel (p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("DYNAMIC EQ", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (curve);
    addAndMakeVisible (panel);

    curve.onBandSelected = [this] (int b) { panel.setBand (b); };
    panel.setBand (curve.getSelectedBand());

    bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, "bypass", bypassButton);

    setSize (740, 480);
}

DynamicEqAudioProcessorEditor::~DynamicEqAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void DynamicEqAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (FactoryLookAndFeel::background());
}

void DynamicEqAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top);

    r.removeFromTop (10);
    panel.setBounds (r.removeFromBottom (130));
    r.removeFromBottom (12);
    curve.setBounds (r);
}
