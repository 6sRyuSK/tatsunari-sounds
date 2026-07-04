#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"
#include "SuppressionCurveComponent.h"

#include <memory>
#include <vector>

class ResonanceSuppressorAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                      private juce::AudioProcessorListener
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
    void refreshPresetSelector();

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    ResonanceSuppressorAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    factory_ui::PresetSelector presetSelector;
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
