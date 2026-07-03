#include "PluginEditor.h"

MultibandEnhancerAudioProcessorEditor::MultibandEnhancerAudioProcessorEditor (MultibandEnhancerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), analyzer (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (analyzer);

    for (int b = 0; b < 5; ++b)
    {
        strips[(size_t) b] = std::make_unique<BandStripComponent> (processor, b);
        addAndMakeVisible (*strips[(size_t) b]);
    }

    // Mode / Quality choice boxes.
    auto fillChoice = [this] (juce::ComboBox& box, juce::Label& label, const char* name, const juce::StringArray& items, const juce::String& pid, std::unique_ptr<ComboAtt>& att)
    {
        box.addItemList (items, 1);
        addAndMakeVisible (box);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
        addAndMakeVisible (label);
        att = std::make_unique<ComboAtt> (processor.apvts, pid, box);
    };
    fillChoice (modeBox,    modeLabel,    "Mode",    { "Tube", "Tape", "Bright", "Clean", "Glue" }, "mode",    modeAtt);
    fillChoice (qualityBox, qualityLabel, "Quality", { "HQ", "Zero Latency" },                      "quality", qualityAtt);

    styleFader (directFader,   directLabel,   "Direct");
    styleFader (enhancedFader, enhancedLabel, "Enhanced");
    directAtt   = std::make_unique<SliderAtt> (processor.apvts, "direct", directFader);
    enhancedAtt = std::make_unique<SliderAtt> (processor.apvts, "wet",    enhancedFader);
    factory_ui::setSliderDecimals (directFader, 1);
    factory_ui::setSliderDecimals (enhancedFader, 1);

    factory_ui::styleKnob (outputKnob, outputLabel, "Output", " dB");
    addAndMakeVisible (outputKnob);
    addAndMakeVisible (outputLabel);
    outputAtt = std::make_unique<SliderAtt> (processor.apvts, "output", outputKnob);
    factory_ui::setSliderDecimals (outputKnob, 1);

    deltaButton.setColour (juce::ToggleButton::tickColourId, FactoryLookAndFeel::accent());
    bypassButton.setColour (juce::ToggleButton::tickColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (deltaButton);
    addAndMakeVisible (bypassButton);
    deltaAtt  = std::make_unique<ButtonAtt> (processor.apvts, "delta",  deltaButton);
    bypassAtt = std::make_unique<ButtonAtt> (processor.apvts, "bypass", bypassButton);

    setResizable (true, true);
    setResizeLimits (760, 480, 1400, 880);
    getConstrainer()->setFixedAspectRatio (920.0 / 580.0);
    setSize (920, 580);
}

MultibandEnhancerAudioProcessorEditor::~MultibandEnhancerAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void MultibandEnhancerAudioProcessorEditor::styleFader (juce::Slider& s, juce::Label& l, const juce::String& name)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 16);
    s.setColour (juce::Slider::textBoxTextColourId, FactoryLookAndFeel::text());
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setTextValueSuffix (" dB");
    addAndMakeVisible (s);
    l.setText (name, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (l);
}

void MultibandEnhancerAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());

    // Title.
    g.setColour (FactoryLookAndFeel::text());
    g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    g.drawText ("Tatsumin Enhancer", getLocalBounds().reduced (16, 8).removeFromTop (22),
                juce::Justification::topLeft);

    factory_ui::dropShadowFor (g, controlCard);
    factory_ui::paintCard (g, controlCard.toFloat());
}

void MultibandEnhancerAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (34); // title
    r.reduce (10, 8);

    // Right control card.
    controlCard = r.removeFromRight (150);
    r.removeFromRight (8);
    {
        auto c = controlCard.reduced (10);

        modeLabel.setBounds (c.removeFromTop (16));
        modeBox.setBounds (c.removeFromTop (24));
        c.removeFromTop (10);

        auto faders = c.removeFromTop (170);
        auto fl = faders.removeFromLeft (faders.getWidth() / 2);
        directLabel.setBounds (fl.removeFromTop (14));
        directFader.setBounds (fl);
        enhancedLabel.setBounds (faders.removeFromTop (14));
        enhancedFader.setBounds (faders);
        c.removeFromTop (8);

        auto out = c.removeFromTop (74);
        outputKnob.setBounds (out.removeFromTop (56));
        outputLabel.setBounds (out);
        c.removeFromTop (6);

        qualityLabel.setBounds (c.removeFromTop (16));
        qualityBox.setBounds (c.removeFromTop (24));
        c.removeFromTop (10);

        deltaButton.setBounds (c.removeFromTop (22));
        c.removeFromTop (4);
        bypassButton.setBounds (c.removeFromTop (22));
    }

    // Left: analyser over the band strip row.
    auto bandRow = r.removeFromBottom (188);
    analyzer.setBounds (r);

    const int sw = bandRow.getWidth() / 5;
    for (int b = 0; b < 5; ++b)
    {
        auto cell = (b < 4) ? bandRow.removeFromLeft (sw) : bandRow;
        strips[(size_t) b]->setBounds (cell);
    }
}
