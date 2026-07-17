#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "factory_params/ParamDesc.h"

#include <memory>
#include <vector>

//
// factory_params::buildApvtsLayout — the JUCE-side adapter that turns a
// declarative ParamDesc table into a juce::AudioProcessorValueTreeState::
// ParameterLayout.
//
// This is the ONLY header in factory_params that includes JUCE (the same split
// presets/ uses: PresetBank.h is JUCE-free, ProgramAdapter.h is JUCE-side). It
// reproduces the previously hand-written juce::AudioParameter* objects EXACTLY and
// in TABLE ORDER — the layout.add() order is the host-visible parameter order, so
// it must not change. Bit-exact parity with the former hand-written layout is a
// test gate (resonance-suppressor preset_test's "paramdesc parity").
//
//   Float  -> AudioParameterFloat(ParameterID{id,versionHint}, name,
//               NormalisableRange<float>{min,max,interval} [+ setSkewForCentre],
//               default, AudioParameterFloatAttributes().withLabel(unit))
//             (the label is applied only when `unit` is non-empty)
//   Bool   -> AudioParameterBool  (ParameterID{id,versionHint}, name, default > 0.5)
//   Choice -> AudioParameterChoice(ParameterID{id,versionHint}, name,
//               StringArray(choices), (int) default)
//
// legacyJuceOnly parameters ARE included here — they exist precisely for the
// JUCE/APVTS build (automation-lane + old-session compatibility).
//
namespace factory_params
{
    inline juce::AudioProcessorValueTreeState::ParameterLayout
    buildApvtsLayout (const std::vector<ParamDesc>& descs)
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        for (const auto& d : descs)
        {
            const juce::ParameterID pid { juce::String (d.id), d.versionHint };

            switch (d.type)
            {
                case ParamType::Float:
                {
                    // Build the range exactly as the hand-written code did: the
                    // {min,max,interval} constructor, then setSkewForCentre when a
                    // centre is set (interval 0 == the former 2-arg continuous ctor).
                    juce::NormalisableRange<float> range { d.minValue, d.maxValue, d.interval };
                    if (d.skewCentre > 0.0f)
                        range.setSkewForCentre (d.skewCentre);

                    juce::AudioParameterFloatAttributes attrs;
                    if (! d.unit.empty())
                        attrs = attrs.withLabel (juce::String (d.unit));

                    layout.add (std::make_unique<juce::AudioParameterFloat> (
                        pid, juce::String (d.name), range, d.defaultValue, attrs));
                    break;
                }

                case ParamType::Bool:
                {
                    layout.add (std::make_unique<juce::AudioParameterBool> (
                        pid, juce::String (d.name), d.defaultValue > 0.5f));
                    break;
                }

                case ParamType::Choice:
                {
                    juce::StringArray choices;
                    for (const auto& c : d.choices)
                        choices.add (juce::String (c));

                    layout.add (std::make_unique<juce::AudioParameterChoice> (
                        pid, juce::String (d.name), choices, static_cast<int> (d.defaultValue)));
                    break;
                }
            }
        }

        return layout;
    }
}
