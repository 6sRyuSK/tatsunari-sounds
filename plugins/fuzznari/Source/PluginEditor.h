#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FuzzCurveComponent.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"

class FuzznariAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::AudioProcessorListener
{
public:
    explicit FuzznariAudioProcessorEditor (FuzznariAudioProcessor&);
    ~FuzznariAudioProcessorEditor() override;

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

    FuzznariAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider driveSlider, biasSlider, gateSlider, stabSlider, toneSlider, levelSlider, mixSlider;
    juce::Label  driveLabel, biasLabel, gateLabel, stabLabel, toneLabel, levelLabel, mixLabel, titleLabel;
    juce::ToggleButton squealButton { "Squeal" };
    juce::ToggleButton bypassButton { "Bypass" };
    factory_ui::PresetSelector presetSelector;
    FuzzCurveComponent curve;

    std::unique_ptr<SliderAttachment> driveAtt, biasAtt, gateAtt, stabAtt, toneAtt, levelAtt, mixAtt;
    std::unique_ptr<ButtonAttachment> squealAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FuzznariAudioProcessorEditor)
};
