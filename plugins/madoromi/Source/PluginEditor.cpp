#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

MadoromiAudioProcessorEditor::MadoromiAudioProcessorEditor (MadoromiAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("MADOROMI", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    configureKnob (clockSlider,   clockLabel,   "Clock",   " Hz");
    configureKnob (washSlider,    washLabel,    "Wash",    " %");
    configureKnob (toneSlider,    toneLabel,    "Tone",    " Hz");
    configureKnob (lengthSlider,  lengthLabel,  "Length",  " ms");
    configureKnob (balanceSlider, balanceLabel, "Balance", " %");
    configureKnob (mixSlider,     mixLabel,     "Mix",     " %");
    configureKnob (outputSlider,  outputLabel,  "Output",  " dB");

    // Big latching Loop button (row 2, last cell). Leave the actual button
    // painting to the LookAndFeel's stock TextButton rendering -- only the
    // on/off colours are set here (palette-only), so the latched state reads
    // clearly in accent.
    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::buttonColourId,   FactoryLookAndFeel::panel());
    loopButton.setColour (juce::TextButton::buttonOnColourId, FactoryLookAndFeel::accent());
    loopButton.setColour (juce::TextButton::textColourOffId,  FactoryLookAndFeel::textDim());
    loopButton.setColour (juce::TextButton::textColourOnId,   FactoryLookAndFeel::panel());
    addAndMakeVisible (loopButton);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    auto& s = processor.apvts;
    clockAtt   = std::make_unique<SliderAttachment> (s, "clock",   clockSlider);
    washAtt    = std::make_unique<SliderAttachment> (s, "wash",    washSlider);
    toneAtt    = std::make_unique<SliderAttachment> (s, "tone",    toneSlider);
    lengthAtt  = std::make_unique<SliderAttachment> (s, "length",  lengthSlider);
    balanceAtt = std::make_unique<SliderAttachment> (s, "balance", balanceSlider);
    mixAtt     = std::make_unique<SliderAttachment> (s, "mix",     mixSlider);
    outputAtt  = std::make_unique<SliderAttachment> (s, "output",  outputSlider);
    loopAtt    = std::make_unique<ButtonAttachment> (s, "loop",    loopButton);
    bypassAtt  = std::make_unique<ButtonAttachment> (s, "bypass",  bypassButton);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    // Hz / ms / % show as whole numbers; dB keeps 2 decimal places.
    for (auto* sl : { &clockSlider, &washSlider, &toneSlider, &lengthSlider, &balanceSlider, &mixSlider })
        factory_ui::setSliderDecimals (*sl, 0);
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (640, 420);
}

MadoromiAudioProcessorEditor::~MadoromiAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void MadoromiAudioProcessorEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                                  const juce::String& name, const juce::String& suffix)
{
    factory_ui::styleKnob (slider, label, name, suffix);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void MadoromiAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void MadoromiAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (10);

    // 4 x 2 grid: row 1 = Clock/Wash/Tone/Length, row 2 = Balance/Mix/Output/Loop.
    const int colW = r.getWidth() / 4;
    const int rowH = r.getHeight() / 2;

    auto cell = [&] (int col, int row)
    {
        return juce::Rectangle<int> (r.getX() + col * colW, r.getY() + row * rowH, colW, rowH).reduced (6);
    };

    auto layoutKnob = [] (juce::Rectangle<int> area, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (area.removeFromTop (18));
        s.setBounds (area);
    };

    layoutKnob (cell (0, 0), clockSlider,  clockLabel);
    layoutKnob (cell (1, 0), washSlider,   washLabel);
    layoutKnob (cell (2, 0), toneSlider,   toneLabel);
    layoutKnob (cell (3, 0), lengthSlider, lengthLabel);

    layoutKnob (cell (0, 1), balanceSlider, balanceLabel);
    layoutKnob (cell (1, 1), mixSlider,     mixLabel);
    layoutKnob (cell (2, 1), outputSlider,  outputLabel);

    // Loop is a big button, not a knob -- give it most of the last cell.
    loopButton.setBounds (cell (3, 1).reduced (2, 18));
}
