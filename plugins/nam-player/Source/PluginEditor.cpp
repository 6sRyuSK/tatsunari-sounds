#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

NamPlayerAudioProcessorEditor::NamPlayerAudioProcessorEditor (NamPlayerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    title.setText ("NAM PLAYER", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (title);

    for (int k = 0; k < kNumSlots; ++k)
    {
        const size_t s = (size_t) k;

        slotHeader[s].setText ("NAM " + juce::String (k + 1), juce::dontSendNotification);
        slotHeader[s].setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        slotHeader[s].setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
        addAndMakeVisible (slotHeader[s]);

        slotNameLabel[s].setFont (juce::Font (juce::FontOptions (12.0f)));
        slotNameLabel[s].setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
        addAndMakeVisible (slotNameLabel[s]);

        slotLoad[s].setButtonText ("Load");
        slotLoad[s].onClick = [this, k] { openModelChooser (k); };
        addAndMakeVisible (slotLoad[s]);

        slotClear[s].setButtonText ("X");
        slotClear[s].onClick = [this, k] { processor.clearModel (k); refreshNames(); };
        addAndMakeVisible (slotClear[s]);

        slotEnable[s].setButtonText ("On");
        slotEnable[s].setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
        addAndMakeVisible (slotEnable[s]);
        buttonAtts.push_back (std::make_unique<BA> (processor.apvts,
            NamPlayerAudioProcessor::slotPid (k, "enable"), slotEnable[s]));

        slotMode[s].addItem ("Series",   1);
        slotMode[s].addItem ("Parallel", 2);
        addAndMakeVisible (slotMode[s]);
        comboAtts.push_back (std::make_unique<CA> (processor.apvts,
            NamPlayerAudioProcessor::slotPid (k, "mode"), slotMode[s]));

        addKnob (slotIn[s],  slotInL[s],  "In",    " dB", NamPlayerAudioProcessor::slotPid (k, "ingain"),  2);
        addKnob (slotOut[s], slotOutL[s], "Level", " dB", NamPlayerAudioProcessor::slotPid (k, "out"),     2);
        addKnob (slotBal[s], slotBalL[s], "Bal",   "",    NamPlayerAudioProcessor::slotPid (k, "balance"), 2);
    }

    irHeader.setText ("CAB IR", juce::dontSendNotification);
    irHeader.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    irHeader.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (irHeader);

    irNameLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    irNameLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (irNameLabel);

    irLoad.onClick = [this] { openIrChooser(); };
    addAndMakeVisible (irLoad);
    irClear.onClick = [this] { processor.clearIr(); refreshNames(); };
    addAndMakeVisible (irClear);

    addKnob (inTrim,  inTrimL,  "Input",  " dB", "in_trim",    2);
    addKnob (irLevel, irLevelL, "IR Lvl", " dB", "ir_level",   2);
    addKnob (outGain, outGainL, "Output", " dB", "out_gain",   2);
    addKnob (mix,     mixL,     "Mix",    " %",  "mix",        0);
    addKnob (loCut,   loCutL,   "Lo Cut", " Hz", "tone_locut", 0);
    addKnob (hiCut,   hiCutL,   "Hi Cut", " Hz", "tone_hicut", 0);

    irEnable.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (irEnable);
    buttonAtts.push_back (std::make_unique<BA> (processor.apvts, "ir_enable", irEnable));

    bypass.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypass);
    buttonAtts.push_back (std::make_unique<BA> (processor.apvts, "bypass", bypass));

    mergeHeader.setText ("MERGE", juce::dontSendNotification);
    mergeHeader.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    mergeHeader.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (mergeHeader);

    reampButton.onClick = [this] { startReamp(); };
    addAndMakeVisible (reampButton);

    reampIrTone.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (reampIrTone);

    refreshNames();
    startTimerHz (4);   // refresh names after async (state-restore) loads
    setSize (780, 648);
}

NamPlayerAudioProcessorEditor::~NamPlayerAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void NamPlayerAudioProcessorEditor::addKnob (juce::Slider& slider, juce::Label& label,
                                             const juce::String& name, const juce::String& suffix,
                                             const juce::String& paramId, int decimals)
{
    factory_ui::styleKnob (slider, label, name, suffix);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
    sliderAtts.push_back (std::make_unique<SA> (processor.apvts, paramId, slider));
    factory_ui::setSliderDecimals (slider, decimals);
}

void NamPlayerAudioProcessorEditor::openModelChooser (int slot)
{
    chooser = std::make_unique<juce::FileChooser> ("Load NAM model", juce::File(), "*.nam");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, slot] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { processor.loadModel (slot, f); refreshNames(); }
        });
}

void NamPlayerAudioProcessorEditor::openIrChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load cabinet IR", juce::File(), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { processor.loadIr (f); refreshNames(); }
        });
}

