#include "PluginEditor.h"

ShimmerReverbAudioProcessorEditor::ShimmerReverbAudioProcessorEditor (ShimmerReverbAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), visualizer (p, p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("SHIMMER REVERB", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    freezeButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (freezeButton);
    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (visualizer);

    addKnob ("size",     "Size",      " %");
    addKnob ("decay",    "Decay",     " s");
    addKnob ("damping",  "Damping",   " %");
    addKnob ("predelay", "Pre-Delay", " ms");
    addKnob ("mix",      "Mix",       " %");
    addKnob ("shimmer",  "Shimmer",   " %");
    addKnob ("voicemix", "Voice Mix", " %");
    addKnob ("lowcut",   "Low Cut",   " Hz");
    addKnob ("highcut",  "High Cut",  " Hz");
    addKnob ("modrate",  "Mod Rate",  " Hz");
    addKnob ("moddepth", "Mod Depth", " %");

    setupPitchBox (pitchABox, pitchALabel, "Pitch A");
    setupPitchBox (pitchBBox, pitchBLabel, "Pitch B");

    auto& s = processor.apvts;
    freezeAtt = std::make_unique<ButtonAttachment>   (s, "freeze", freezeButton);
    bypassAtt = std::make_unique<ButtonAttachment>   (s, "bypass", bypassButton);
    pitchAAtt = std::make_unique<ComboBoxAttachment> (s, "pitcha", pitchABox);
    pitchBAtt = std::make_unique<ComboBoxAttachment> (s, "pitchb", pitchBBox);

    setSize (740, 520);
}

ShimmerReverbAudioProcessorEditor::~ShimmerReverbAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void ShimmerReverbAudioProcessorEditor::addKnob (const char* id, const char* name, const char* suffix)
{
    auto slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag,
                                                  juce::Slider::TextBoxBelow);
    slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    slider->setTextValueSuffix (suffix);
    addAndMakeVisible (*slider);

    auto label = std::make_unique<juce::Label>();
    label->setText (name, juce::dontSendNotification);
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (*label);

    knobAtts.push_back (std::make_unique<SliderAttachment> (processor.apvts, id, *slider));
    // Continuous (skewed) params otherwise show 7 decimals; cap to 2.
    if (slider->getInterval() == 0.0)
        slider->setNumDecimalPlacesToDisplay (2);
    knobs.push_back (std::move (slider));
    knobLabels.push_back (std::move (label));
}

void ShimmerReverbAudioProcessorEditor::setupPitchBox (juce::ComboBox& box, juce::Label& label, const char* name)
{
    box.addItemList ({ "+12", "+7", "+5", "+19", "-12" }, 1);
    box.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (box);
    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (label);
}

void ShimmerReverbAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (FactoryLookAndFeel::background());
}

void ShimmerReverbAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    freezeButton.setBounds (top.removeFromRight (90));
    titleLabel.setBounds (top);

    r.removeFromTop (10);
    visualizer.setBounds (r.removeFromTop (180));
    r.removeFromTop (12);

    const int cols = 6;
    const int rowH = r.getHeight() / 2;
    auto row1 = r.removeFromTop (rowH);
    auto row2 = r;
    const int cw = row1.getWidth() / cols;

    auto place = [] (juce::Rectangle<int> cell, juce::Slider& s, juce::Label& l) {
        l.setBounds (cell.removeFromTop (16));
        s.setBounds (cell);
    };

    for (int i = 0; i < cols; ++i)
        place (row1.removeFromLeft (cw).reduced (4), *knobs[(size_t) i], *knobLabels[(size_t) i]);

    for (int i = 0; i < 5; ++i)
        place (row2.removeFromLeft (cw).reduced (4), *knobs[(size_t) (6 + i)], *knobLabels[(size_t) (6 + i)]);

    // Last cell: the two pitch selectors stacked.
    auto cell = row2.reduced (4);
    pitchALabel.setBounds (cell.removeFromTop (14));
    pitchABox.setBounds (cell.removeFromTop (24));
    cell.removeFromTop (4);
    pitchBLabel.setBounds (cell.removeFromTop (14));
    pitchBBox.setBounds (cell.removeFromTop (24));
}
