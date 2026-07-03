#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "AnalyzerComponent.h"
#include "BandStripComponent.h"
#include "factory_ui/FactoryLookAndFeel.h"

#include <array>
#include <memory>

//
// Editor: the analyser card (three spectra + draggable crossovers) over a row of
// five band strips, with a right-hand control card (Mode, Direct / Enhanced
// faders, Output, Quality, Delta Listen, Bypass). Uses factory_ui throughout.
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

    void styleFader (juce::Slider&, juce::Label&, const juce::String&);

    MultibandEnhancerAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    AnalyzerComponent analyzer;
    std::array<std::unique_ptr<BandStripComponent>, 5> strips;

    juce::Rectangle<int> controlCard, bandCard;

    juce::ComboBox modeBox, qualityBox;
    juce::Label    modeLabel, qualityLabel;
    juce::Slider   directFader, enhancedFader, outputKnob;
    juce::Label    directLabel, enhancedLabel, outputLabel;
    juce::ToggleButton deltaButton { "Delta Listen" }, bypassButton { "Bypass" };

    std::unique_ptr<ComboAtt>  modeAtt, qualityAtt;
    std::unique_ptr<SliderAtt> directAtt, enhancedAtt, outputAtt;
    std::unique_ptr<ButtonAtt> deltaAtt, bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEnhancerAudioProcessorEditor)
};
