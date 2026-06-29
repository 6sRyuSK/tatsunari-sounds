#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "FactoryLookAndFeel.h"

#include <vector>

//
// Grain-cloud visualiser. A GUI-side particle system that mirrors the engine's
// parameters (density spawns dots; spread sets horizontal scatter = pan; pitch
// sets vertical position; grain size sets each dot's lifetime). Overall
// brightness follows the processor's output level. No coupling to the audio
// thread, so no data races. Purely illustrative.
//
class GrainCloudComponent : public juce::Component,
                            private juce::Timer
{
public:
    GrainCloudComponent (GranularDelayAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s)
    {
        particles.resize (kMaxParticles);
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (FactoryLookAndFeel::panel());
        g.fillRoundedRectangle (r, 6.0f);

        auto area = r.reduced (10.0f);

        // Axis hints.
        g.setColour (FactoryLookAndFeel::track());
        g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());
        g.drawVerticalLine   ((int) area.getCentreX(), area.getY(), area.getBottom());
        g.setColour (FactoryLookAndFeel::textDim());
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("L", juce::Rectangle<float> (area.getX(), area.getCentreY() - 14.0f, 16.0f, 12.0f),
                    juce::Justification::centredLeft);
        g.drawText ("R", juce::Rectangle<float> (area.getRight() - 16.0f, area.getCentreY() - 14.0f, 16.0f, 12.0f),
                    juce::Justification::centredRight);
        g.drawText ("pitch +", juce::Rectangle<float> (area.getX() + 4.0f, area.getY(), 60.0f, 12.0f),
                    juce::Justification::centredLeft);

        const float level = juce::jlimit (0.0f, 1.0f, processor.getOutputLevel() * 3.0f);
        for (const auto& p : particles)
        {
            if (! p.active) continue;
            const float env = std::sin (juce::MathConstants<float>::pi * (p.age / p.lifetime)); // fade in/out
            const float alpha = juce::jlimit (0.0f, 1.0f, env * (0.35f + 0.65f * level));
            const float px = area.getX() + p.x * area.getWidth();
            const float py = area.getY() + p.y * area.getHeight();
            const float rad = 2.5f + 4.0f * env;
            g.setColour (FactoryLookAndFeel::accent().withAlpha (alpha * 0.85f));
            g.fillEllipse (px - rad, py - rad, rad * 2.0f, rad * 2.0f);
        }

        g.setColour (FactoryLookAndFeel::track());
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

private:
    struct Particle { bool active = false; float x = 0.5f, y = 0.5f, age = 0.0f, lifetime = 0.1f; };

    float raw (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    void timerCallback() override
    {
        const float dt = 1.0f / 60.0f;
        const float density = raw ("density");
        const float spread  = raw ("spread") * 0.01f;
        const float pitch   = raw ("pitch");
        const float pitchR  = raw ("pitchrand");
        const float lifeSec = raw ("grainsize") * 1.0e-3f;
        const bool  on      = raw ("bypass") < 0.5f;

        // Update existing.
        for (auto& p : particles)
        {
            if (! p.active) continue;
            p.age += dt;
            if (p.age >= p.lifetime) p.active = false;
        }

        // Spawn new at the density rate.
        if (on)
        {
            spawnAccum += density * dt;
            while (spawnAccum >= 1.0f)
            {
                spawnAccum -= 1.0f;
                spawn (spread, pitch, pitchR, lifeSec);
            }
        }
        repaint();
    }

    void spawn (float spread, float pitch, float pitchR, float lifeSec)
    {
        for (auto& p : particles)
        {
            if (p.active) continue;
            const float pan = juce::jlimit (-1.0f, 1.0f, spread * (rng.nextFloat() * 2.0f - 1.0f));
            const float semis = pitch + pitchR * (rng.nextFloat() * 2.0f - 1.0f);
            p.active = true;
            p.age = 0.0f;
            p.lifetime = juce::jmax (0.02f, lifeSec);
            p.x = 0.5f + 0.5f * pan;
            p.y = juce::jlimit (0.02f, 0.98f, 0.5f - semis / 48.0f);
            return;
        }
    }

    static constexpr int kMaxParticles = 256;

    GranularDelayAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    std::vector<Particle> particles;
    juce::Random rng;
    float spawnAccum = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainCloudComponent)
};
