#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

ResonanceSuppressorAudioProcessorEditor::ResonanceSuppressorAudioProcessorEditor (ResonanceSuppressorAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p), curve (p, p.apvts)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("Resonance Suppressor", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    deltaB.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff45b8acu)); // teal, matches the reduction trace
    addAndMakeVisible (deltaB);
    addAndMakeVisible (linkB);
    addAndMakeVisible (bypassB);

    // Detection mode selector. Items must be added manually (the attachment does
    // not populate the box in this JUCE version — same pattern as the other
    // plugins' combos); item IDs 1,2 map to parameter indices 0,1 (Soft, Hard).
    modeBox.addItemList ({ "Soft", "Hard" }, 1);
    modeBox.setJustificationType (juce::Justification::centred);
    modeBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
    modeBox.setTooltip ("Soft: adaptive, level-independent.  Hard: absolute level (Depth = threshold).");
    addAndMakeVisible (modeBox);
    modeAtt = std::make_unique<CA> (processor.apvts, "mode", modeBox);

    // Quality selector, sitting left of Mode. Items added manually like modeBox;
    // item IDs 1..3 map to parameter indices 0..2 (Fast, Normal, High). Tooltip
    // only (no modal) — the trade-off is latency vs. low-frequency resolution.
    qualityBox.addItemList ({ "Fast", "Normal", "High" }, 1);
    qualityBox.setJustificationType (juce::Justification::centred);
    qualityBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
    qualityBox.setTooltip ("Fast: half latency, half low-frequency resolution. High: double resolution, double latency.");
    addAndMakeVisible (qualityBox);
    qualityAtt = std::make_unique<CA> (processor.apvts, "quality", qualityBox);

    // --- Second header row: Mode/Quality/Channel/Sidechain/SC Listen/Delta/Link + Link Amt. ---
    // Channel mode combo. Manual addItemList like the others; item IDs 1,2 -> 0,1.
    channelBox.addItemList ({ "Stereo", "Mid-Side" }, 1);
    channelBox.setJustificationType (juce::Justification::centred);
    channelBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
    channelBox.setTooltip ("Stereo: process L/R. Mid-Side: process the M/S encode (bypass stays bit-transparent).");
    addAndMakeVisible (channelBox);
    channelAtt = std::make_unique<CA> (processor.apvts, "channelMode", channelBox);

    scEnableB.setTooltip ("Key detection off the Sidechain input bus (falls back to internal when unpatched).");
    scListenB.setTooltip ("Monitor the raw sidechain (delayed to the plugin latency).");
    addAndMakeVisible (scEnableB);
    addAndMakeVisible (scListenB);
    scEnableAtt = std::make_unique<BA> (processor.apvts, "scEnable", scEnableB);
    scListenAtt = std::make_unique<BA> (processor.apvts, "scListen", scListenB);

    // Link Amount: horizontal slider with a small caption on its left. Colours come
    // from the LookAndFeel; % integer text (setSliderDecimals must run AFTER the
    // attachment — see #23).
    linkAmtL.setText ("Link Amt", juce::dontSendNotification);
    linkAmtL.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    linkAmtL.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (linkAmtL);
    linkAmtS.setSliderStyle (juce::Slider::LinearHorizontal);
    linkAmtS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 18);
    linkAmtS.setColour (juce::Slider::textBoxTextColourId, FactoryLookAndFeel::text());
    linkAmtS.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    linkAmtS.setTextValueSuffix (" %");
    addAndMakeVisible (linkAmtS);
    linkAmtAtt = std::make_unique<SA> (processor.apvts, "linkAmt", linkAmtS);
    factory_ui::setSliderDecimals (linkAmtS, 0); // % integer, after the attachment (#23)

    // --- Phase 5b-1: A/B compare. A two-way radio pair (mutually-exclusive
    // highlight via a shared radio group ID) plus a Copy button that copies the
    // active slot's live state onto the inactive one. Palette only: panel() for
    // the unselected fill, accent() for the selected fill, text()/white for the
    // label so the selected pill reads clearly against the coral fill.
    for (auto* b : { &abAButton, &abBButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x4142, juce::dontSendNotification); // 'AB'
        b->setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
        b->setColour (juce::TextButton::buttonOnColourId, FactoryLookAndFeel::accent());
        b->setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::text());
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        addAndMakeVisible (*b);
    }
    abAButton.setToggleState (true, juce::dontSendNotification); // matches abActive == 0 at construction
    abAButton.setTooltip ("Switch to comparison slot A.");
    abBButton.setTooltip ("Switch to comparison slot B.");
    abAButton.onClick = [this] { processor.setABSlot (0); updateABUI(); };
    abBButton.onClick = [this] { processor.setABSlot (1); updateABUI(); };

    abCopyButton.setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
    abCopyButton.setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::accent());
    addAndMakeVisible (abCopyButton);
    abCopyButton.onClick = [this] { processor.copyActiveToOther(); updateABUI(); };

    // --- Phase 5b-2: Undo/Redo. UndoManager lives on the processor (declared
    // ahead of apvts so the APVTS ctor can bind to it -- see PluginProcessor.h);
    // enabled state is refreshed on click and by the idle-transaction timer
    // below (timerCallback()), which also covers host-driven parameter changes.
    for (auto* b : { &undoButton, &redoButton })
    {
        b->setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
        b->setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::text());
        addAndMakeVisible (*b);
    }
    undoButton.setTooltip ("Undo the last parameter change.");
    redoButton.setTooltip ("Redo.");
    undoButton.onClick = [this] { processor.getUndoManager().undo(); refreshUndoRedoButtons(); };
    redoButton.onClick = [this] { processor.getUndoManager().redo(); refreshUndoRedoButtons(); };

    addAndMakeVisible (curve);

    addKnob (depthS, depthL, "Depth",       " %",  "depth");
    addKnob (sharpS, sharpL, "Sharpness",   " %",  "sharpness");
    addKnob (selS,   selL,   "Selectivity", " %",  "selectivity");
    addKnob (atkS,   atkL,   "Attack",      " ms", "attack");
    addKnob (relS,   relL,   "Release",     " ms", "release");
    addKnob (tiltS,  tiltL,  "Tilt",        " %",  "tilt");
    addKnob (mixS,   mixL,   "Mix",         " %",  "mix");

    deltaAtt  = std::make_unique<BA> (processor.apvts, "delta",  deltaB);
    linkAtt   = std::make_unique<BA> (processor.apvts, "link",   linkB);
    bypassAtt = std::make_unique<BA> (processor.apvts, "bypass", bypassB);

    updateABUI();
    refreshUndoRedoButtons();
    startTimer (500); // idle-transaction boundary + Undo/Redo enable refresh (Phase 5b-2)

    setResizable (true, true);
    // Phase 5b: the busier 2-row header (A/B + Undo/Redo added to row 1; Mode/
    // Quality/Delta/Link moved down into row 2 alongside Channel/Sidechain/SC
    // Listen/Link Amt) no longer fits the old 640x440 floor -- measured against
    // the actual row-1/row-2 control widths below (see resized()), 860x590 is
    // the smallest size that lays out every control without overlap or clipping
    // (verified against a Standalone build). Aspect ratio re-derived from that
    // floor (was 760/520) rather than kept arbitrarily.
    setResizeLimits (860, 590, 1300, 892);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (860.0 / 590.0);
    setSize (1032, 708); // default 20% larger than the 860x590 reference (same aspect)
}

