#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "GainReductionMeter.h"

class BusCompressorAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit BusCompressorAudioProcessorEditor (BusCompressorAudioProcessor&);
    ~BusCompressorAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void configureKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix);

    BusCompressorAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider thresholdSlider, attackSlider, releaseSlider, makeupSlider, mixSlider;
    juce::Label  thresholdLabel, attackLabel, releaseLabel, makeupLabel, mixLabel, ratioLabel, titleLabel;
    juce::ComboBox ratioBox;
    juce::ToggleButton bypassButton { "Bypass" };
    GainReductionMeter meter;

    std::unique_ptr<SliderAttachment>   thrAtt, atkAtt, relAtt, makeAtt, mixAtt;
    std::unique_ptr<ComboBoxAttachment> ratioAtt;
    std::unique_ptr<ButtonAttachment>   bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusCompressorAudioProcessorEditor)
};
