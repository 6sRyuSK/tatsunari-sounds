#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"
#include "ReverbVisualizer.h"

#include <memory>
#include <vector>

class ShimmerReverbAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ShimmerReverbAudioProcessorEditor (ShimmerReverbAudioProcessor&);
    ~ShimmerReverbAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void addKnob (const char* id, const char* name, const char* suffix);
    void setupPitchBox (juce::ComboBox&, juce::Label&, const char* name);

    ShimmerReverbAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton freezeButton { "Freeze" };
    juce::ToggleButton bypassButton { "Bypass" };
    juce::ComboBox pitchABox, pitchBBox;
    juce::Label pitchALabel, pitchBLabel;
    ReverbVisualizer visualizer;

    std::vector<std::unique_ptr<juce::Slider>> knobs;
    std::vector<std::unique_ptr<juce::Label>>  knobLabels;
    std::vector<std::unique_ptr<SliderAttachment>> knobAtts;

    std::unique_ptr<ButtonAttachment>   freezeAtt, bypassAtt;
    std::unique_ptr<ComboBoxAttachment> pitchAAtt, pitchBAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbAudioProcessorEditor)
};
