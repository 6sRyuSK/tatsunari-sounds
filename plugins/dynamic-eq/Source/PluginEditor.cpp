#include "PluginEditor.h"

DynamicEqAudioProcessorEditor::DynamicEqAudioProcessorEditor (DynamicEqAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
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
    // Warm white background with a soft vertical gradient.
    juce::ColourGradient bg (FactoryLookAndFeel::background(), 0.0f, 0.0f,
                             FactoryLookAndFeel::backgroundLo(), 0.0f, (float) getHeight(), false);
    g.setGradientFill (bg);
    g.fillAll();

    // Soft drop shadows behind the two cards.
    auto dropShadow = [&g] (juce::Rectangle<int> b)
    {
        juce::DropShadow ds (FactoryLookAndFeel::shadow(), 16, { 0, 5 });
        juce::Path path; path.addRoundedRectangle (b.toFloat(), 10.0f);
        ds.drawForPath (g, path);
    };
    dropShadow (curve.getBounds());
    dropShadow (panel.getBounds());
}

void DynamicEqAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (28);
    bypassButton.setBounds (top.removeFromRight (110));
    titleLabel.setBounds (top);

    r.removeFromTop (10);
    panel.setBounds (r.removeFromBottom (170));
    r.removeFromBottom (12);
    curve.setBounds (r);
}
