#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/FactoryChrome.h"

#include <memory>

//
// One band strip of the enhancer: a coloured header (band name), a compact per-band
// Mode selector, a Solo toggle, an Enhance knob (0..150 %), a Width knob and a live
// residual-RMS meter that shows how much harmonic energy this band is currently
// adding. GUI-thread only.
//
class BandStripComponent : public juce::Component,
                           private juce::Timer
{
public:
    BandStripComponent (MultibandEnhancerAudioProcessor& p, int bandIndex)
        : processor (p), band (bandIndex)
    {
        factory_ui::styleKnob (enhance, enhLabel, "Enhance", " %");
        addAndMakeVisible (enhance);
        addAndMakeVisible (enhLabel);

        factory_ui::styleKnob (width, widthLabel, "Width", " %");
        addAndMakeVisible (width);
        addAndMakeVisible (widthLabel);

        modeBox.addItemList ({ "Tube", "Tape", "Bright", "Clean", "Glue" }, 1);
        addAndMakeVisible (modeBox);

        soloButton.setColour (juce::ToggleButton::tickColourId, FactoryLookAndFeel::bandColour (band));
        addAndMakeVisible (soloButton);

        enhAtt  = std::make_unique<SliderAtt> (
            processor.apvts, MultibandEnhancerAudioProcessor::enhId (band), enhance);
        widthAtt = std::make_unique<SliderAtt> (
            processor.apvts, MultibandEnhancerAudioProcessor::widthId (band), width);
        modeAtt = std::make_unique<ComboAtt> (
            processor.apvts, MultibandEnhancerAudioProcessor::modeId (band), modeBox);
        soloAtt = std::make_unique<ButtonAtt> (
            processor.apvts, MultibandEnhancerAudioProcessor::soloId (band), soloButton);
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

        // Residual meter (thin vertical bar down the right of the knob column).
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
        r.removeFromTop (22);                          // header (name painted)
        modeBox.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        soloButton.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        // Thin residual meter down the right edge of the two knobs.
        meterArea = r.removeFromRight (8).toFloat().reduced (0.0f, 4.0f);
        r.removeFromRight (4);

        auto enhArea = r.removeFromTop (r.getHeight() / 2);
        enhLabel.setBounds (enhArea.removeFromBottom (14));
        enhance.setBounds (enhArea);
        widthLabel.setBounds (r.removeFromBottom (14));
        width.setBounds (r);
    }

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override { repaint(); }

    MultibandEnhancerAudioProcessor& processor;
    int band;

    juce::Slider   enhance, width;
    juce::Label    enhLabel, widthLabel;
    juce::ComboBox modeBox;
    juce::ToggleButton soloButton { "Solo" };
    juce::Rectangle<float> meterArea;

    std::unique_ptr<SliderAtt> enhAtt, widthAtt;
    std::unique_ptr<ComboAtt>  modeAtt;
    std::unique_ptr<ButtonAtt> soloAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandStripComponent)
};
