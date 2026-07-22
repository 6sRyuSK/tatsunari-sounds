#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"

//
// Vertical gain-reduction meter. Polls the processor's published GR (<= 0 dB)
// at ~30 fps and draws a bar growing downward from the top, with a smoothed
// peak hold. Pure GUI-thread code.
//
class GainReductionMeter : public juce::Component,
                           private juce::Timer
{
public:
    explicit GainReductionMeter (BusCompressorAudioProcessor& p) : processor (p)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r, 6.0f, 1.0f);

        auto scale = r.reduced (8.0f);
        scale.removeFromBottom (16.0f); // room for the readout

        // dB ticks (0 .. -maxDb).
        g.setColour (FactoryLookAndFeel::textDim());
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (int db = 0; db >= -(int) maxDb; db -= 5)
        {
            const float t = (float) (-db) / maxDb;
            const float ly = scale.getY() + t * scale.getHeight();
            g.drawHorizontalLine ((int) ly, scale.getX(), scale.getRight());
            g.drawText (juce::String (db),
                        juce::Rectangle<float> (scale.getRight() - 22.0f, ly - 6.0f, 20.0f, 12.0f),
                        juce::Justification::centredRight);
        }

        // Reduction bar (downward from the top).
        const float gr = juce::jlimit (0.0f, maxDb, -displayedGr);
        const float h = (gr / maxDb) * scale.getHeight();
        auto bar = juce::Rectangle<float> (scale.getX(), scale.getY(),
                                           scale.getWidth() * 0.55f, h);
        g.setColour (FactoryLookAndFeel::accent().withAlpha (0.85f));
        g.fillRoundedRectangle (bar, 3.0f);

        // Numeric readout.
        g.setColour (FactoryLookAndFeel::text());
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (juce::String (displayedGr, 1) + " dB",
                    getLocalBounds().removeFromBottom (18),
                    juce::Justification::centred);
    }

private:
    void timerCallback() override
    {
        const float gr = processor.getGainReductionDb(); // <= 0
        // Fast attack, slow release for a readable meter.
        if (gr < displayedGr) displayedGr = gr;
        else                  displayedGr += (gr - displayedGr) * 0.3f;
        repaint();
    }

    BusCompressorAudioProcessor& processor;
    float displayedGr = 0.0f;
    static constexpr float maxDb = 20.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainReductionMeter)
};
