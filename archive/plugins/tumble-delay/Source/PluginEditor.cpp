#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

namespace
{
    // Fixed layout metrics (everything else is sliced from getLocalBounds()).
    constexpr int kHeaderH    = 26;  // title / preset / bypass row
    constexpr int kTitleH     = 14;  // group caption row inside a card
    constexpr int kKnobRowH   = 86;  // caption 13 + knob body + text box 18
    constexpr int kChoiceRowH = 38;  // caption 13 + combo 22
    constexpr int kCardPad    = 8;
    constexpr int kGap        = 10;
    constexpr int kTabRowH    = 26;
    constexpr int kBankCardH  = kCardPad * 2 + kTitleH + kKnobRowH * 2;
}

TumbleDelayAudioProcessorEditor::TumbleDelayAudioProcessorEditor (TumbleDelayAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p), visualizer (p)
{
    setLookAndFeel (&lnf);

    const juce::String deg (juce::CharPointer_UTF8 ("\xc2\xb0")); // degree sign

    titleLabel.setText ("TUMBLE DELAY", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    addAndMakeVisible (visualizer);

    // ---- right column: World / Detect / Out ----
    styleAndAdd (shape,    "Shape");
    styleAndAdd (boxSize,  "Box Size", " x");
    styleAndAdd (sizeSync, "Size Sync");
    styleAndAdd (spin,     "Spin", " rev/s");
    styleAndAdd (spinSync, "Spin Sync");
    styleAndAdd (gravity,  "Gravity", " %");
    ballCollideButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (ballCollideButton);

    styleAndAdd (sense,  "Sense", " dB");
    styleAndAdd (retrig, "Retrig", " ms");
    styleAndAdd (spread, "Spread", " %");

    styleAndAdd (refeed, "Refeed", " %");
    styleAndAdd (tone,   "Tone", " Hz");
    styleAndAdd (mix,    "Mix", " %");
    styleAndAdd (output, "Output", " dB");

    // ---- bottom bank: slot tabs (select button + enable LED per slot) ----
    static constexpr const char* tabNames[4] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = tabButtons[(size_t) i];
        b.setButtonText (tabNames[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1001);
        b.setColour (juce::TextButton::buttonColourId,   FactoryLookAndFeel::panel());
        b.setColour (juce::TextButton::buttonOnColourId, FactoryLookAndFeel::bandColour (i));
        b.setColour (juce::TextButton::textColourOffId,  FactoryLookAndFeel::textDim());
        b.setColour (juce::TextButton::textColourOnId,   FactoryLookAndFeel::panel());
        b.onClick = [this, i] { selectSlot (i); };
        addAndMakeVisible (b);

        auto& led = tabLeds[(size_t) i];
        led.setColour (juce::ToggleButton::tickColourId, FactoryLookAndFeel::bandColour (i));
        addAndMakeVisible (led);
    }

    // ---- bottom bank: controls (styled once; re-attached per selected slot) ----
    styleAndAdd (bkCount,        "Balls", "");
    styleAndAdd (bkBallSize,     "Ball Size", " %");
    styleAndAdd (bkSpeed,        "Speed", " x");
    styleAndAdd (bkDirection,    "Direction", deg);
    styleAndAdd (bkDirRandom,    "Dir Random", " %");
    styleAndAdd (bkPreDelay,     "Pre-Delay", " ms");
    styleAndAdd (bkPreDelaySync, "PD Sync");
    styleAndAdd (bkTime,         "Time", " ms");
    styleAndAdd (bkTimeSync,     "Time Sync");
    styleAndAdd (bkBounce,       "Bounce", " %");
    styleAndAdd (bkDrag,         "Drag", " %");
    styleAndAdd (bkDecayCurve,   "Curve", "");
    styleAndAdd (bkLifeMode,     "Life Mode");
    styleAndAdd (bkLifeTime,     "Life Time", " s");
    styleAndAdd (bkLifeBounces,  "Life Bounces", "");
    styleAndAdd (bkMotion,       "Motion", " %");
    styleAndAdd (bkStep,         "Step", " ms");
    styleAndAdd (bkSpray,        "Spray", " ms");
    styleAndAdd (bkPitch,        "Pitch", " st");
    styleAndAdd (bkPitchRand,    "Pitch Rand", " st");
    styleAndAdd (bkGrain,        "Grain", " ms");
    styleAndAdd (bkReverse,      "Reverse", " %");
    styleAndAdd (bkPanMode,      "Pan Mode");
    styleAndAdd (bkGain,         "Slot Gain", " dB");

    // ---- attachments (each helper pins the text decimals AFTER attaching, #23) ----
    auto& s = processor.apvts;

    attachChoice (shape,    "boxShape");
    attachKnob   (boxSize,  "boxSize", 2);
    // The #23 decimals helper reformats the RAW value (seconds); Box Size shows
    // the geometric scale instead (1.00x = the 0.40 s reference box), so put the
    // multiplier conversion back on top of it.
    {
        constexpr double ref = factory_core::TumbleDelay::kReferenceBoxSizeSeconds;
        boxSize.slider.textFromValueFunction = [] (double v) { return juce::String (v / ref, 2); };
        boxSize.slider.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() * ref; };
        boxSize.slider.updateText();
    }
    attachChoice (sizeSync, "boxSizeSync");
    attachKnob   (spin,     "spin", 2);
    attachChoice (spinSync, "spinSync");
    attachKnob   (gravity,  "gravity", 0);
    ballCollideAtt = std::make_unique<ButtonAttachment> (s, "ballCollide", ballCollideButton);

    attachKnob (sense,  "sense", 2);
    attachKnob (retrig, "retrig", 0);
    attachKnob (spread, "spawnSpread", 0);

    attachKnob (refeed, "refeed", 0);
    attachKnob (tone,   "tone", 0);
    attachKnob (mix,    "mix", 0);
    attachKnob (output, "output", 2);

    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    for (int i = 0; i < 4; ++i)
        tabLedAtts[(size_t) i] = std::make_unique<ButtonAttachment> (
            s, juce::String (kSlotPrefix[i]) + "On", tabLeds[(size_t) i]);

    selectSlot (0); // binds the bank onto slot A + sets the tab toggle states

    setSize (1060, 714);
}

