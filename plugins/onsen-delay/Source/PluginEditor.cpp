#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

OnsenDelayAudioProcessorEditor::OnsenDelayAudioProcessorEditor (OnsenDelayAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p),
      stepDots (p.uiCurrentStep)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("ONSEN DELAY", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    factory_ui::styleKnob (timeSlider,   timeLabel,   "Time",   " ms");
    factory_ui::styleKnob (glideSlider,  glideLabel,  "Glide",  " %");
    factory_ui::styleKnob (regenSlider,  regenLabel,  "Regen",  " %");
    factory_ui::styleKnob (toneSlider,   toneLabel,   "Tone",   " Hz");
    factory_ui::styleKnob (mixSlider,    mixLabel,    "Mix",    " %");
    factory_ui::styleKnob (outputSlider, outputLabel, "Output", " dB");
    for (auto* c : { &timeSlider, &glideSlider, &regenSlider, &toneSlider, &mixSlider, &outputSlider })
        addAndMakeVisible (*c);
    for (auto* l : { &timeLabel, &glideLabel, &regenLabel, &toneLabel, &mixLabel, &outputLabel })
        addAndMakeVisible (*l);

    styleChoiceLabel (int1Label,     "Interval 1");
    styleChoiceLabel (int2Label,     "Interval 2");
    styleChoiceLabel (divisionLabel, "Division");
    styleChoiceLabel (stepModeLabel, "Sequence");
    styleChoiceLabel (stepLabel,     "Step");
    for (auto* b : { &int1Box, &int2Box, &divisionBox, &stepModeBox })
        addAndMakeVisible (*b);

    syncButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (syncButton);

    // Momentary: pressed = advance parameter 1, released = 0 (Manual mode's
    // sequencer trigger; the processor edge-detects the rising side).
    advanceButton.onStateChange = [this]
    {
        const bool down = advanceButton.isDown();
        if (down == advanceDown)
            return;
        advanceDown = down;
        if (auto* param = processor.apvts.getParameter ("advance"))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (down ? 1.0f : 0.0f);
            param->endChangeGesture();
        }
    };
    addAndMakeVisible (advanceButton);
    addAndMakeVisible (stepDots);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    auto& s = processor.apvts;
    timeAtt     = std::make_unique<SliderAttachment> (s, "time",   timeSlider);
    glideAtt    = std::make_unique<SliderAttachment> (s, "glide",  glideSlider);
    regenAtt    = std::make_unique<SliderAttachment> (s, "regen",  regenSlider);
    toneAtt     = std::make_unique<SliderAttachment> (s, "tone",   toneSlider);
    mixAtt      = std::make_unique<SliderAttachment> (s, "mix",    mixSlider);
    outputAtt   = std::make_unique<SliderAttachment> (s, "output", outputSlider);
    syncAtt     = std::make_unique<ButtonAttachment> (s, "sync",   syncButton);
    bypassAtt   = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);
    int1Att     = std::make_unique<ComboBoxAttachment> (s, "int1",     int1Box);
    int2Att     = std::make_unique<ComboBoxAttachment> (s, "int2",     int2Box);
    divisionAtt = std::make_unique<ComboBoxAttachment> (s, "division", divisionBox);
    stepModeAtt = std::make_unique<ComboBoxAttachment> (s, "stepmode", stepModeBox);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    factory_ui::setSliderDecimals (timeSlider,   0);
    factory_ui::setSliderDecimals (glideSlider,  0);
    factory_ui::setSliderDecimals (regenSlider,  0);
    factory_ui::setSliderDecimals (toneSlider,   0);
    factory_ui::setSliderDecimals (mixSlider,    0);
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (640, 288);
}

OnsenDelayAudioProcessorEditor::~OnsenDelayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void OnsenDelayAudioProcessorEditor::styleChoiceLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (12.0f)));
    label.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (label);
}

void OnsenDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void OnsenDelayAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (12);

    // Knob row: Time / Glide / Regen / Tone / Mix / Output.
    auto knobRow = r.removeFromTop (118);
    const int kw = knobRow.getWidth() / 6;
    auto layoutKnob = [&] (juce::Slider& sl, juce::Label& lb)
    {
        auto cell = knobRow.removeFromLeft (kw);
        lb.setBounds (cell.removeFromTop (18));
        sl.setBounds (cell.reduced (4, 0));
    };
    layoutKnob (timeSlider,   timeLabel);
    layoutKnob (glideSlider,  glideLabel);
    layoutKnob (regenSlider,  regenLabel);
    layoutKnob (toneSlider,   toneLabel);
    layoutKnob (mixSlider,    mixLabel);
    layoutKnob (outputSlider, outputLabel);

    r.removeFromTop (12);

    // Sequencer / sync row: Int1, Int2, Sync+Division, Sequence mode, Step.
    auto row = r.removeFromTop (46);
    const int cw = row.getWidth() / 5;

    auto cell = row.removeFromLeft (cw);
    int1Label.setBounds (cell.removeFromTop (18));
    int1Box.setBounds (cell.reduced (6, 0));

    cell = row.removeFromLeft (cw);
    int2Label.setBounds (cell.removeFromTop (18));
    int2Box.setBounds (cell.reduced (6, 0));

    cell = row.removeFromLeft (cw);
    divisionLabel.setBounds (cell.removeFromTop (18));
    syncButton.setBounds (cell.removeFromLeft (juce::jmin (64, cw / 2)));
    divisionBox.setBounds (cell.reduced (2, 0));

    cell = row.removeFromLeft (cw);
    stepModeLabel.setBounds (cell.removeFromTop (18));
    stepModeBox.setBounds (cell.reduced (6, 0));

    cell = row; // remainder
    stepLabel.setBounds (cell.removeFromTop (18));
    advanceButton.setBounds (cell.removeFromLeft (cell.getWidth() / 2).reduced (4, 2));
    stepDots.setBounds (cell.reduced (4, 8));
}
