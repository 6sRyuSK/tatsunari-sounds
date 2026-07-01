#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
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
    SuppressionCurveComponent curve;

    juce::Slider depthS, sharpS, atkS, relS, mixS;
    juce::Label  depthL, sharpL, atkL, relL, mixL;
    juce::ToggleButton deltaB { "Delta" }, linkB { "Link" }, bypassB { "Bypass" };
    juce::ComboBox modeBox;

    std::vector<std::unique_ptr<SA>> knobAtts;
    std::unique_ptr<BA> deltaAtt, linkAtt, bypassAtt;
    std::unique_ptr<CA> modeAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessorEditor)
};
