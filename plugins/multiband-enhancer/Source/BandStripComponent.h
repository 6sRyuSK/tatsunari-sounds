#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

//
// One band strip of the enhancer: a coloured header (band name), a vertical
// Enhance slider, a small Width knob and a live residual-RMS meter that shows how
// much harmonic energy this band is currently adding. GUI-thread only.
//
class BandStripComponent : public juce::Component,
                           private juce::Timer
{
public:
    BandStripComponent (MultibandEnhancerAudioProcessor& p, int bandIndex)
        : processor (p), band (bandIndex)
    {
        enhance.setSliderStyle (juce::Slider::LinearVertical);
        enhance.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
        enhance.setColour (juce::Slider::textBoxTextColourId, FactoryLookAndFeel::text());
        enhance.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        enhance.setTextValueSuffix (" %");
        addAndMakeVisible (enhance);

        factory_ui::styleKnob (width, widthLabel, "Width", " %");
        addAndMakeVisible (width);
        addAndMakeVisible (widthLabel);

        enhAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, MultibandEnhancerAudioProcessor::enhId (band), enhance);
        widthAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, MultibandEnhancerAudioProcessor::widthId (band), width);
        factory_ui::setSliderDecimals (enhance, 0);
        factory_ui::setSliderDecimals (width, 0);

        startTimerHz (30);
    }

    ~BandStripComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        factory_ui::dropShadowFor (g, r.toNearestInt(), 9.0f);
        factory_ui::paintCard (g, r, 9.0f);

        // Header: coloured dot + band name.
        auto header = r.reduced (8.0f).removeFromTop (18.0f);
        const auto col = FactoryLookAndFeel::bandColour (band);
        g.setColour (col);
        g.fillEllipse (header.removeFromLeft (12.0f).withSizeKeepingCentre (9.0f, 9.0f));
        header.removeFromLeft (5.0f);
        g.setColour (FactoryLookAndFeel::text());
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (MultibandEnhancerAudioProcessor::kBandNames[band], header, juce::Justification::centredLeft);

        // Residual meter (vertical bar to the right of the Enhance slider).
        const float db = processor.bandResidualRmsDb (band);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f); // -90..0 dBFS
        auto m = meterArea;
        g.setColour (FactoryLookAndFeel::track());
        g.fillRoundedRectangle (m, 2.0f);
        auto fillR = m.withTop (m.getBottom() - norm * m.getHeight());
        g.setColour (col.withAlpha (0.85f));
        g.fillRoundedRectangle (fillR, 2.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        r.removeFromTop (24); // header

        auto bottom = r.removeFromBottom (72);
        width.setBounds (bottom.removeFromTop (54));
        widthLabel.setBounds (bottom);

        auto body = r.reduced (2);
        meterArea = body.removeFromRight (8).toFloat().reduced (0.0f, 4.0f);
        body.removeFromRight (4);
        enhance.setBounds (body);
    }

private:
    void timerCallback() override { repaint(); }

    MultibandEnhancerAudioProcessor& processor;
    int band;

    juce::Slider enhance, width;
    juce::Label  widthLabel;
    juce::Rectangle<float> meterArea;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> enhAtt, widthAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandStripComponent)
};