void NamPlayerAudioProcessorEditor::startReamp()
{
    chooser = std::make_unique<juce::FileChooser> ("リアンプ入力 WAV を選択 (48 kHz)", juce::File(), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile()) chooseReampOutput (f);
        });
}

void NamPlayerAudioProcessorEditor::chooseReampOutput (const juce::File& inputFile)
{
    const auto def = inputFile.getParentDirectory()
                        .getChildFile (inputFile.getFileNameWithoutExtension() + "_reamp.wav");
    chooser = std::make_unique<juce::FileChooser> ("リアンプ出力 WAV を保存", def, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, inputFile] (const juce::FileChooser& fc)
        {
            const auto out = fc.getResult();
            if (out != juce::File()) runReampJob (inputFile, out);
        });
}

void NamPlayerAudioProcessorEditor::runReampJob (const juce::File& in, const juce::File& out)
{
    reampButton.setEnabled (false);
    reampButton.setButtonText ("Rendering...");

    const bool inclIrTone = reampIrTone.getToggleState();
    auto* proc = &processor;
    juce::Component::SafePointer<NamPlayerAudioProcessorEditor> safe (this);

    // Fire-and-forget background render; UI is touched only via the message thread.
    juce::Thread::launch ([proc, safe, in, out, inclIrTone]
    {
        const auto result = proc->renderReampToFile (in, out, inclIrTone, nullptr);
        juce::MessageManager::callAsync ([safe, result]
        {
            if (auto* ed = safe.getComponent())
            {
                ed->reampButton.setEnabled (true);
                ed->reampButton.setButtonText ("Reamp Export...");
                juce::NativeMessageBox::showMessageBoxAsync (
                    result.ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                    result.ok ? "MERGE" : "MERGE — 失敗", result.message, ed);
            }
        });
    });
}

void NamPlayerAudioProcessorEditor::refreshNames()
{
    for (int k = 0; k < kNumSlots; ++k)
    {
        const auto nm = processor.slotName (k);
        slotNameLabel[(size_t) k].setText (nm.isEmpty() ? "- empty -" : nm, juce::dontSendNotification);
    }
    const auto ir = processor.irName();
    irNameLabel.setText (ir.isEmpty() ? "- empty -" : ir, juce::dontSendNotification);
}

void NamPlayerAudioProcessorEditor::timerCallback()
{
    refreshNames();
}

void NamPlayerAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void NamPlayerAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (28);
    bypass.setBounds (top.removeFromRight (96));
    top.removeFromRight (8);
    title.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);
    r.removeFromTop (8);

    auto knobIn = [] (juce::Rectangle<int> a, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (a.removeFromTop (16));
        s.setBounds (a.reduced (6, 0));
    };

    for (int k = 0; k < kNumSlots; ++k)
    {
        const size_t s = (size_t) k;
        auto row = r.removeFromTop (128);
        auto left = row.removeFromLeft (184);
        slotHeader[s].setBounds (left.removeFromTop (22));
        slotNameLabel[s].setBounds (left.removeFromTop (18));
        auto lb = left.removeFromTop (26);
        slotClear[s].setBounds (lb.removeFromRight (28).reduced (2));
        slotLoad[s].setBounds (lb.reduced (2));
        slotEnable[s].setBounds (left.removeFromTop (24).reduced (2));
        slotMode[s].setBounds (left.removeFromTop (26).reduced (2, 2));

        const int cw = row.getWidth() / 3;
        knobIn (row.removeFromLeft (cw), slotIn[s],  slotInL[s]);
        knobIn (row.removeFromLeft (cw), slotOut[s], slotOutL[s]);
        knobIn (row,                     slotBal[s], slotBalL[s]);
        r.removeFromTop (4);
    }

    auto g = r;

    // MERGE row reserved from the bottom so the knobs above don't consume it.
    auto mergeRow = g.removeFromBottom (30);
    g.removeFromBottom (8);
    mergeHeader.setBounds (mergeRow.removeFromLeft (70));
    reampButton.setBounds (mergeRow.removeFromLeft (150).reduced (2));
    reampIrTone.setBounds (mergeRow.reduced (6, 2));

    auto irRow = g.removeFromTop (24);
    irHeader.setBounds (irRow.removeFromLeft (70));
    irClear.setBounds (irRow.removeFromRight (30).reduced (2));
    irLoad.setBounds  (irRow.removeFromRight (72).reduced (2));
    irEnable.setBounds (irRow.removeFromLeft (70));
    irNameLabel.setBounds (irRow);
    g.removeFromTop (6);

    juce::Slider* gs[] { &inTrim, &irLevel, &outGain, &mix, &loCut, &hiCut };
    juce::Label*  gl[] { &inTrimL, &irLevelL, &outGainL, &mixL, &loCutL, &hiCutL };
    const int cw = g.getWidth() / 6;
    for (int i = 0; i < 6; ++i)
        knobIn (i < 5 ? g.removeFromLeft (cw) : g, *gs[i], *gl[i]);
}
