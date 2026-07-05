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

    addAndMakeVisible (curve);

    addKnob (depthS, depthL, "Depth",     " %",  "depth");
    addKnob (sharpS, sharpL, "Sharpness", " %",  "sharpness");
    addKnob (atkS,   atkL,   "Attack",    " ms", "attack");
    addKnob (relS,   relL,   "Release",   " ms", "release");
    addKnob (mixS,   mixL,   "Mix",       " %",  "mix");

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
    top.removeFromRight (10);
    titleLabel.setBounds (top.removeFromLeft (210));
    top.removeFromLeft (10);
    presetController.selector().setBounds (top);

    r.removeFromTop (10);
    // Bottom control row at ~80% of its former height so the analyser gets the
    // extra vertical space. The row is centred horizontally so the smaller knobs
    // don't stretch edge-to-edge.
    auto knobs = r.removeFromBottom (83);
    r.removeFromBottom (12);
    curve.setBounds (r);

    juce::Slider* sl[] = { &depthS, &sharpS, &atkS, &relS, &mixS };
    juce::Label*  lb[] = { &depthL, &sharpL, &atkL, &relL, &mixL };
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
