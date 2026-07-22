#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

MochiStretchAudioProcessorEditor::MochiStretchAudioProcessorEditor (MochiStretchAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("MOCHI STRETCH", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    factory_ui::styleKnob (speedSlider,  speedLabel,  "Speed",  " %");
    factory_ui::styleKnob (pitchSlider,  pitchLabel,  "Pitch",  " st");
    factory_ui::styleKnob (windowSlider, windowLabel, "Window", " ms");
    addAndMakeVisible (speedSlider);  addAndMakeVisible (speedLabel);
    addAndMakeVisible (pitchSlider);  addAndMakeVisible (pitchLabel);
    addAndMakeVisible (windowSlider); addAndMakeVisible (windowLabel);

    factory_ui::styleKnob (mixSlider,    mixLabel,    "Mix",    " %");
    factory_ui::styleKnob (outputSlider, outputLabel, "Output", " dB");
    addAndMakeVisible (mixSlider);    addAndMakeVisible (mixLabel);
    addAndMakeVisible (outputSlider); addAndMakeVisible (outputLabel);

    // Hold is a latching toggle button (madoromi-style loop latch), not a
    // knob: clicking flips and holds state, coloured to show ON/OFF.
    holdLabel.setText ("Hold", juce::dontSendNotification);
    holdLabel.setJustificationType (juce::Justification::centred);
    holdLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (holdLabel);

    holdButton.setClickingTogglesState (true);
    holdButton.setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
    holdButton.setColour (juce::TextButton::buttonOnColourId, FactoryLookAndFeel::accent());
    holdButton.setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::text());
    holdButton.setColour (juce::TextButton::textColourOnId, FactoryLookAndFeel::panel());
    addAndMakeVisible (holdButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    auto& s = processor.apvts;
    speedAtt  = std::make_unique<SliderAttachment> (s, "speed",  speedSlider);
    pitchAtt  = std::make_unique<SliderAttachment> (s, "pitch",  pitchSlider);
    windowAtt = std::make_unique<SliderAttachment> (s, "window", windowSlider);
    mixAtt    = std::make_unique<SliderAttachment> (s, "mix",    mixSlider);
    outputAtt = std::make_unique<SliderAttachment> (s, "output", outputSlider);
    holdAtt   = std::make_unique<ButtonAttachment> (s, "hold",   holdButton);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    factory_ui::setSliderDecimals (speedSlider, 0); // "%" convention: 0 decimals (house style)
    factory_ui::setSliderDecimals (pitchSlider, 0);
    factory_ui::setSliderDecimals (windowSlider, 0);
    factory_ui::setSliderDecimals (mixSlider, 0);
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (640, 420);
}

MochiStretchAudioProcessorEditor::~MochiStretchAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void MochiStretchAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void MochiStretchAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (14);

    // Row 1: Speed / Pitch / Window knobs.
    auto row1 = r.removeFromTop (150);
    const int w1 = row1.getWidth() / 3;
    auto speedArea  = row1.removeFromLeft (w1);
    auto pitchArea  = row1.removeFromLeft (w1);
    auto windowArea = row1;

    speedLabel.setBounds (speedArea.removeFromTop (18));
    speedSlider.setBounds (speedArea.reduced (10, 0));
    pitchLabel.setBounds (pitchArea.removeFromTop (18));
    pitchSlider.setBounds (pitchArea.reduced (10, 0));
    windowLabel.setBounds (windowArea.removeFromTop (18));
    windowSlider.setBounds (windowArea.reduced (10, 0));

    r.removeFromTop (14);

    // Row 2: Hold (latching button) / Mix / Output.
    auto row2 = r.removeFromTop (150);
    const int w2 = row2.getWidth() / 3;
    auto holdArea   = row2.removeFromLeft (w2);
    auto mixArea    = row2.removeFromLeft (w2);
    auto outputArea = row2;

    holdLabel.setBounds (holdArea.removeFromTop (18));
    holdButton.setBounds (holdArea.reduced (16, 34));

    mixLabel.setBounds (mixArea.removeFromTop (18));
    mixSlider.setBounds (mixArea.reduced (10, 0));
    outputLabel.setBounds (outputArea.removeFromTop (18));
    outputSlider.setBounds (outputArea.reduced (10, 0));
}
