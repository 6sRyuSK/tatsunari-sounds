#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"
#include "BandMeter.h"

#include <memory>
#include <vector>

class VocalMbCompAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit VocalMbCompAudioProcessorEditor (VocalMbCompAudioProcessor&);
    ~VocalMbCompAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void configureKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix);
    std::unique_ptr<SliderAttachment> attach (const juce::String& id, juce::Slider&);

    VocalMbCompAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };

    BandMeter meterLow, meterMid, meterHigh;
    juce::Label nameLow, nameMid, nameHigh;
    juce::Slider trimLow, trimMid, trimHigh;
    juce::Label trimLowLbl, trimMidLbl, trimHighLbl;

    juce::Slider compress, output, mix, lowFreq, highFreq;
    juce::Label compressLbl, outputLbl, mixLbl, lowFreqLbl, highFreqLbl;

    std::vector<std::unique_ptr<SliderAttachment>> atts;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMbCompAudioProcessorEditor)
};