ResonanceSuppressorAudioProcessorEditor::~ResonanceSuppressorAudioProcessorEditor()
{
    stopTimer();
    processor.setListenNode (-1); // Phase 5a-2: never leave Listen soloed after the editor closes
    setLookAndFeel (nullptr);
}

void ResonanceSuppressorAudioProcessorEditor::timerCallback()
{
    // Idle-transaction boundary (Phase 5b-2): the APVTS's own value -> ValueTree
    // flush timer routes every parameter write through the UndoManager, but
    // without transaction breaks a whole session collapses into one undo step.
    // Closing the transaction here every 500 ms turns each burst of edits
    // (a knob drag, a curve-node drag, a click) into its own discrete step
    // without needing per-gesture hooks into every attachment / the curve's
    // own begin/endGesture calls.
    processor.getUndoManager().beginNewTransaction();
    refreshUndoRedoButtons();
}

void ResonanceSuppressorAudioProcessorEditor::refreshUndoRedoButtons()
{
    auto& um = processor.getUndoManager();
    undoButton.setEnabled (um.canUndo());
    redoButton.setEnabled (um.canRedo());
}

void ResonanceSuppressorAudioProcessorEditor::updateABUI()
{
    const bool isA = processor.getABSlot() == 0;
    abAButton.setToggleState (isA, juce::dontSendNotification);
    abBButton.setToggleState (! isA, juce::dontSendNotification);
    abCopyButton.setButtonText (isA ? "Copy A>B" : "Copy B>A");
    abCopyButton.setTooltip (juce::String ("Copy slot ") + (isA ? "A" : "B") + " onto slot " + (isA ? "B" : "A") + ".");
}

