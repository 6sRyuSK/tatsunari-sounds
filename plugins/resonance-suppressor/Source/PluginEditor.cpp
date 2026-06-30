#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

ResonanceSuppressorAudioProcessorEditor::ResonanceSuppressorAudioProcessorEditor (ResonanceSuppressorAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), curve (p, p.apvts)
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

    addAndMakeVisible (curve);

    addKnob (depthS, depthL, "Depth",     " %",  "depth");
    addKnob (sharpS, sharpL, "Sharpness", " %",  "sharpness");
    addKnob (lowS,   lowL,   "Low",       " Hz", "lowfreq");
    addKnob (highS,  highL,  "High",      " Hz", "highfreq");
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
    setSize (760, 520);
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
    s.setNumDecimalPlacesToDisplay (id.containsIgnoreCase ("freq") ? 0 : 2);
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
    bypassB.setBounds (top.removeFromRight (84));
    top.removeFromRight (4);
    linkB.setBounds (top.removeFromRight (64));
    top.removeFromRight (4);
    deltaB.setBounds (top.removeFromRight (74));
    titleLabel.setBounds (top);

    r.removeFromTop (10);
    auto knobs = r.removeFromBottom (104);
    r.removeFromBottom (12);
    curve.setBounds (r);

    juce::Slider* sl[] = { &depthS, &sharpS, &lowS, &highS, &atkS, &relS, &mixS };
    juce::Label*  lb[] = { &depthL, &sharpL, &lowL, &highL, &atkL, &relL, &mixL };
    const int n = (int) std::size (sl);
    const int cw = knobs.getWidth() / n;
    for (int i = 0; i < n; ++i)
    {
        auto cell = (i == n - 1) ? knobs : knobs.removeFromLeft (cw);
        cell = cell.reduced (4, 0);
        lb[i]->setBounds (cell.removeFromTop (16));
        sl[i]->setBounds (cell);
    }
}
