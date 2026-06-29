#include "PluginEditor.h"

VocalMbCompAudioProcessorEditor::VocalMbCompAudioProcessorEditor (VocalMbCompAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      meterLow (p, 0), meterMid (p, 1), meterHigh (p, 2)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("VOCAL MB COMP", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    auto setupBandName = [this] (juce::Label& l, const juce::String& t) {
        l.setText (t, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
        l.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        addAndMakeVisible (l);
    };
    setupBandName (nameLow, "LOW");
    setupBandName (nameMid, "MID");
    setupBandName (nameHigh, "HIGH");

    addAndMakeVisible (meterLow);
    addAndMakeVisible (meterMid);
    addAndMakeVisible (meterHigh);

    configureKnob (trimLow,  trimLowLbl,  "Amount", " %");
    configureKnob (trimMid,  trimMidLbl,  "Amount", " %");
    configureKnob (trimHigh, trimHighLbl, "Amount", " %");

    configureKnob (compress, compressLbl, "Compress", " %");
    configureKnob (output,   outputLbl,   "Output",   " dB");
    configureKnob (mix,      mixLbl,      "Mix",      " %");
    configureKnob (lowFreq,  lowFreqLbl,  "Low/Mid",  " Hz");
    configureKnob (highFreq, highFreqLbl, "Mid/High", " Hz");

    atts.push_back (attach ("low",      trimLow));
    atts.push_back (attach ("mid",      trimMid));
    atts.push_back (attach ("high",     trimHigh));
    atts.push_back (attach ("compress", compress));
    atts.push_back (attach ("output",   output));
    atts.push_back (attach ("mix",      mix));
    atts.push_back (attach ("lowfreq",  lowFreq));
    atts.push_back (attach ("highfreq", highFreq));
    bypassAtt = std::make_unique<ButtonAttachment> (processor.apvts, "bypass", bypassButton);

    setSize (640, 460);
}

VocalMbCompAudioProcessorEditor::~VocalMbCompAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void VocalMbCompAudioProcessorEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                                     const juce::String& name, const juce::String& suffix)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
    slider.setTextValueSuffix (suffix);
    addAndMakeVisible (slider);

    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (label);
}

std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
VocalMbCompAudioProcessorEditor::attach (const juce::String& id, juce::Slider& s)
{
    return std::make_unique<SliderAttachment> (processor.apvts, id, s);
}

void VocalMbCompAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (FactoryLookAndFeel::background());
}

void VocalMbCompAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top);

    r.removeFromTop (10);

    // Band section: three columns (name, meter, amount knob).
    auto bandArea = r.removeFromTop (250);
    const int colW = bandArea.getWidth() / 3;

    struct Col { juce::Label* name; BandMeter* meter; juce::Slider* trim; juce::Label* trimLbl; };
    Col cols[3] = {
        { &nameLow,  &meterLow,  &trimLow,  &trimLowLbl },
        { &nameMid,  &meterMid,  &trimMid,  &trimMidLbl },
        { &nameHigh, &meterHigh, &trimHigh, &trimHighLbl },
    };
    for (auto& c : cols)
    {
        auto cell = bandArea.removeFromLeft (colW).reduced (10, 0);
        c.name->setBounds (cell.removeFromTop (20));
        cell.removeFromTop (4);
        auto knob = cell.removeFromBottom (96);
        c.trimLbl->setBounds (knob.removeFromTop (16));
        c.trim->setBounds (knob);
        cell.removeFromBottom (4);
        c.meter->setBounds (cell.reduced (cell.getWidth() / 2 - 22, 0));
    }

    r.removeFromTop (12);

    // Global controls row.
    const int gcols = 5;
    const int gw = r.getWidth() / gcols;
    auto place = [] (juce::Rectangle<int> cell, juce::Slider& s, juce::Label& l) {
        l.setBounds (cell.removeFromTop (16));
        s.setBounds (cell);
    };
    place (r.removeFromLeft (gw).reduced (4), compress, compressLbl);
    place (r.removeFromLeft (gw).reduced (4), output,   outputLbl);
    place (r.removeFromLeft (gw).reduced (4), mix,      mixLbl);
    place (r.removeFromLeft (gw).reduced (4), lowFreq,  lowFreqLbl);
    place (r.reduced (4),                     highFreq, highFreqLbl);
}
