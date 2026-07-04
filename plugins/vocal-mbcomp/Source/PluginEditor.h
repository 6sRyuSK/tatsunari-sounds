#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"
#include "BandMeter.h"

#include <memory>
#include <vector>

class VocalMbCompAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                              private juce::AudioProcessorListener
{
public:
    explicit VocalMbCompAudioProcessorEditor (VocalMbCompAudioProcessor&);
    ~VocalMbCompAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void configureKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& suffix);
    std::unique_ptr<SliderAttachment> attach (const juce::String& id, juce::Slider&);
    void refreshPresetSelector();

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    VocalMbCompAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    factory_ui::PresetSelector presetSelector;

    BandMeter meterLow, meterMid, meterHigh;
    juce::Label nameLow, nameMid, nameHigh;
    juce::Slider trimLow, trimMid, trimHigh;
    juce::Label trimLowLbl, trimMidLbl, trimHighLbl;

    juce::Slider compress, output, mix, lowFreq, highFreq;
    juce::Label compressLbl, outputLbl, mixLbl, lowFreqLbl, highFreqLbl;

    std::vector<std::unique_ptr<SliderAttachment>> atts;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMbCompAudioProcessorEditor)
};
