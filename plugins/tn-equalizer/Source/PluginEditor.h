#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"
#include "EqCurveComponent.h"
#include "BandControlPanel.h"

class DynamicEqAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit DynamicEqAudioProcessorEditor (DynamicEqAudioProcessor&);
    ~DynamicEqAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:

    DynamicEqAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    factory_ui::PresetSelectorController presetController;
    EqCurveComponent curve;
    BandControlPanel panel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicEqAudioProcessorEditor)
};
