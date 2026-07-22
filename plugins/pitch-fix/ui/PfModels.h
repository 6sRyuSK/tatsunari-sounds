#pragma once
//
// plugins/pitch-fix/ui/PfModels.h — the visage-free seams between the Pf editor
// and its host shell: the lock-free status feed (audio → UI text read-outs) and
// the preset list model. JUCE-free AND visage-free, so a harness can mock both
// and the contracts stay headless-compilable.
//
#include <atomic>
#include <string>
#include <vector>

namespace pf_ui
{
    // Pointers into the live PfCore's published atomics (the shell wires them in
    // makePfClapEditor; a harness can point them at its own dummies). All reads
    // are relaxed atomic loads on the UI thread — no locks, no copies.
    struct PfUiFeed
    {
        std::atomic<float>* detectedHz     = nullptr;  // 0 == unvoiced
        std::atomic<float>* targetHz       = nullptr;  // current scale target
        std::atomic<float>* shiftCents     = nullptr;  // live correction amount
        std::atomic<int>*   latencySamples = nullptr;  // reported lookahead
        std::atomic<float>* sampleRateHz   = nullptr;  // prepared rate (for ms)
    };

    // Program list the preset selector renders (index 0 == Init, then the bank).
    // load() applies through the real PresetSession in the shell build.
    class PfPresetModel
    {
    public:
        virtual ~PfPresetModel() = default;

        virtual std::vector<std::string> names() const = 0;
        virtual int  currentIndex() const = 0;
        virtual bool load (int index) = 0;
    };
} // namespace pf_ui
