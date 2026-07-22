#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

OmoideEchoAudioProcessorEditor::OmoideEchoAudioProcessorEditor (OmoideEchoAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("OMOIDE ECHO", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    factory_ui::styleKnob (delaySlider,  delayLabel,  "Delay",  " ms");
    factory_ui::styleKnob (regenSlider,  regenLabel,  "Regen",  " %");
    factory_ui::styleKnob (toneSlider,   toneLabel,   "Tone",   " Hz");
    factory_ui::styleKnob (scanSlider,   scanLabel,   "Scan",   " %");
    factory_ui::styleKnob (memorySlider, memoryLabel, "Memory", " %");
    factory_ui::styleKnob (mixSlider,    mixLabel,    "Mix",    " %");
    factory_ui::styleKnob (outputSlider, outputLabel, "Output", " dB");

    for (auto* s : { &delaySlider, &regenSlider, &toneSlider, &scanSlider,
                     &memorySlider, &mixSlider, &outputSlider })
        addAndMakeVisible (s);
    for (auto* l : { &delayLabel, &regenLabel, &toneLabel, &scanLabel,
                     &memoryLabel, &mixLabel, &outputLabel })
        addAndMakeVisible (l);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    auto& s = processor.apvts;
    delayAtt  = std::make_unique<SliderAttachment> (s, "delay",     delaySlider);
    regenAtt  = std::make_unique<SliderAttachment> (s, "regen",     regenSlider);
    toneAtt   = std::make_unique<SliderAttachment> (s, "tone",      toneSlider);
    scanAtt   = std::make_unique<SliderAttachment> (s, "scan",      scanSlider);
    memoryAtt = std::make_unique<SliderAttachment> (s, "scanlevel", memorySlider);
    mixAtt    = std::make_unique<SliderAttachment> (s, "mix",       mixSlider);
    outputAtt = std::make_unique<SliderAttachment> (s, "output",    outputSlider);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass",    bypassButton);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    factory_ui::setSliderDecimals (delaySlider,  0);
    factory_ui::setSliderDecimals (regenSlider,  0);
    factory_ui::setSliderDecimals (toneSlider,   0);
    factory_ui::setSliderDecimals (scanSlider,   0);
    factory_ui::setSliderDecimals (memorySlider, 0);
    factory_ui::setSliderDecimals (mixSlider,    0);
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (640, 420);
}

OmoideEchoAudioProcessorEditor::~OmoideEchoAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void OmoideEchoAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void OmoideEchoAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (14);

    auto layoutRow = [] (juce::Rectangle<int> row,
                         std::initializer_list<std::pair<juce::Slider*, juce::Label*>> knobs)
    {
        const int w = row.getWidth() / (int) knobs.size();
        for (auto& knob : knobs)
        {
            auto cell = row.removeFromLeft (w);
            knob.second->setBounds (cell.removeFromTop (18));
            knob.first->setBounds (cell.reduced (6, 0));
        }
    };

    auto row1 = r.removeFromTop (150);
    layoutRow (row1, { { &delaySlider, &delayLabel }, { &regenSlider, &regenLabel },
                       { &toneSlider, &toneLabel },   { &scanSlider, &scanLabel } });

    r.removeFromTop (14);

    auto row2 = r.removeFromTop (150);
    layoutRow (row2, { { &memorySlider, &memoryLabel }, { &mixSlider, &mixLabel },
                       { &outputSlider, &outputLabel } });
}
