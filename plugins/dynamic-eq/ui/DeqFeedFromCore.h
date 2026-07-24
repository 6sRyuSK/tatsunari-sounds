#pragma once
//
// plugins/dynamic-eq/ui/DeqFeedFromCore.h — the REAL deq_ui::DeqFeed over the shell's
// live deq_core::DeqCore. Zero-copy forwarding of the core's published analyser rings +
// per-band live gains to the editor, the exact hand-off the JUCE editor got from the
// processor (copyAnalyzerSamples / getLiveGainDb). GUI thread; the core's atomics are
// the audio-thread side of the lock-free hand-off.
//
#include "DeqModels.h"
#include "DeqCore.h"

namespace deq_ui
{
    class DeqFeedFromCore final : public DeqFeed
    {
    public:
        explicit DeqFeedFromCore (deq_core::DeqCore& core) : core_ (core) {}

        void copyAnalyzerSamples (float* dst, int n, bool post) const override
        {
            core_.copyAnalyzerSamples (dst, n, post);
        }
        float  liveGainDb (int band) const override { return core_.liveGainDb (band); }
        double sampleRate() const override           { return core_.sampleRate(); }
        int    numBands() const override             { return core_.numBands(); }

    private:
        deq_core::DeqCore& core_;
    };
} // namespace deq_ui
