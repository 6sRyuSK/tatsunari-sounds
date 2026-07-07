#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"
#include "SuppressionCurveComponent.h"

#include <memory>
#include <vector>

class ResonanceSuppressorAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ResonanceSuppressorAudioProcessorEditor (ResonanceSuppressorAudioProcessor&);
    ~ResonanceSuppressorAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void addKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix, const juce::String& id);

    ResonanceSuppressorAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    factory_ui::PresetSelectorController presetController;
    SuppressionCurveComponent curve;

    // Knob row (left -> right): Depth, Sharpness, Selectivity, Attack, Release, Tilt, Mix.
    juce::Slider depthS, sharpS, selS, atkS, relS, tiltS, mixS;
    juce::Label  depthL, sharpL, selL, atkL, relL, tiltL, mixL;
    juce::ToggleButton deltaB { "Delta" }, linkB { "Link" }, bypassB { "Bypass" };
    juce::ComboBox modeBox, qualityBox;

    std::vector<std::unique_ptr<SA>> knobAtts;
    std::unique_ptr<BA> deltaAtt, linkAtt, bypassAtt;
    std::unique_ptr<CA> modeAtt, qualityAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessorEditor)
};
