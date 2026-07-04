#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"

#include <array>
#include <memory>
#include <vector>

class NamPlayerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer,
                                            private juce::AudioProcessorListener
{
public:
    explicit NamPlayerAudioProcessorEditor (NamPlayerAudioProcessor&);
    ~NamPlayerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    static constexpr int kNumSlots = NamPlayerAudioProcessor::kNumSlots;

    void addKnob (juce::Slider&, juce::Label&, const juce::String& name,
                  const juce::String& suffix, const juce::String& paramId, int decimals);
    void openModelChooser (int slot);
    void openIrChooser();
    void refreshNames();
    void refreshPresetSelector();
    void timerCallback() override;

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    // MERGE: pick a 48 kHz input WAV, then an output path, then render the reamp pair
    // on a background thread (never the message/audio thread).
    void startReamp();
    void chooseReampOutput (const juce::File& inputFile);
    void runReampJob (const juce::File& in, const juce::File& out);

    NamPlayerAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label title;
    factory_ui::PresetSelector presetSelector;

    std::array<juce::Label, kNumSlots>        slotHeader, slotNameLabel;
    std::array<juce::TextButton, kNumSlots>   slotLoad, slotClear;
    std::array<juce::ToggleButton, kNumSlots> slotEnable;
    std::array<juce::ComboBox, kNumSlots>     slotMode;
    std::array<juce::Slider, kNumSlots>       slotIn, slotOut, slotBal;
    std::array<juce::Label, kNumSlots>        slotInL, slotOutL, slotBalL;

    juce::Label      irHeader, irNameLabel;
    juce::TextButton irLoad { "Load IR" }, irClear { "X" };

    juce::Slider inTrim, irLevel, outGain, mix, loCut, hiCut;
    juce::Label  inTrimL, irLevelL, outGainL, mixL, loCutL, hiCutL;
    juce::ToggleButton irEnable { "IR" }, bypass { "Bypass" };

    juce::Label        mergeHeader;
    juce::TextButton   reampButton  { "Reamp Export..." };
    juce::ToggleButton reampIrTone  { "Include Cab IR + Tone" };

    std::unique_ptr<juce::FileChooser> chooser;

    std::vector<std::unique_ptr<SA>> sliderAtts;
    std::vector<std::unique_ptr<BA>> buttonAtts;
    std::vector<std::unique_ptr<CA>> comboAtts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamPlayerAudioProcessorEditor)
};
