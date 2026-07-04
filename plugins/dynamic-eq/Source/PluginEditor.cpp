#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

DynamicEqAudioProcessorEditor::DynamicEqAudioProcessorEditor (DynamicEqAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      curve (p, p.apvts), panel (p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("Dynamic EQ", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
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

    addAndMakeVisible (curve);
    addAndMakeVisible (panel);

    curve.onBandSelected = [this] (int b) { panel.setBand (b); };
    panel.setBand (curve.getSelectedBand());

    bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, "bypass", bypassButton);

    setResizable (true, true);
    setResizeLimits (620, 440, 1280, 900);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (740.0 / 520.0);
    setSize (740, 520);
}

DynamicEqAudioProcessorEditor::~DynamicEqAudioProcessorEditor()
{
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}

void DynamicEqAudioProcessorEditor::refreshPresetSelector()
{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}

void DynamicEqAudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                           const ChangeDetails& details)
{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<DynamicEqAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    });
}

void DynamicEqAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, curve.getBounds());
    factory_ui::dropShadowFor (g, panel.getBounds());
}

void DynamicEqAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (28);
    bypassButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (8);
    titleLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (8);
    presetSelector.setBounds (top);

    r.removeFromTop (10);
    panel.setBounds (r.removeFromBottom (170));
    r.removeFromBottom (12);
    curve.setBounds (r);
}
