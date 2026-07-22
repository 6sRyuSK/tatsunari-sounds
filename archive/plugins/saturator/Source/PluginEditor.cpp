#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

SaturatorAudioProcessorEditor::SaturatorAudioProcessorEditor (SaturatorAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p), curve (p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("SATURATOR", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    configureKnob (driveSlider,  driveLabel,  "Drive",  " dB");
    configureKnob (mixSlider,    mixLabel,    "Mix",    " %");
    configureKnob (outputSlider, outputLabel, "Output", " dB");

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (curve);

    auto& s = processor.apvts;
    driveAtt  = std::make_unique<SliderAttachment> (s, "drive",  driveSlider);
    mixAtt    = std::make_unique<SliderAttachment> (s, "mix",    mixSlider);
    outputAtt = std::make_unique<SliderAttachment> (s, "output", outputSlider);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    // Pin the text-box precision. Must run after the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23). dB to
    // 2 dp; % as an integer.
    factory_ui::setSliderDecimals (driveSlider, 2);
    factory_ui::setSliderDecimals (outputSlider, 2);
    factory_ui::setSliderDecimals (mixSlider, 0);

    setSize (460, 380);
}

SaturatorAudioProcessorEditor::~SaturatorAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void SaturatorAudioProcessorEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                                   const juce::String& name, const juce::String& suffix)
{
    factory_ui::styleKnob (slider, label, name, suffix);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void SaturatorAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, curve.getBounds());
}

void SaturatorAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (10);
    curve.setBounds (r.removeFromTop (168));

    r.removeFromTop (14);
    auto knobs = r;
    const int colW = knobs.getWidth() / 3;

    auto layoutKnob = [] (juce::Rectangle<int> area, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (area.removeFromTop (18));
        s.setBounds (area.reduced (6, 0));
    };

    layoutKnob (knobs.removeFromLeft (colW), driveSlider,  driveLabel);
    layoutKnob (knobs.removeFromLeft (colW), mixSlider,    mixLabel);
    layoutKnob (knobs,                       outputSlider, outputLabel);
}
