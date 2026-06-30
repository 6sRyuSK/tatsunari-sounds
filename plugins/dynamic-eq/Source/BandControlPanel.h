#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"

#include <initializer_list>
#include <utility>

//
// Detail controls for the currently selected band. Rebinds its APVTS
// attachments whenever the selection changes (Pro-Q-style: edit one band at a
// time). Left half = EQ (type / slope / freq / gain / Q); right half =
// Dynamics (on / threshold / range / attack / release / knee). GUI-thread only.
//
class BandControlPanel : public juce::Component
{
public:
    explicit BandControlPanel (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
        addAndMakeVisible (title);

        bypassButton.setButtonText ("Bypass");
        addAndMakeVisible (bypassButton);

        dynButton.setButtonText ("Dynamics");
        addAndMakeVisible (dynButton);

        typeBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass" }, 1);
        typeBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
        typeBox.onChange = [this] { updateSlopeEnablement(); };
        addAndMakeVisible (typeBox);

        slopeBox.addItemList ({ "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct",
                                "60 dB/oct", "72 dB/oct", "84 dB/oct", "96 dB/oct" }, 1);
        slopeBox.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
        addAndMakeVisible (slopeBox);

        configureKnob (freqSlider, freqLabel, "Freq",   " Hz");
        configureKnob (gainSlider, gainLabel, "Gain",   " dB");
        configureKnob (qSlider,    qLabel,    "Q",      {});
        configureKnob (thrSlider,  thrLabel,  "Thresh", " dB");
        configureKnob (rngSlider,  rngLabel,  "Range",  " dB");
        configureKnob (atkSlider,  atkLabel,  "Attack", " ms");
        configureKnob (relSlider,  relLabel,  "Release"," ms");
        configureKnob (kneeSlider, kneeLabel, "Knee",   " dB");

        setBand (0);
    }

    void setBand (int b)
    {
        band = b;
        title.setText ("BAND " + juce::String (b + 1), juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, FactoryLookAndFeel::bandColour (b));

        // Drop the old attachments BEFORE creating the new ones. Otherwise a
        // new attachment, while syncing its control to the newly-selected
        // band's value, would notify the still-alive old attachment, which
        // writes that value back into the previously-selected band's parameter
        // (i.e. selecting another node would overwrite the band you just left).
        bypassAtt.reset(); typeAtt.reset(); slopeAtt.reset(); dynAtt.reset();
        freqAtt.reset(); gainAtt.reset(); qAtt.reset();
        thrAtt.reset(); rngAtt.reset(); atkAtt.reset(); relAtt.reset(); kneeAtt.reset();

        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        auto id = [b] (const char* s) { return DynamicEqAudioProcessor::pid (b, s); };

        bypassAtt = std::make_unique<BA> (apvts, id ("byp"),   bypassButton);
        typeAtt   = std::make_unique<CA> (apvts, id ("type"),  typeBox);
        slopeAtt  = std::make_unique<CA> (apvts, id ("slope"), slopeBox);
        dynAtt    = std::make_unique<BA> (apvts, id ("dyn"),   dynButton);
        freqAtt   = std::make_unique<SA> (apvts, id ("freq"),  freqSlider);
        gainAtt   = std::make_unique<SA> (apvts, id ("gain"),  gainSlider);
        qAtt      = std::make_unique<SA> (apvts, id ("q"),     qSlider);
        thrAtt    = std::make_unique<SA> (apvts, id ("thr"),   thrSlider);
        rngAtt    = std::make_unique<SA> (apvts, id ("rng"),   rngSlider);
        atkAtt    = std::make_unique<SA> (apvts, id ("atk"),   atkSlider);
        relAtt    = std::make_unique<SA> (apvts, id ("rel"),   relSlider);
        kneeAtt   = std::make_unique<SA> (apvts, id ("knee"),  kneeSlider);

        freqSlider.setNumDecimalPlacesToDisplay (0); // integer Hz
        qSlider.setNumDecimalPlacesToDisplay (2);
        for (auto* sl : { &gainSlider, &thrSlider, &rngSlider, &atkSlider, &relSlider, &kneeSlider })
            sl->setNumDecimalPlacesToDisplay (2);

        updateSlopeEnablement();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);

