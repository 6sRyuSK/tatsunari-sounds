#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"
#include "TransferCurveComponent.h"

class SaturatorAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::AudioProcessorListener
{
public:
    explicit SaturatorAudioProcessorEditor (SaturatorAudioProcessor&);
    ~SaturatorAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void configureKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix);
    void refreshPresetSelector();

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    SaturatorAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider driveSlider, mixSlider, outputSlider;
    juce::Label  driveLabel, mixLabel, outputLabel, titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    factory_ui::PresetSelector presetSelector;
    TransferCurveComponent curve;

    std::unique_ptr<SliderAttachment> driveAtt, mixAtt, outputAtt;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SaturatorAudioProcessorEditor)
};
