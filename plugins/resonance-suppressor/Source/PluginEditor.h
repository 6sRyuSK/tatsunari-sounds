#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"
#include "SuppressionCurveComponent.h"

#include <memory>
#include <vector>

// Phase 5b: private juce::Timer drives two periodic, message-thread-only
// chores -- segmenting continuous edits into discrete undo transactions (see
// timerCallback()) and refreshing the Undo/Redo buttons' enabled state.
class ResonanceSuppressorAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                       private juce::Timer
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
    // Idle-transaction boundary (Phase 5b-2): every tick, close off whatever
    // transaction the APVTS's parameter-flush timer has been accumulating into,
    // so a knob drag / curve drag becomes its own undo step instead of merging
    // with the next gesture. Also refreshes the Undo/Redo buttons' enabled state
    // (covers host-driven parameter changes too, not just this editor's own).
    void timerCallback() override;
    void refreshUndoRedoButtons();
    // Reflect the active A/B slot: highlight A or B, retarget the Copy button's
    // label/tooltip at the OTHER (inactive) slot.
    void updateABUI();

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

    // Second header row (Pass 3B routing + Phase 5b re-layout): mode/quality
    // moved down from the top row (freed for A/B + Undo/Redo, see resized()),
    // channel mode, sidechain toggles, Delta/Link, Link Amount.
    juce::ComboBox channelBox;
    juce::ToggleButton scEnableB { "Sidechain" }, scListenB { "SC Listen" };
    juce::Slider linkAmtS;
    juce::Label  linkAmtL;

    // Phase 5b-1: A/B compare. abAButton/abBButton are a two-way radio pair
    // (mutually exclusive highlight, see the constructor); abCopyButton copies
    // the active slot onto the inactive one, its label/tooltip pointed at the
    // inactive slot (updateABUI()).
    juce::TextButton abAButton { "A" }, abBButton { "B" }, abCopyButton { "Copy" };
    // Phase 5b-2: Undo/Redo, enabled state tracks UndoManager::canUndo/canRedo.
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };

    std::vector<std::unique_ptr<SA>> knobAtts;
    std::unique_ptr<BA> deltaAtt, linkAtt, bypassAtt;
    std::unique_ptr<CA> modeAtt, qualityAtt;
    std::unique_ptr<CA> channelAtt;
    std::unique_ptr<BA> scEnableAtt, scListenAtt;
    std::unique_ptr<SA> linkAmtAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessorEditor)
};
