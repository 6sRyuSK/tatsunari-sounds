#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "AnalyzerComponent.h"
#include "BandStripComponent.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"

#include <array>
#include <memory>

//
// Editor: the analyser card (three spectra + draggable crossovers) over a row of
// five band strips (each with its own Enhance / Width knobs, Mode selector and
// Solo toggle), with a right-hand control card (Mix, Output, Quality, Delta
// Listen, Bypass). Uses factory_ui throughout.
//
class MultibandEnhancerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                              private juce::AudioProcessorListener
{
public:
    explicit MultibandEnhancerAudioProcessorEditor (MultibandEnhancerAudioProcessor&);
    ~MultibandEnhancerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void refreshPresetSelector();

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    MultibandEnhancerAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    factory_ui::PresetSelector presetSelector;
    AnalyzerComponent analyzer;
    std::array<std::unique_ptr<BandStripComponent>, 5> strips;

    juce::Rectangle<int> controlCard, bandCard;

    juce::ComboBox qualityBox;
    juce::Label    qualityLabel;
    juce::Slider   mixKnob, outputKnob;
    juce::Label    mixLabel, outputLabel;
    juce::ToggleButton deltaButton { "Delta Listen" }, bypassButton { "Bypass" };

    std::unique_ptr<ComboAtt>  qualityAtt;
    std::unique_ptr<SliderAtt> mixAtt, outputAtt;
    std::unique_ptr<ButtonAtt> deltaAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEnhancerAudioProcessorEditor)
};
