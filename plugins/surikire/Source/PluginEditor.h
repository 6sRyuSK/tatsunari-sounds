#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

class SurikireAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SurikireAudioProcessorEditor (SurikireAudioProcessor&);
    ~SurikireAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    // Ten knobs in 2 rows x 5 columns, row-major:
    //   row 1: Wow / Flutter / Gen / Saturate / Noise
    //   row 2: Failure / HP / LP / Mix / Output
    static constexpr int kNumKnobs = 10;

    SurikireAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider sliders[kNumKnobs];
    juce::Label  labels[kNumKnobs];
    juce::Label  titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    std::unique_ptr<SliderAttachment> sliderAtts[kNumKnobs];
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurikireAudioProcessorEditor)
};