TumbleDelayAudioProcessorEditor::~TumbleDelayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

// -------------------------------------------------------------- build helpers

void TumbleDelayAudioProcessorEditor::styleAndAdd (Knob& k, const juce::String& name,
                                                   const juce::String& suffix)
{
    factory_ui::styleKnob (k.slider, k.label, name, suffix);
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
}

void TumbleDelayAudioProcessorEditor::styleAndAdd (Choice& c, const juce::String& name)
{
    c.label.setText (name, juce::dontSendNotification);
    c.label.setJustificationType (juce::Justification::centred);
    c.label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    addAndMakeVisible (c.box);
    addAndMakeVisible (c.label);
}

void TumbleDelayAudioProcessorEditor::attachKnob (Knob& k, const juce::String& paramID, int decimals)
{
    k.att.reset(); // never two attachments on one slider (the old one would echo
                   // the new slot's value back into the old parameter)
    k.att = std::make_unique<SliderAttachment> (processor.apvts, paramID, k.slider);
    factory_ui::setSliderDecimals (k.slider, decimals); // AFTER the attachment (#23)
}

void TumbleDelayAudioProcessorEditor::attachChoice (Choice& c, const juce::String& paramID)
{
    c.att.reset(); // as above: detach before repopulating / re-attaching
    if (auto* param = processor.apvts.getParameter (paramID))
    {
        // The attachment maps selected-item-index <-> normalised value linearly,
        // so the items must be exactly the parameter's choice strings, in order.
        c.box.clear (juce::dontSendNotification);
        c.box.addItemList (param->getAllValueStrings(), 1);
        c.att = std::make_unique<ComboBoxAttachment> (processor.apvts, paramID, c.box);
    }
}

// ------------------------------------------------------------------ slot bank

void TumbleDelayAudioProcessorEditor::selectSlot (int slot)
{
    selectedSlot = juce::jlimit (0, 3, slot);
    for (int i = 0; i < 4; ++i)
        tabButtons[(size_t) i].setToggleState (i == selectedSlot, juce::dontSendNotification);
    attachBank (selectedSlot);
    repaint(); // bank card captions are tinted with the slot colour
}

void TumbleDelayAudioProcessorEditor::attachBank (int slot)
{
    const juce::String pre (kSlotPrefix[juce::jlimit (0, 3, slot)]);

    // Physics
    attachKnob   (bkCount,        pre + "Count", 0);
    attachKnob   (bkBallSize,     pre + "BallSize", 0);
    attachKnob   (bkSpeed,        pre + "Speed", 2);
    attachKnob   (bkDirection,    pre + "Direction", 0);
    attachKnob   (bkDirRandom,    pre + "DirRandom", 0);
    attachKnob   (bkPreDelay,     pre + "PreDelay", 0);
    attachChoice (bkPreDelaySync, pre + "PreDelaySync");
    attachKnob   (bkTime,         pre + "Time", 0);
    attachChoice (bkTimeSync,     pre + "TimeSync");
    attachKnob   (bkBounce,       pre + "Bounce", 0);
    attachKnob   (bkDrag,         pre + "Drag", 0);
    attachKnob   (bkDecayCurve,   pre + "DecayCurve", 0);
    attachChoice (bkLifeMode,     pre + "LifeMode");
    attachKnob   (bkLifeTime,     pre + "LifeTime", 2);
    attachKnob   (bkLifeBounces,  pre + "LifeBounces", 0);
    // Source
    attachKnob   (bkMotion,       pre + "Motion", 0);
    attachKnob   (bkStep,         pre + "Step", 0);
    attachKnob   (bkSpray,        pre + "Spray", 0);
    // Sound
    attachKnob   (bkPitch,        pre + "Pitch", 1);
    attachKnob   (bkPitchRand,    pre + "PitchRand", 1);
    attachKnob   (bkGrain,        pre + "Grain", 0);
    attachKnob   (bkReverse,      pre + "Reverse", 0);
    attachChoice (bkPanMode,      pre + "PanMode");
    attachKnob   (bkGain,         pre + "Gain", 2);
}