        if (dividerX >= 0)
        {
            auto r = getLocalBounds().reduced (10).toFloat();
            g.setColour (FactoryLookAndFeel::track().withAlpha (0.8f));
            g.drawVerticalLine (dividerX, r.getY() + 2.0f, r.getBottom() - 2.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);

        auto left  = r.removeFromLeft ((int) (r.getWidth() * 0.45f));
        auto gap   = r.removeFromLeft (14);
        dividerX   = gap.getCentreX();
        auto right = r;

        // ---- left: EQ ----
        auto lrow1 = left.removeFromTop (24);
        bypassButton.setBounds (lrow1.removeFromRight (88));
        title.setBounds (lrow1);
        left.removeFromTop (6);
        auto lrow2 = left.removeFromTop (24);
        typeBox.setBounds (lrow2.removeFromLeft (lrow2.getWidth() / 2 - 4));
        lrow2.removeFromLeft (8);
        slopeBox.setBounds (lrow2);
        left.removeFromTop (8);
        layoutKnobRow (left, { { &freqSlider, &freqLabel }, { &gainSlider, &gainLabel }, { &qSlider, &qLabel } });

        // ---- right: Dynamics ----
        auto rrow1 = right.removeFromTop (24);
        dynButton.setBounds (rrow1.removeFromLeft (140));
        right.removeFromTop (6);
        right.removeFromTop (24); // spacer so the knob row aligns with the left
        right.removeFromTop (8);
        layoutKnobRow (right, { { &thrSlider, &thrLabel }, { &rngSlider, &rngLabel },
                                { &atkSlider, &atkLabel }, { &relSlider, &relLabel },
                                { &kneeSlider, &kneeLabel } });
    }

private:
    void updateSlopeEnablement()
    {
        const int t = typeBox.getSelectedItemIndex();
        const bool cut = (t == (int) factory_core::BandType::HighPass
                          || t == (int) factory_core::BandType::LowPass);
        slopeBox.setEnabled (cut);
        slopeBox.setAlpha (cut ? 1.0f : 0.45f);
    }

    void layoutKnobRow (juce::Rectangle<int> area,
                        std::initializer_list<std::pair<juce::Slider*, juce::Label*>> items)
    {
        const int n = (int) items.size();
        if (n == 0) return;
        const int colW = area.getWidth() / n;
        int i = 0;
        for (auto& it : items)
        {
            auto col = (i == n - 1) ? area : area.removeFromLeft (colW);
            it.second->setBounds (col.removeFromTop (16));
            it.first->setBounds (col);
            ++i;
        }
    }

    void configureKnob (juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::String& suffix)
    {
        // Set text colours on the component directly so they don't depend on
        // which LookAndFeel was active when the text box was first created.
        slider.setColour (juce::Slider::textBoxTextColourId, FactoryLookAndFeel::text());
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
        slider.setTextValueSuffix (suffix);
        addAndMakeVisible (slider);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
        addAndMakeVisible (label);
    }

    juce::AudioProcessorValueTreeState& apvts;
    int band = 0;
    int dividerX = -1;

    juce::Label title;
    juce::ToggleButton bypassButton, dynButton;
    juce::ComboBox typeBox, slopeBox;
    juce::Slider freqSlider, gainSlider, qSlider, thrSlider, rngSlider, atkSlider, relSlider, kneeSlider;
    juce::Label  freqLabel, gainLabel, qLabel, thrLabel, rngLabel, atkLabel, relLabel, kneeLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bypassAtt, dynAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAtt, slopeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   freqAtt, gainAtt, qAtt,
                                                                            thrAtt, rngAtt, atkAtt, relAtt, kneeAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandControlPanel)
};
