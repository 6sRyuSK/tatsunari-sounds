#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"
#include "GrainCloudComponent.h"

#include <memory>
#include <vector>

class GranularDelayAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit GranularDelayAudioProcessorEditor (GranularDelayAudioProcessor&);
    ~GranularDelayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void addKnob (const char* id, const char* name, const char* suffix);

    GranularDelayAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    juce::ToggleButton syncButton { "Sync" };
    juce::ComboBox divisionBox;
    GrainCloudComponent cloud;

    std::vector<std::unique_ptr<juce::Slider>> knobs;
    std::vector<std::unique_ptr<juce::Label>>  knobLabels;
    std::vector<std::unique_ptr<SliderAttachment>> knobAtts;

    std::unique_ptr<ButtonAttachment>   bypassAtt, syncAtt;
    std::unique_ptr<ComboBoxAttachment> divisionAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GranularDelayAudioProcessorEditor)
};