// -------------------------------------------------------------------- painting

void TumbleDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());

    // The visualizer paints its own card; its shadow must come from the parent.
    if (! visualizer.getBounds().isEmpty())
        factory_ui::dropShadowFor (g, visualizer.getBounds());

    paintGroup (g, worldCard,  "WORLD");
    paintGroup (g, detectCard, "DETECT");
    paintGroup (g, outCard,    "OUT");

    const auto slotColour = FactoryLookAndFeel::bandColour (selectedSlot);
    paintGroup (g, physicsCard, "PHYSICS", slotColour);
    paintGroup (g, sourceCard,  "SOURCE",  slotColour);
    paintGroup (g, soundCard,   "SOUND",   slotColour);
}

void TumbleDelayAudioProcessorEditor::paintGroup (juce::Graphics& g, juce::Rectangle<int> card,
                                                  const juce::String& title, juce::Colour titleColour) const
{
    if (card.isEmpty()) return;
    factory_ui::dropShadowFor (g, card);
    factory_ui::paintCard (g, card.toFloat());

    auto inner = card.reduced (kCardPad);
    g.setColour (titleColour);
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText (title, inner.removeFromTop (kTitleH).withTrimmedLeft (2),
                juce::Justification::centredLeft);
}

// --------------------------------------------------------------------- layout

juce::Rectangle<int> TumbleDelayAudioProcessorEditor::col (juce::Rectangle<int> row, int n, int i)
{
    const int w = row.getWidth() / juce::jmax (1, n);
    return { row.getX() + i * w, row.getY(), w, row.getHeight() };
}

void TumbleDelayAudioProcessorEditor::placeKnob (Knob& k, juce::Rectangle<int> cell)
{
    cell = cell.reduced (2, 0);
    k.label.setBounds (cell.removeFromTop (13));
    k.slider.setBounds (cell);
}

void TumbleDelayAudioProcessorEditor::placeChoice (Choice& c, juce::Rectangle<int> cell)
{
    cell = cell.reduced (4, 0);
    const int blockH = 13 + 2 + 22;
    auto block = cell.withSizeKeepingCentre (cell.getWidth(), juce::jmin (cell.getHeight(), blockH));
    c.label.setBounds (block.removeFromTop (13));
    block.removeFromTop (2);
    c.box.setBounds (block.removeFromTop (22));
}

void TumbleDelayAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    // Header: title | preset selector | bypass.
    auto top = r.removeFromTop (kHeaderH);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (8);
    top.removeFromRight (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (kGap);

    // Bottom band (fixed height) first; the rest is the main area.
    auto bank = r.removeFromBottom (kTabRowH + 6 + kBankCardH);
    r.removeFromBottom (kGap);

    // Main area: visualizer left (~55%), right column of three groups.
    auto mainArea = r;
    visualizer.setBounds (mainArea.removeFromLeft (juce::roundToInt ((float) mainArea.getWidth() * 0.55f)));
    mainArea.removeFromLeft (12);

    const int worldH = kCardPad * 2 + kTitleH + kKnobRowH + kChoiceRowH;
    layoutWorld (mainArea.removeFromTop (worldH));
    mainArea.removeFromTop (8);
    const int half = (mainArea.getHeight() - 8) / 2;
    layoutDetect (mainArea.removeFromTop (half));
    mainArea.removeFromTop (8);
    layoutOut (mainArea);

    layoutBank (bank);
}

void TumbleDelayAudioProcessorEditor::layoutWorld (juce::Rectangle<int> area)
{
    worldCard = area;
    auto r = area.reduced (kCardPad);
    r.removeFromTop (kTitleH);
    auto knobRow = r.removeFromTop (kKnobRowH);
    auto syncRow = r.removeFromTop (kChoiceRowH);

    placeChoice (shape,   col (knobRow, 4, 0));
    placeKnob   (boxSize, col (knobRow, 4, 1));
    placeKnob   (spin,    col (knobRow, 4, 2));
    placeKnob   (gravity, col (knobRow, 4, 3));

    ballCollideButton.setBounds (col (syncRow, 4, 0).reduced (4, 2));
    placeChoice (sizeSync, col (syncRow, 4, 1)); // sits under its Box Size knob
    placeChoice (spinSync, col (syncRow, 4, 2)); // sits under its Spin knob
}

