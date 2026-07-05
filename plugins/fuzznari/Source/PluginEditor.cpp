#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

FuzznariAudioProcessorEditor::FuzznariAudioProcessorEditor (FuzznariAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p), curve (p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("FUZZNARI", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    configureKnob (driveSlider, driveLabel, "Drive", " dB");
    configureKnob (biasSlider,  biasLabel,  "Bias",  " %");
    configureKnob (gateSlider,  gateLabel,  "Gate",  " %");
    configureKnob (stabSlider,  stabLabel,  "Stab",  " %");
    configureKnob (toneSlider,  toneLabel,  "Tone",  " %");
    configureKnob (levelSlider, levelLabel, "Level", " dB");
    configureKnob (mixSlider,   mixLabel,   "Mix",   " %");

    // The Squeal switch arms the self-oscillator — accent-coloured as the one
    // "hot" control, and placed over the Stab column it belongs to.
    squealButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (squealButton);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (curve);

    auto& s = processor.apvts;
    driveAtt  = std::make_unique<SliderAttachment> (s, "drive", driveSlider);
    biasAtt   = std::make_unique<SliderAttachment> (s, "bias",  biasSlider);
    gateAtt   = std::make_unique<SliderAttachment> (s, "gate",  gateSlider);
    stabAtt   = std::make_unique<SliderAttachment> (s, "stab",  stabSlider);
    toneAtt   = std::make_unique<SliderAttachment> (s, "tone",  toneSlider);
    levelAtt  = std::make_unique<SliderAttachment> (s, "level", levelSlider);
    mixAtt    = std::make_unique<SliderAttachment> (s, "mix",   mixSlider);
    squealAtt = std::make_unique<ButtonAttachment> (s, "osc",    squealButton);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    // Pin the text-box precision. Must run after the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23). dB to
    // 2 dp; % as an integer.
    factory_ui::setSliderDecimals (driveSlider, 2);
    factory_ui::setSliderDecimals (levelSlider, 2);
    factory_ui::setSliderDecimals (biasSlider, 0);
    factory_ui::setSliderDecimals (gateSlider, 0);
    factory_ui::setSliderDecimals (stabSlider, 0);
    factory_ui::setSliderDecimals (toneSlider, 0);
    factory_ui::setSliderDecimals (mixSlider, 0);

    setSize (640, 540);
}

FuzznariAudioProcessorEditor::~FuzznariAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void FuzznariAudioProcessorEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                                  const juce::String& name, const juce::String& suffix)
{
    factory_ui::styleKnob (slider, label, name, suffix);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void FuzznariAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, curve.getBounds());
}

void FuzznariAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (140));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (10);
    curve.setBounds (r.removeFromTop (300));

    r.removeFromTop (6);
    auto squealRow = r.removeFromTop (22);

    r.removeFromTop (4);
    auto knobs = r;
    const int colW = knobs.getWidth() / 7;

    // Squeal sits over the Stab column (4th knob) — it arms that control.
    squealRow.removeFromLeft (colW * 3);
    squealButton.setBounds (squealRow.removeFromLeft (colW * 2));

    auto layoutKnob = [] (juce::Rectangle<int> area, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (area.removeFromTop (18));
        s.setBounds (area.reduced (4, 0));
    };

    layoutKnob (knobs.removeFromLeft (colW), driveSlider, driveLabel);
    layoutKnob (knobs.removeFromLeft (colW), biasSlider,  biasLabel);
    layoutKnob (knobs.removeFromLeft (colW), gateSlider,  gateLabel);
    layoutKnob (knobs.removeFromLeft (colW), stabSlider,  stabLabel);
    layoutKnob (knobs.removeFromLeft (colW), toneSlider,  toneLabel);
    layoutKnob (knobs.removeFromLeft (colW), levelSlider, levelLabel);
    layoutKnob (knobs,                       mixSlider,   mixLabel);
}
