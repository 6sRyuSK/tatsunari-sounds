#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

class MochiStretchAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MochiStretchAudioProcessorEditor (MochiStretchAudioProcessor&);
    ~MochiStretchAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    MochiStretchAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };

    // Row 1: Speed / Pitch / Window knobs.
    juce::Slider speedSlider, pitchSlider, windowSlider;
    juce::Label  speedLabel, pitchLabel, windowLabel;

    // Row 2: Hold (latching button, not a knob) / Mix / Output.
    juce::TextButton holdButton { "HOLD" };
    juce::Label holdLabel;
    juce::Slider mixSlider, outputSlider;
    juce::Label  mixLabel, outputLabel;

    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    std::unique_ptr<SliderAttachment> speedAtt, pitchAtt, windowAtt, mixAtt, outputAtt;
    std::unique_ptr<ButtonAttachment> holdAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MochiStretchAudioProcessorEditor)
};