void ResonanceSuppressorAudioProcessorEditor::addKnob (juce::Slider& s, juce::Label& l,
                                                       const juce::String& name, const juce::String& suffix,
                                                       const juce::String& id)
{
    factory_ui::styleKnob (s, l, name, suffix);
    addAndMakeVisible (s);
    addAndMakeVisible (l);
    knobAtts.push_back (std::make_unique<SA> (processor.apvts, id, s));
    // % and frequency read as integers; dB / ms to 2 dp. Must run after the
    // attachment, which otherwise formats with up to 7 decimals (see #23).
    const bool integer = suffix.containsIgnoreCase ("%") || suffix.containsIgnoreCase ("Hz");
    factory_ui::setSliderDecimals (s, integer ? 0 : 2);
}

void ResonanceSuppressorAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
    factory_ui::dropShadowFor (g, curve.getBounds());
}

void ResonanceSuppressorAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    // ---- Header row 1: title (left) / presets (centre, stretches) / A|B +
    // Copy / Undo|Redo / Bypass (right). ----
    auto top = r.removeFromTop (28);
    bypassB.setBounds (top.removeFromRight (98));
    top.removeFromRight (10);
    redoButton.setBounds (top.removeFromRight (50));
    top.removeFromRight (4);
    undoButton.setBounds (top.removeFromRight (50));
    top.removeFromRight (12);
    abCopyButton.setBounds (top.removeFromRight (78));
    top.removeFromRight (6);
    abBButton.setBounds (top.removeFromRight (26));
    top.removeFromRight (2);
    abAButton.setBounds (top.removeFromRight (26));
    top.removeFromRight (12);
    titleLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (10);
    presetController.selector().setBounds (top);

    // ---- Header row 2: Mode / Quality / Channel / Sidechain / SC Listen /
    // Delta / Link + Link Amt. Mode/Quality/Delta/Link moved down here from row
    // 1 (Phase 5b) to make room for A/B + Undo/Redo above; each toggle pill
    // keeps the width it was tuned to (pill + full caption, see #25) since only
    // the row changed, not the control. ----
    auto row2 = r.removeFromTop (26);
    modeBox.setBounds (row2.removeFromLeft (94));
    row2.removeFromLeft (6);
    qualityBox.setBounds (row2.removeFromLeft (80));
    row2.removeFromLeft (10);
    channelBox.setBounds (row2.removeFromLeft (100));
    row2.removeFromLeft (6);
    scEnableB.setBounds (row2.removeFromLeft (106));
    row2.removeFromLeft (6);
    scListenB.setBounds (row2.removeFromLeft (100));
    row2.removeFromLeft (10);
    deltaB.setBounds (row2.removeFromLeft (86));
    row2.removeFromLeft (6);
    linkB.setBounds (row2.removeFromLeft (82));
    row2.removeFromLeft (10);
    // Link Amount takes whatever remains on the right: caption + horizontal slider.
    linkAmtL.setBounds (row2.removeFromLeft (56));
    linkAmtS.setBounds (row2);
    r.removeFromTop (8);

    r.removeFromTop (10);
    // Bottom control row at ~80% of its former height so the analyser gets the
    // extra vertical space. The row is centred horizontally so the smaller knobs
    // don't stretch edge-to-edge.
    auto knobs = r.removeFromBottom (83);
    r.removeFromBottom (12);
    curve.setBounds (r);

    juce::Slider* sl[] = { &depthS, &sharpS, &selS, &atkS, &relS, &tiltS, &mixS };
    juce::Label*  lb[] = { &depthL, &sharpL, &selL, &atkL, &relL, &tiltL, &mixL };
    const int n = (int) std::size (sl);
    const int cw = juce::jmin (96, knobs.getWidth() / n);
    knobs = knobs.withSizeKeepingCentre (cw * n, knobs.getHeight());
    for (int i = 0; i < n; ++i)
    {
        auto cell = (i == n - 1) ? knobs : knobs.removeFromLeft (cw);
        cell = cell.reduced (4, 0);
        lb[i]->setBounds (cell.removeFromTop (16));
        sl[i]->setBounds (cell);
    }
}
