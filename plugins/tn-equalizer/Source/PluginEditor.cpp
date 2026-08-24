#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

DynamicEqAudioProcessorEditor::DynamicEqAudioProcessorEditor (DynamicEqAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p),
      curve (p, p.apvts), panel (p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("Dynamic EQ", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
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

    setResizable (true, true);
    setResizeLimits (620, 440, 1280, 900);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (740.0 / 520.0);
    setSize (740, 520);
}

DynamicEqAudioProcessorEditor::~DynamicEqAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void DynamicEqAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, curve.getBounds());
    factory_ui::dropShadowFor (g, panel.getBounds());
}

void DynamicEqAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (28);
    bypassButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (8);
    titleLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (10);
    panel.setBounds (r.removeFromBottom (170));
    r.removeFromBottom (12);
    curve.setBounds (r);
}
