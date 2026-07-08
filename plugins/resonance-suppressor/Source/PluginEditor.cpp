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

    // --- Second header row (Pass 3B routing) — minimal placement; full layout later. ---
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

    setResizable (true, true);
    setResizeLimits (640, 440, 1280, 900);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (760.0 / 520.0);
    setSize (912, 624); // default 20% larger than the 760x520 reference (same aspect)
}

ResonanceSuppressorAudioProcessorEditor::~ResonanceSuppressorAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
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

    auto top = r.removeFromTop (28);
    // Each pill reserves ~42px (toggle box + gap) before its caption, so give
    // every toggle room for its full label — "Link" was clipped at 64px (see #25).
    bypassB.setBounds (top.removeFromRight (98));
    top.removeFromRight (6);
    linkB.setBounds (top.removeFromRight (82));
    top.removeFromRight (6);
    deltaB.setBounds (top.removeFromRight (86));
    top.removeFromRight (10);
    modeBox.setBounds (top.removeFromRight (104));
    top.removeFromRight (6);
    // Quality sits just left of Mode; the preset selector takes whatever remains in
    // the middle and shrinks to make room (minimal change — full layout is a later phase).
    qualityBox.setBounds (top.removeFromRight (86));
    top.removeFromRight (10);
    titleLabel.setBounds (top.removeFromLeft (210));
    top.removeFromLeft (10);
    presetController.selector().setBounds (top);

    // Second slim header row (Pass 3B routing), directly under the top row: channel mode
    // + sidechain toggles on the left, Link Amount pinned right. The analyser loses
    // 26+8 px of height for it (minimal placement — full routing layout is a later phase).
    auto row2 = r.removeFromTop (26);
    channelBox.setBounds (row2.removeFromLeft (110));
    row2.removeFromLeft (6);
    scEnableB.setBounds (row2.removeFromLeft (106));
    row2.removeFromLeft (6);
    scListenB.setBounds (row2.removeFromLeft (100));
    auto linkArea = row2.removeFromRight (200); // "Link Amt" caption + horizontal slider
    linkAmtL.setBounds (linkArea.removeFromLeft (56));
    linkAmtS.setBounds (linkArea);
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
