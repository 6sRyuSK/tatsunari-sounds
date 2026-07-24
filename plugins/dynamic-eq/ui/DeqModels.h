#pragma once
//
// plugins/dynamic-eq/ui/DeqModels.h — the visage-free (and JUCE-free) seams between
// the Dynamic EQ editor and its host shell: the analyser / live-gain feed (audio → UI)
// and the factory-preset list model. Keeping these framework-free lets the editor's
// pure logic stay headless-compilable and lets the shell supply real backends
// (DeqFeedFromCore over the live DeqCore; a PresetSession-backed preset model).
//
#include <string>
#include <vector>

namespace deq_ui
{
    // Audio → UI hand-off. The editor pulls the most recent analyser samples (pre/post
    // EQ) and each band's live (post-dynamics) gain, all lock-free reads on the core's
    // published state. The shell wires a DeqFeedFromCore over the live DeqCore.
    class DeqFeed
    {
    public:
        virtual ~DeqFeed() = default;

        // Copy the latest `n` mono analyser samples into `dst`. `post` selects the
        // post-EQ ring instead of the pre-EQ input.
        virtual void copyAnalyzerSamples (float* dst, int n, bool post) const = 0;
        // Per-band effective gain (dB) including the live dynamic offset.
        virtual float liveGainDb (int band) const = 0;
        virtual double sampleRate() const = 0;
        virtual int    numBands() const = 0;
    };

    // Program list the preset selector renders (index 0 == Init, then the bank).
    // load() applies through the real PresetSession in the shell build.
    class DeqPresetModel
    {
    public:
        virtual ~DeqPresetModel() = default;

        virtual std::vector<std::string> names() const = 0;
        virtual int  currentIndex() const = 0;
        virtual bool load (int index) = 0;
    };
} // namespace deq_ui
