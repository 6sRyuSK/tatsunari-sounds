#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelectorController.h"

//
// Three-dot sequencer position indicator (base / int1 / int2). Reads the
// audio thread's current step through an atomic; repaints only on change.
//
class StepDotsComponent final : public juce::Component, private juce::Timer
{
public:
    explicit StepDotsComponent (std::atomic<int>& source) : src (source)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        const float d = (float) juce::jmin (getHeight(), getWidth() / 3) - 4.0f;
        const float y = ((float) getHeight() - d) * 0.5f;
        for (int i = 0; i < 3; ++i)
        {
            const float x = (float) getWidth() * ((float) i + 0.5f) / 3.0f - d * 0.5f;
            g.setColour (i == shown ? FactoryLookAndFeel::accent()
                                    : FactoryLookAndFeel::track());
            g.fillEllipse (x, y, d, d);
        }
    }

private:
    void timerCallback() override
    {
        const int s = src.load (std::memory_order_relaxed);
        if (s != shown) { shown = s; repaint(); }
    }

    std::atomic<int>& src;
    int shown = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepDotsComponent)
};

class OnsenDelayAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit OnsenDelayAudioProcessorEditor (OnsenDelayAudioProcessor&);
    ~OnsenDelayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void styleChoiceLabel (juce::Label& label, const juce::String& text);

    OnsenDelayAudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Label titleLabel;
    juce::ToggleButton bypassButton { "Bypass" };
    // Owns the preset picker + the two-way host<->editor program sync.
    factory_ui::PresetSelectorController presetController;

    // Knob row.
    juce::Slider timeSlider, glideSlider, regenSlider, toneSlider, mixSlider, outputSlider;
    juce::Label  timeLabel,  glideLabel,  regenLabel,  toneLabel,  mixLabel,  outputLabel;

    // Sequencer / sync row.
    juce::ComboBox int1Box, int2Box, divisionBox, stepModeBox;
    juce::Label    int1Label, int2Label, divisionLabel, stepModeLabel, stepLabel;
    juce::ToggleButton syncButton { "Sync" };
    juce::TextButton   advanceButton { "Step" };
    StepDotsComponent  stepDots;
    bool advanceDown = false;

    std::unique_ptr<SliderAttachment> timeAtt, glideAtt, regenAtt, toneAtt, mixAtt, outputAtt;
    std::unique_ptr<ButtonAttachment> syncAtt, bypassAtt;
    std::unique_ptr<ComboBoxAttachment> int1Att, int2Att, divisionAtt, stepModeAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnsenDelayAudioProcessorEditor)
};
