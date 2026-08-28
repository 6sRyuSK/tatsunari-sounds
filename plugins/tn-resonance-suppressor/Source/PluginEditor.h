#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "RsLookAndFeel.h"
#include "RsWidgets.h"
#include "factory_ui/PresetSelectorController.h"
#include "SuppressionCurveComponent.h"

#include <memory>
#include <vector>

// Phase P1 new-UI chrome: a plugin-local RsLookAndFeel + RsWidgets rebuild the
// editor's header and footer to the approved demo mockup, keeping 100% of the
// shipped v2.0.1 wiring (every APVTS param, A/B, Undo/Redo, preset sync, the
// 500 ms undo-transaction / button-enable timer, and the destructor's
// setListenNode(-1)). The analyser (SuppressionCurveComponent) is only placed
// in the new layout -- its internals are untouched (Phase 2).
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

    // Wire one RsKnob: style + SliderAttachment (existing id) + post-attachment
    // decimal formatting (#23: setSliderDecimals must run AFTER the attachment).
    void addKnob (rs::RsKnob&, const juce::String& name, juce::Colour accent, bool big,
                  const juce::String& suffix, const juce::String& id, int decimals);

    // Idle-transaction boundary (Phase 5b-2, preserved): close off the APVTS
    // flush timer's accumulating transaction each tick so a gesture becomes its
    // own undo step, and refresh the Undo/Redo enabled state (covers host-driven
    // changes too).
    void timerCallback() override;
    void refreshUndoRedoButtons();
    // Reflect the active A/B slot on the A|B segment + the Copy button tooltip.
    void updateABUI();

    // Lay out one 2-column settings row in the footer's third column and record
    // the two pill-cell chrome rects (card + icon + caption) for paint().
    void layoutPillRow (juce::Rectangle<int> row,
                        rs::RsPillToggle& left,  rs::icons::Glyph lg, const juce::String& lcap,
                        rs::RsPillToggle& right, rs::icons::Glyph rg, const juce::String& rcap);

    ResonanceSuppressorAudioProcessor& processor;
    RsLookAndFeel rsLnf;

    // --- Header ---
    rs::RsBrand brand;
    factory_ui::PresetSelectorController presetController; // reused as-is (host/user sync, Save/Overwrite/Delete)
    rs::RsSegmented   abSeg;                                // A|B compare (manual: setABSlot/getABSlot)
    rs::RsIconButton  copyBtn, undoBtn, redoBtn;
    rs::RsPillToggle  bypassToggle;

    // --- Analyzer (placed only; internals untouched) ---
    SuppressionCurveComponent curve;

    // --- Footer: knobs (left->right, existing ids) ---
    rs::RsKnob depthK, detailK;              // big, coral arc ("detail" replaces sharpness/selectivity, v2.1)
    rs::RsKnob atkK, relK, tiltK;            // small (amber / amber / mint)

    // --- Footer: MODE + settings ---
    rs::RsSegmented    modeSeg;                                  // Soft / Hard (mode)
    rs::RsPillToggle   deltaToggle, scEnableToggle, scListenToggle, linkToggle;
    rs::RsValueSetting qualitySet, chSet;                        // quality / channelMode
    rs::RsLinkSlider   linkAmtSlider;                            // linkAmt
    rs::RsLinkSlider   mixSlider, outSlider;                     // mix / out (5th footer row, v2.1)

    // Attachments (all existing param IDs).
    std::vector<std::unique_ptr<SA>> knobAtts;
    std::unique_ptr<CA> modeAtt, qualityAtt, channelAtt;
    std::unique_ptr<BA> deltaAtt, scEnableAtt, scListenAtt, linkAtt, bypassAtt;
    std::unique_ptr<SA> linkAmtAtt, mixAtt, outAtt;

    // Chrome rects computed in resized(), painted in paint().
    juce::Rectangle<int> footerCardBounds, modeCellBounds, bypassLabelBounds;
    int footerDivX1 = 0, footerDivX2 = 0;
    struct SettingCell { juce::Rectangle<int> bounds; rs::icons::Glyph glyph; juce::String caption; };
    std::vector<SettingCell> pillCells;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorAudioProcessorEditor)
};
