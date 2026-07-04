#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

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

    addAndMakeVisible (visualizer);

    // Decimals: % integer; cutoff-Hz integer; s / ms / rate-Hz to 2 dp.
    addKnob ("size",     "Size",      " %",  0);
    addKnob ("decay",    "Decay",     " s",  2);
    addKnob ("damping",  "Damping",   " %",  0);
    addKnob ("predelay", "Pre-Delay", " ms", 2);
    addKnob ("mix",      "Mix",       " %",  0);
    addKnob ("shimmer",  "Shimmer",   " %",  0);
    addKnob ("voicemix", "Voice Mix", " %",  0);
    addKnob ("lowcut",   "Low Cut",   " Hz", 0);
    addKnob ("highcut",  "High Cut",  " Hz", 0);
    addKnob ("modrate",  "Mod Rate",  " Hz", 2);
    addKnob ("moddepth", "Mod Depth", " %",  0);

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
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}

void ShimmerReverbAudioProcessorEditor::refreshPresetSelector()
{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}

void ShimmerReverbAudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                               const ChangeDetails& details)
{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<ShimmerReverbAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    });
}

void ShimmerReverbAudioProcessorEditor::addKnob (const char* id, const char* name, const char* suffix, int decimals)
{
    auto slider = std::make_unique<juce::Slider>();
    auto label  = std::make_unique<juce::Label>();
    // styleKnob sets the value-box text colour on the slider itself; without it
    // the box keeps the stale (white) colour the default LookAndFeel baked in at
    // construction, since the editor's LnF isn't re-applied to it (see #54 white text).
    factory_ui::styleKnob (*slider, *label, name, suffix);
    addAndMakeVisible (*slider);
    addAndMakeVisible (*label);

    knobAtts.push_back (std::make_unique<SliderAttachment> (processor.apvts, id, *slider));
    // Pin the text-box precision. Must run after the attachment, which otherwise
    // formats continuous ranges with up to 7 decimals (see #23/#26).
    factory_ui::setSliderDecimals (*slider, decimals);
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
    label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (label);
}

void ShimmerReverbAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, visualizer.getBounds());
}

void ShimmerReverbAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    freezeButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    titleLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    presetSelector.setBounds (top);

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
