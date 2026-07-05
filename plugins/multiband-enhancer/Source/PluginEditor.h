#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "AnalyzerComponent.h"
#include "BandStripComponent.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

#include <array>
#include <memory>

//
// Editor: the analyser card (three spectra + draggable crossovers) over a row of
// five band strips (each with its own Enhance / Width knobs, Mode selector and
// Solo toggle), with a right-hand control card (Mix, Output, Quality, Delta
// Listen, Bypass). Uses factory_ui throughout.
//
class MultibandEnhancerAudioProcessorEditor : public juce::AudioProcessorEditor
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

    // Linear-phase crossover is HQ-only: grey the phase selector out in Zero-Latency.
    void updatePhaseEnablement();

    MultibandEnhancerAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    factory_ui::PresetSelectorController presetController;
    AnalyzerComponent analyzer;
    std::array<std::unique_ptr<BandStripComponent>, 5> strips;

    juce::Rectangle<int> controlCard, bandCard;

    juce::ComboBox qualityBox, phaseBox;
    juce::Label    qualityLabel, phaseLabel;
    juce::Slider   mixKnob, outputKnob;
    juce::Label    mixLabel, outputLabel;
    juce::ToggleButton deltaButton { "Delta Listen" }, bypassButton { "Bypass" };

    std::unique_ptr<ComboAtt>  qualityAtt, phaseAtt;
    std::unique_ptr<SliderAtt> mixAtt, outputAtt;
    std::unique_ptr<ButtonAtt> deltaAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEnhancerAudioProcessorEditor)
};
