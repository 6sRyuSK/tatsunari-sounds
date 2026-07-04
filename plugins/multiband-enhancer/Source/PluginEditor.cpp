#include "PluginEditor.h"

MultibandEnhancerAudioProcessorEditor::MultibandEnhancerAudioProcessorEditor (MultibandEnhancerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), analyzer (p)
{
    setLookAndFeel (&lnf);

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

    addAndMakeVisible (analyzer);

    for (int b = 0; b < 5; ++b)
    {
        strips[(size_t) b] = std::make_unique<BandStripComponent> (processor, b);
        addAndMakeVisible (*strips[(size_t) b]);
    }

    // Quality choice box (Mode is now per-band on each strip).
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
    fillChoice (qualityBox, qualityLabel, "Quality", { "HQ", "Zero Latency" }, "quality", qualityAtt);
    fillChoice (phaseBox, phaseLabel, "Xover Phase", { "Standard", "Linear" }, "phase", phaseAtt);

    // Linear phase is HQ-only; grey the selector out whenever Zero-Latency is picked
    // (fires for both user edits and host automation of the Quality box).
    qualityBox.onChange = [this] { updatePhaseEnablement(); };
    updatePhaseEnablement();

    // Single Mix knob (dry/enhanced blend) replaces the Direct + Enhanced faders.
    factory_ui::styleKnob (mixKnob, mixLabel, "Mix", " %");
    addAndMakeVisible (mixKnob);
    addAndMakeVisible (mixLabel);
    mixAtt = std::make_unique<SliderAtt> (processor.apvts, "mix", mixKnob);
    factory_ui::setSliderDecimals (mixKnob, 0);

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
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}

void MultibandEnhancerAudioProcessorEditor::refreshPresetSelector()
{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}

void MultibandEnhancerAudioProcessorEditor::updatePhaseEnablement()
{
    const bool zeroLatency = qualityBox.getSelectedItemIndex() == 1; // 0 = HQ, 1 = ZL
    phaseBox.setEnabled (! zeroLatency);
    phaseLabel.setEnabled (! zeroLatency);
    phaseBox.setTooltip (zeroLatency ? "Linear-phase crossover requires HQ quality"
                                     : "Linear-phase FIR crossover (mastering; adds ~43 ms latency)");
}

void MultibandEnhancerAudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                                   const ChangeDetails& details)
{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<MultibandEnhancerAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    });
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

    // Title band: the title text is painted top-left (see paint); drop the shared
    // preset selector into the right of the same 34px band (house top-row style).
    auto titleBand = r.removeFromTop (34).reduced (16, 6);
    presetSelector.setBounds (titleBand.removeFromRight (juce::jmin (260, titleBand.getWidth() / 2)));

    r.reduce (10, 8);

    // Right control card.
    controlCard = r.removeFromRight (150);
    r.removeFromRight (8);
    {
        auto c = controlCard.reduced (10);
        c.removeFromTop (4);

        auto mixArea = c.removeFromTop (94);
        mixKnob.setBounds (mixArea.removeFromTop (76));
        mixLabel.setBounds (mixArea);
        c.removeFromTop (10);

        auto out = c.removeFromTop (94);
        outputKnob.setBounds (out.removeFromTop (76));
        outputLabel.setBounds (out);
        c.removeFromTop (12);

        qualityLabel.setBounds (c.removeFromTop (16));
        qualityBox.setBounds (c.removeFromTop (24));
        c.removeFromTop (10);

        phaseLabel.setBounds (c.removeFromTop (16));
        phaseBox.setBounds (c.removeFromTop (24));
        c.removeFromTop (12);

        deltaButton.setBounds (c.removeFromTop (22));
        c.removeFromTop (6);
        bypassButton.setBounds (c.removeFromTop (22));
    }

    // Left: analyser over the band strip row (taller strips carry the per-band
    // mode selector + solo toggle in addition to the Enhance / Width knobs).
    auto bandRow = r.removeFromBottom (214);
    analyzer.setBounds (r);

    const int sw = bandRow.getWidth() / 5;
    for (int b = 0; b < 5; ++b)
    {
        auto cell = (b < 4) ? bandRow.removeFromLeft (sw) : bandRow;
        strips[(size_t) b]->setBounds (cell);
    }
}
