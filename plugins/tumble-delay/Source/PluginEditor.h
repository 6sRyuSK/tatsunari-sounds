#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "TumbleVisualizer.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

#include <array>
#include <memory>

//
// Tumble Delay editor. factory_ui "kawaii" chrome: a physics visualizer on the
// left (~55%), a right column of World / Detect / Out groups, and a bottom bank
// with slot tabs A-D whose knob bank re-attaches onto the selected slot's params.
//
class TumbleDelayAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TumbleDelayAudioProcessorEditor (TumbleDelayAudioProcessor&);
    ~TumbleDelayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    // A rotary knob = slider + caption label + its (rebindable) APVTS attachment.
    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttachment> att;
    };
    // A choice = combo + caption label + its (rebindable) APVTS attachment.
    struct Choice
    {
        juce::ComboBox box;
        juce::Label    label;
        std::unique_ptr<ComboBoxAttachment> att;
    };

    // ---- build / attach helpers ----
    void styleAndAdd (Knob& k, const juce::String& name, const juce::String& suffix);
    void styleAndAdd (Choice& c, const juce::String& name);
    void attachKnob  (Knob& k, const juce::String& paramID, int decimals);
    void attachChoice (Choice& c, const juce::String& paramID);

    void selectSlot (int slot);
    void attachBank (int slot); // (re)binds the bottom bank onto slot a/b/c/d

    // ---- layout helpers ----
    static juce::Rectangle<int> col (juce::Rectangle<int> row, int n, int i);
    static void placeKnob   (Knob& k, juce::Rectangle<int> cell);
    static void placeChoice (Choice& c, juce::Rectangle<int> cell);
    void paintGroup (juce::Graphics& g, juce::Rectangle<int> card, const juce::String& title,
                     juce::Colour titleColour = FactoryLookAndFeel::textDim()) const;

    void layoutWorld   (juce::Rectangle<int> area);
    void layoutDetect  (juce::Rectangle<int> area);
    void layoutOut     (juce::Rectangle<int> area);
    void layoutBank    (juce::Rectangle<int> area);
    void layoutPhysics (juce::Rectangle<int> card);
    void layoutSource  (juce::Rectangle<int> card);
    void layoutSound   (juce::Rectangle<int> card);

    TumbleDelayAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    TumbleVisualizer visualizer;

    // ---- right column: World ----
    Choice shape;
    Knob   boxSize;  Choice sizeSync;
    Knob   spin;     Choice spinSync;
    Knob   gravity;
    juce::ToggleButton ballCollideButton { "Ball Collide" };
    std::unique_ptr<ButtonAttachment> ballCollideAtt;
    // ---- right column: Detect ----
    Knob sense, retrig, spread;
    // ---- right column: Out ----
    Knob refeed, tone, mix, output;

    // ---- bottom bank: slot tabs ----
    std::array<juce::TextButton, 4>   tabButtons;
    std::array<juce::ToggleButton, 4> tabLeds;
    std::array<std::unique_ptr<ButtonAttachment>, 4> tabLedAtts;
    int selectedSlot = 0;

    // ---- bottom bank: re-attached controls for the selected slot ----
    // Physics
    Knob   bkCount, bkBallSize, bkSpeed, bkDirection, bkDirRandom, bkPreDelay;
    Choice bkPreDelaySync;
    Knob   bkTime;
    Choice bkTimeSync;
    Knob   bkBounce, bkDrag, bkDecayCurve;
    Choice bkLifeMode;
    Knob   bkLifeTime, bkLifeBounces;
    // Source
    Knob   bkMotion, bkStep, bkSpray;
    // Sound
    Knob   bkPitch, bkPitchRand, bkGrain, bkReverse;
    Choice bkPanMode;
    Knob   bkGain;

    // Card rectangles (computed in resized(), painted in paint()).
    juce::Rectangle<int> worldCard, detectCard, outCard;
    juce::Rectangle<int> physicsCard, sourceCard, soundCard;

    static constexpr const char* kSlotPrefix[4] = { "a", "b", "c", "d" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TumbleDelayAudioProcessorEditor)
};
