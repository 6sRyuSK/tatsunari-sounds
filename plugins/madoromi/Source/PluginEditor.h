#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

class MadoromiAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MadoromiAudioProcessorEditor (MadoromiAudioProcessor&);
    ~MadoromiAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void configureKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix);

    MadoromiAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    // Row 1: Clock / Wash / Tone / Length. Row 2: Balance / Mix / Output / Loop.
    juce::Slider clockSlider, washSlider, toneSlider, lengthSlider, balanceSlider, mixSlider, outputSlider;
    juce::Label  clockLabel, washLabel, toneLabel, lengthLabel, balanceLabel, mixLabel, outputLabel, titleLabel;

    // Loop is a big latching footswitch-style TextButton (MOOD-style), not a
    // small checkbox: setClickingTogglesState(true) + a ButtonAttachment to the
    // "loop" bool param. The default LookAndFeel TextButton rendering (via
    // buttonOnColourId set below) shows the latched state in accent.
    juce::TextButton loopButton { "Loop" };
    juce::ToggleButton bypassButton { "Bypass" };

    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    std::unique_ptr<SliderAttachment> clockAtt, washAtt, toneAtt, lengthAtt, balanceAtt, mixAtt, outputAtt;
    std::unique_ptr<ButtonAttachment> loopAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MadoromiAudioProcessorEditor)
};
