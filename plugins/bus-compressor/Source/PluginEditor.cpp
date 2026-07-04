#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

BusCompressorAudioProcessorEditor::BusCompressorAudioProcessorEditor (BusCompressorAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), meter (p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("BUS COMPRESSOR", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    configureKnob (thresholdSlider, thresholdLabel, "Threshold", " dB");
    configureKnob (attackSlider,    attackLabel,    "Attack",    " ms");
    configureKnob (releaseSlider,   releaseLabel,   "Release",   " ms");
    configureKnob (makeupSlider,    makeupLabel,    "Makeup",    " dB");
    configureKnob (mixSlider,       mixLabel,       "Mix",       " %");

    ratioLabel.setText ("Ratio", juce::dontSendNotification);
    ratioLabel.setJustificationType (juce::Justification::centred);
    ratioLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (ratioLabel);

    ratioBox.addItem ("2:1", 1);
    ratioBox.addItem ("4:1", 2);
    ratioBox.addItem ("10:1", 3);
    ratioBox.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (ratioBox);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // Preset selector: populate from the processor's program list and wire the
    // two-way host sync. User selection drives the program API + notifies the
    // host; host-driven changes come back via audioProcessorChanged.
    refreshPresetSelector();
    presetSelector.onChange = [this] (int idx)
    {
        processor.setCurrentProgram (idx);
        processor.updateHostDisplay (
            juce::AudioProcessorListener::ChangeDetails{}.withProgramChanged (true));
    };
    addAndMakeVisible (presetSelector);
    processor.addListener (this);

    addAndMakeVisible (meter);

    auto& s = processor.apvts;
    thrAtt    = std::make_unique<SliderAttachment>   (s, "threshold", thresholdSlider);
    atkAtt    = std::make_unique<SliderAttachment>   (s, "attack",    attackSlider);
    relAtt    = std::make_unique<SliderAttachment>   (s, "release",   releaseSlider);
    makeAtt   = std::make_unique<SliderAttachment>   (s, "makeup",    makeupSlider);
    mixAtt    = std::make_unique<SliderAttachment>   (s, "mix",       mixSlider);
    ratioAtt  = std::make_unique<ComboBoxAttachment> (s, "ratio",     ratioBox);
    bypassAtt = std::make_unique<ButtonAttachment>   (s, "bypass",    bypassButton);

    // Pin the text-box precision. Must run after the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23). dB / ms
    // to 2 dp; % as an integer.
    for (auto* sl : { &thresholdSlider, &attackSlider, &releaseSlider, &makeupSlider })
        factory_ui::setSliderDecimals (*sl, 2);
    factory_ui::setSliderDecimals (mixSlider, 0);

    setSize (580, 340);
}

BusCompressorAudioProcessorEditor::~BusCompressorAudioProcessorEditor()
{
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}

void BusCompressorAudioProcessorEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                                       const juce::String& name, const juce::String& suffix)
{
    factory_ui::styleKnob (slider, label, name, suffix);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void BusCompressorAudioProcessorEditor::refreshPresetSelector()
{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}

void BusCompressorAudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                               const ChangeDetails& details)
{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<BusCompressorAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    });
}

void BusCompressorAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, meter.getBounds());
}

void BusCompressorAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    presetSelector.setBounds (top);

    r.removeFromTop (10);

    // Gain-reduction meter on the left.
    meter.setBounds (r.removeFromLeft (96));
    r.removeFromLeft (14);

    // 3 x 2 grid of controls on the right.
    const int colW = r.getWidth() / 3;
    const int rowH = r.getHeight() / 2;

    auto cell = [&] (int col, int row) {
        return juce::Rectangle<int> (r.getX() + col * colW, r.getY() + row * rowH, colW, rowH).reduced (6);
    };

    auto layoutKnob = [] (juce::Rectangle<int> area, juce::Slider& s, juce::Label& l) {
        l.setBounds (area.removeFromTop (18));
        s.setBounds (area);
    };

    layoutKnob (cell (0, 0), thresholdSlider, thresholdLabel);
    layoutKnob (cell (1, 0), attackSlider,    attackLabel);
    layoutKnob (cell (2, 0), releaseSlider,   releaseLabel);
    layoutKnob (cell (0, 1), makeupSlider,    makeupLabel);
    layoutKnob (cell (1, 1), mixSlider,       mixLabel);

    // Ratio selector occupies the last cell.
    auto rc = cell (2, 1);
    ratioLabel.setBounds (rc.removeFromTop (18));
    ratioBox.setBounds (rc.withSizeKeepingCentre (rc.getWidth(), 28));
}
