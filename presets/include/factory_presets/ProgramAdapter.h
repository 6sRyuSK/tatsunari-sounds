#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "factory_presets/PresetBank.h"

#include <atomic>
#include <vector>

//
// factory_presets::ProgramAdapter — rides the JUCE program API so factory
// presets are reachable from both the host's program list (VST3/AU factory
// presets) and the editor's preset selector, without a bespoke state channel.
//
// A Processor holds one adapter, calls configure() once from its constructor,
// and forwards the five program-API methods to it. Persistence is a single
// `presetIndex` attribute the Processor appends to / reads back from its state
// XML (append-only: existing sessions stay compatible, no major bump).
//
// Real-time safety (hard rule): setCurrentProgram() may be called from any
// thread by the host, so its apply path must not allocate, lock, or syscall.
// configure() therefore PRECOMPUTES, on the message thread, a per-program table
// of normalised targets in a fixed parameter order; setCurrentProgram() only
// stores an atomic index and writes each parameter via setValueNotifyingHost.
// It never touches the ValueTree (apvts.replaceState is message-thread only and
// is forbidden here).
//
namespace factory_presets
{
    class ProgramAdapter
    {
    public:
        ProgramAdapter() = default;

        // Configure once on the message thread (the Processor constructor, after
        // its APVTS is built). `excludeIDs` are parameters presets must never
        // touch (e.g. "bypass", monitoring toggles) — they are left untouched by
        // every program, including Init.
        void configure (juce::AudioProcessorValueTreeState& apvts,
                        const PresetBank& presetBank,
                        const char* const* excludeIDs = nullptr,
                        int numExcludeIDs = 0)
        {
            bank        = &presetBank;
            numPrograms = 1 + juce::jmax (0, presetBank.numPresets);
            current.store (0, std::memory_order_relaxed);

            // Managed parameters, in a fixed order, minus the exclusion list.
            params.clear();
            for (auto* base : apvts.processor.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
                {
                    if (isExcluded (rp->getParameterID(), excludeIDs, numExcludeIDs))
                        continue;
                    params.push_back (rp);
                }

            const size_t np = params.size();
            targets.assign ((size_t) numPrograms * np, 0.0f);

            // Program 0 (Init): every managed parameter to its default.
            for (size_t i = 0; i < np; ++i)
                targets[i] = params[i]->getDefaultValue();

            // Programs 1..N: the preset's value where the parameter is listed,
            // its default otherwise (so no residue from the prior program).
            for (int prog = 1; prog < numPrograms; ++prog)
            {
                const Preset& preset = presetBank.presets[prog - 1];
                float* row = targets.data() + (size_t) prog * np;
                for (size_t i = 0; i < np; ++i)
                {
                    auto* rp = params[i];
                    float norm = rp->getDefaultValue();
                    const auto id = rp->getParameterID();
                    for (int e = 0; e < preset.numParams; ++e)
                        if (id == preset.params[e].paramID)
                        {
                            norm = rp->convertTo0to1 (preset.params[e].value);
                            break;
                        }
                    row[i] = norm;
                }
            }
        }

        int getNumPrograms() const noexcept { return numPrograms; }

        int getCurrentProgram() const noexcept { return current.load (std::memory_order_relaxed); }

        // RT-safe: atomic index store + preallocated per-parameter writes only.
        void setCurrentProgram (int index)
        {
            if (index < 0 || index >= numPrograms || params.empty())
            {
                if (index >= 0 && index < numPrograms)
                    current.store (index, std::memory_order_relaxed);
                return;
            }

            current.store (index, std::memory_order_relaxed);

            const size_t np = params.size();
            const float* row = targets.data() + (size_t) index * np;
            for (size_t i = 0; i < np; ++i)
                params[i]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, row[i]));
        }

        juce::String getProgramName (int index) const
        {
            if (index == 0)
                return "Init";
            if (bank != nullptr && index >= 1 && index <= bank->numPresets)
                return juce::String (bank->presets[index - 1].name);
            return {};
        }

        // Persistence (message thread). Append-only: unknown/absent attribute
        // reads back as program 0, keeping old sessions compatible.
        void writeStateAttribute (juce::XmlElement& xml) const
        {
            xml.setAttribute ("presetIndex", current.load (std::memory_order_relaxed));
        }

        void readStateAttribute (const juce::XmlElement& xml)
        {
            const int idx = xml.getIntAttribute ("presetIndex", 0);
            current.store ((idx >= 0 && idx < numPrograms) ? idx : 0, std::memory_order_relaxed);
        }

    private:
        static bool isExcluded (const juce::String& id, const char* const* excludeIDs, int numExcludeIDs)
        {
            for (int k = 0; k < numExcludeIDs; ++k)
                if (id == excludeIDs[k])
                    return true;
            return false;
        }

        const PresetBank* bank = nullptr;
        int numPrograms = 1;
        std::atomic<int> current { 0 };

        // Fixed-order managed parameters and their per-program normalised targets
        // (row-major: numPrograms rows × params.size() columns). Preallocated in
        // configure() so the apply path is allocation-free.
        std::vector<juce::RangedAudioParameter*> params;
        std::vector<float>                        targets;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramAdapter)
    };
}