void TumbleDelayAudioProcessorEditor::layoutDetect (juce::Rectangle<int> area)
{
    detectCard = area;
    auto r = area.reduced (kCardPad);
    r.removeFromTop (kTitleH);

    placeKnob (sense,  col (r, 3, 0));
    placeKnob (retrig, col (r, 3, 1));
    placeKnob (spread, col (r, 3, 2));
}

void TumbleDelayAudioProcessorEditor::layoutOut (juce::Rectangle<int> area)
{
    outCard = area;
    auto r = area.reduced (kCardPad);
    r.removeFromTop (kTitleH);

    placeKnob (refeed, col (r, 4, 0));
    placeKnob (tone,   col (r, 4, 1));
    placeKnob (mix,    col (r, 4, 2));
    placeKnob (output, col (r, 4, 3));
}

void TumbleDelayAudioProcessorEditor::layoutBank (juce::Rectangle<int> area)
{
    auto tabs = area.removeFromTop (kTabRowH);
    for (int i = 0; i < 4; ++i)
    {
        auto cell = tabs.removeFromLeft (96);
        tabButtons[(size_t) i].setBounds (cell.removeFromLeft (52));
        cell.removeFromLeft (4);
        tabLeds[(size_t) i].setBounds (cell);
        tabs.removeFromLeft (10);
    }
    area.removeFromTop (6);

    // Physics 8 columns : Source 2 : Sound 3 (shared column unit).
    const int unit = (area.getWidth() - kGap * 2) / 13;
    layoutPhysics (area.removeFromLeft (unit * 8));
    area.removeFromLeft (kGap);
    layoutSource (area.removeFromLeft (unit * 2));
    area.removeFromLeft (kGap);
    layoutSound (area);
}

void TumbleDelayAudioProcessorEditor::layoutPhysics (juce::Rectangle<int> card)
{
    physicsCard = card;
    auto r = card.reduced (kCardPad);
    r.removeFromTop (kTitleH);
    auto row1 = r.removeFromTop (kKnobRowH);
    auto row2 = r;

    placeKnob (bkCount,     col (row1, 8, 0));
    placeKnob (bkBallSize,  col (row1, 8, 1));
    placeKnob (bkSpeed,     col (row1, 8, 2));
    placeKnob (bkDirection, col (row1, 8, 3));
    placeKnob (bkDirRandom, col (row1, 8, 4));
    placeKnob (bkPreDelay,  col (row1, 8, 5));
    placeKnob (bkTime,      col (row1, 8, 6));
    placeKnob (bkBounce,    col (row1, 8, 7));

    placeKnob   (bkDrag,         col (row2, 8, 0));
    placeKnob   (bkDecayCurve,   col (row2, 8, 1));
    placeChoice (bkLifeMode,     col (row2, 8, 2));
    placeKnob   (bkLifeTime,     col (row2, 8, 3));
    placeKnob   (bkLifeBounces,  col (row2, 8, 4));
    placeChoice (bkPreDelaySync, col (row2, 8, 5)); // under Pre-Delay
    placeChoice (bkTimeSync,     col (row2, 8, 6)); // under Time
}

void TumbleDelayAudioProcessorEditor::layoutSource (juce::Rectangle<int> card)
{
    sourceCard = card;
    auto r = card.reduced (kCardPad);
    r.removeFromTop (kTitleH);
    auto row1 = r.removeFromTop (kKnobRowH);
    auto row2 = r;

    placeKnob (bkMotion, col (row1, 2, 0));
    placeKnob (bkStep,   col (row1, 2, 1));
    placeKnob (bkSpray,  col (row2, 2, 0));
}

void TumbleDelayAudioProcessorEditor::layoutSound (juce::Rectangle<int> card)
{
    soundCard = card;
    auto r = card.reduced (kCardPad);
    r.removeFromTop (kTitleH);
    auto row1 = r.removeFromTop (kKnobRowH);
    auto row2 = r;

    placeKnob (bkPitch,     col (row1, 3, 0));
    placeKnob (bkPitchRand, col (row1, 3, 1));
    placeKnob (bkGrain,     col (row1, 3, 2));

    placeKnob   (bkReverse, col (row2, 3, 0));
    placeChoice (bkPanMode, col (row2, 3, 1));
    placeKnob   (bkGain,    col (row2, 3, 2));
}
