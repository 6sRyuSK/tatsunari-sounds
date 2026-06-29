#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"

//
// Vertical per-band gain-reduction meter. Polls the processor's published GR for
// one band at ~30 fps with a smoothed release. GUI-thread only.
//
class BandMeter : public juce::Component,
                  private juce::Timer
{
public:
    BandMeter (VocalMbCompAudioProcessor& p, int bandIndex) : processor (p), band (bandIndex)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        auto scale = r.reduced (4.0f);
        const float gr = juce::jlimit (0.0f, maxDb, -displayedGr);
        const float h = (gr / maxDb) * scale.getHeight();
        auto bar = juce::Rectangle<float> (scale.getX(), scale.getY(), scale.getWidth(), h);
        g.setColour (FactoryLookAndFeel::accent().withAlpha (0.85f));
        g.fillRoundedRectangle (bar, 2.0f);
    }

private:
    void timerCallback() override
    {
        const float gr = processor.getBandGainReductionDb (band);
        if (gr < displayedGr) displayedGr = gr;
        else                  displayedGr += (gr - displayedGr) * 0.3f;
        repaint();
    }

    VocalMbCompAudioProcessor& processor;
    int band = 0;
    float displayedGr = 0.0f;
    static constexpr float maxDb = 18.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandMeter)
};
