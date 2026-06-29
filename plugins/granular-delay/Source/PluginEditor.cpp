#include "PluginEditor.h"

GranularDelayAudioProcessorEditor::GranularDelayAudioProcessorEditor (GranularDelayAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), cloud (p, p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("GRANULAR DELAY", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (cloud);

    // Order matters for layout.
    addKnob ("delay",     "Delay",      " ms");
    addKnob ("feedback",  "Feedback",   " %");
    addKnob ("mix",       "Mix",        " %");
    addKnob ("grainsize", "Grain",      " ms");
    addKnob ("density",   "Density",    " Hz");
    addKnob ("jitter",    "Jitter",     " ms");
    addKnob ("pitch",     "Pitch",      " st");
    addKnob ("pitchrand", "Pitch Rnd",  " st");
    addKnob ("spread",    "Spread",     " %");
    addKnob ("lforate",   "LFO Rate",   " Hz");
    addKnob ("lfodepth",  "LFO Depth",  " %");

    syncButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (syncButton);
    divisionBox.addItemList ({ "1/4", "1/4.", "1/8", "1/8.", "1/8T", "1/16" }, 1);
    divisionBox.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (divisionBox);

    auto& s = processor.apvts;
    bypassAtt   = std::make_unique<ButtonAttachment>   (s, "bypass",   bypassButton);
    syncAtt     = std::make_unique<ButtonAttachment>   (s, "sync",     syncButton);
    divisionAtt = std::make_unique<ComboBoxAttachment> (s, "division", divisionBox);

    setSize (720, 520);
}

GranularDelayAudioProcessorEditor::~GranularDelayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void GranularDelayAudioProcessorEditor::addKnob (const char* id, const char* name, const char* suffix)
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
    knobs.push_back (std::move (slider));
    knobLabels.push_back (std::move (label));
}

void GranularDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (FactoryLookAndFeel::background());
}

void GranularDelayAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top);

    r.removeFromTop (10);
    cloud.setBounds (r.removeFromTop (188));
    r.removeFromTop (12);

    // Two rows of six cells.
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

    // Last cell: sync toggle + division selector.
    auto syncCell = row2.reduced (4);
    syncButton.setBounds (syncCell.removeFromTop (24));
    syncCell.removeFromTop (6);
    divisionBox.setBounds (syncCell.removeFromTop (28));
}
