#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

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

    addAndMakeVisible (cloud);

    // Order matters for layout. Decimals: % integer; ms / st(fine) / rate-Hz to
    // 2 dp; pitch is whole semitones (0 dp).
    addKnob ("delay",     "Delay",      " ms", 2);
    addKnob ("feedback",  "Feedback",   " %",  0);
    addKnob ("mix",       "Mix",        " %",  0);
    addKnob ("grainsize", "Grain",      " ms", 2);
    addKnob ("density",   "Density",    " Hz", 2);
    addKnob ("jitter",    "Jitter",     " ms", 2);
    addKnob ("pitch",     "Pitch",      " st", 0);
    addKnob ("pitchrand", "Pitch Rnd",  " st", 2);
    addKnob ("spread",    "Spread",     " %",  0);
    addKnob ("lforate",   "LFO Rate",   " Hz", 2);
    addKnob ("lfodepth",  "LFO Depth",  " %",  0);

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
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}

void GranularDelayAudioProcessorEditor::refreshPresetSelector()
{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}

void GranularDelayAudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                               const ChangeDetails& details)
{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<GranularDelayAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    });
}

void GranularDelayAudioProcessorEditor::addKnob (const char* id, const char* name, const char* suffix, int decimals)
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

void GranularDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, cloud.getBounds());
}

void GranularDelayAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    presetSelector.setBounds (top);

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
