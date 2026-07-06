#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

class TumbleDelayAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TumbleDelayAudioProcessorEditor (TumbleDelayAudioProcessor&);
    ~TumbleDelayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    TumbleDelayAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider outputSlider;
    juce::Label  outputLabel, titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    std::unique_ptr<SliderAttachment> outputAtt;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TumbleDelayAudioProcessorEditor)
};
