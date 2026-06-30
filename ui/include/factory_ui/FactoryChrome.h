#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_ui/FactoryLookAndFeel.h"

//
// factory_ui chrome — small shared helpers so every editor paints the same
// background, card shadows and knobs as the Dynamic EQ. Header-only.
//
namespace factory_ui
{
    // Warm-white vertical gradient background filling `bounds`.
    inline void paintBackground (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        juce::ColourGradient bg (FactoryLookAndFeel::background(), 0.0f, (float) bounds.getY(),
                                 FactoryLookAndFeel::backgroundLo(), 0.0f, (float) bounds.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRect (bounds);
    }

    // Soft drop shadow behind a rounded card. Call before painting the card.
    inline void dropShadowFor (juce::Graphics& g, juce::Rectangle<int> card, float radius = 10.0f)
    {
        juce::DropShadow ds (FactoryLookAndFeel::shadow(), 16, { 0, 5 });
        juce::Path path; path.addRoundedRectangle (card.toFloat(), radius);
        ds.drawForPath (g, path);
    }

    // White card fill + soft track outline.
    inline void paintCard (juce::Graphics& g, juce::Rectangle<float> card, float radius = 10.0f)
    {
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (card, radius);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (card.reduced (0.5f), radius, 1.2f);
    }

    // Configure a rotary knob + its caption label in the house style. Does not
    // add them to a parent (the editor keeps ownership / visibility).
    inline void styleKnob (juce::Slider& slider, juce::Label& label,
                           const juce::String& name, const juce::String& suffix)
    {
        slider.setColour (juce::Slider::textBoxTextColourId, FactoryLookAndFeel::text());
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
        slider.setTextValueSuffix (suffix);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
    }
}
