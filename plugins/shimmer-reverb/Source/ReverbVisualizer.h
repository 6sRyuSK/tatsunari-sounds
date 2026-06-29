#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"

#include <vector>

//
// Shimmer visualiser. A bottom glow represents the reverb body (brightness from
// output level); motes rise upward (octave-up shimmer floating up), spawned at a
// rate set by the Shimmer amount and the output level, fading as they reach the
// top. GUI-side only; no coupling to the audio thread.
//
class ReverbVisualizer : public juce::Component,
                         private juce::Timer
{
public:
    ReverbVisualizer (ShimmerReverbAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s)
    {
        motes.resize (kMaxMotes);
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 6.0f);

        auto area = r.reduced (8.0f);
        const float level = juce::jlimit (0.0f, 1.0f, smoothedLevel * 3.0f);

        // Reverb body: a soft glow rising from the bottom.
        juce::ColourGradient grad (FactoryLookAndFeel::accent().withAlpha (0.02f + 0.22f * level),
                                   area.getCentreX(), area.getBottom(),
                                   FactoryLookAndFeel::accent().withAlpha (0.0f),
                                   area.getCentreX(), area.getY() + area.getHeight() * 0.25f, false);
        g.setGradientFill (grad);
        g.fillRect (area);

        // Rising motes.
        for (const auto& m : motes)
        {
            if (! m.active) continue;
            const float env = std::sin (juce::MathConstants<float>::pi * (m.age / m.lifetime));
            const float px = area.getX() + m.x * area.getWidth();
            const float py = area.getY() + m.y * area.getHeight();
            const float rad = 1.5f + 2.5f * env;
            g.setColour (FactoryLookAndFeel::accent().withAlpha (juce::jlimit (0.0f, 1.0f, env * 0.8f)));
            g.fillEllipse (px - rad, py - rad, rad * 2.0f, rad * 2.0f);
        }

        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

private:
    struct Mote { bool active = false; float x = 0.5f, y = 1.0f, age = 0.0f, lifetime = 1.0f, speed = 0.2f; };

    float raw (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    void timerCallback() override
    {
        const float dt = 1.0f / 60.0f;
        smoothedLevel += (processor.getOutputLevel() - smoothedLevel) * 0.2f;
        const float level = juce::jlimit (0.0f, 1.0f, smoothedLevel * 3.0f);
        const float shimmer = raw ("shimmer") * 0.01f;
        const bool  on = raw ("bypass") < 0.5f;

        for (auto& m : motes)
        {
            if (! m.active) continue;
            m.age += dt;
            m.y -= m.speed * dt;
            if (m.age >= m.lifetime || m.y < 0.0f) m.active = false;
        }

        if (on)
        {
            spawnAccum += shimmer * (0.2f + level) * 30.0f * dt;
            while (spawnAccum >= 1.0f)
            {
                spawnAccum -= 1.0f;
                spawn();
            }
        }
        repaint();
    }

    void spawn()
    {
        for (auto& m : motes)
        {
            if (m.active) continue;
            m.active = true;
            m.age = 0.0f;
            m.x = rng.nextFloat();
            m.y = 1.0f;
            m.lifetime = 1.6f + rng.nextFloat() * 1.4f;
            m.speed = 0.3f + rng.nextFloat() * 0.35f;
            return;
        }
    }

    static constexpr int kMaxMotes = 220;

    ShimmerReverbAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    std::vector<Mote> motes;
    juce::Random rng;
    float spawnAccum = 0.0f;
    float smoothedLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbVisualizer)
};
