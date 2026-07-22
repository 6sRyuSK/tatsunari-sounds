#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

class OmoideEchoAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit OmoideEchoAudioProcessorEditor (OmoideEchoAudioProcessor&);
    ~OmoideEchoAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    OmoideEchoAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    // Row 1: Delay / Regen / Tone / Scan. Row 2: Memory (=scanlevel) / Mix / Output.
    juce::Slider delaySlider, regenSlider, toneSlider, scanSlider;
    juce::Slider memorySlider, mixSlider, outputSlider;
    juce::Label  delayLabel, regenLabel, toneLabel, scanLabel;
    juce::Label  memoryLabel, mixLabel, outputLabel, titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    std::unique_ptr<SliderAttachment> delayAtt, regenAtt, toneAtt, scanAtt;
    std::unique_ptr<SliderAttachment> memoryAtt, mixAtt, outputAtt;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OmoideEchoAudioProcessorEditor)
};
