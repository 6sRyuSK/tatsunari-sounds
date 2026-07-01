#pragma once
//
// NamModel — binds one NeuralAmpModelerCore model (nam::DSP) to the headless
// factory_core::MonoProcessor interface the routing engine consumes. Deliberately
// JUCE-free (std::string paths) so it can be exercised without a plugin host. This
// is the only translation unit that includes the NAM headers.
//
// True stereo: NAM models are mono and stateful, so the plugin loads one NamModel
// per (slot, channel); L and R never share an instance.
//
#include "factory_core/NamRoutingEngine.h"   // MonoProcessor

#include <memory>
#include <string>
#include <vector>

namespace nam { class DSP; }

class NamModel final : public factory_core::MonoProcessor
{
public:
    NamModel();
    ~NamModel() override;

    // Load a .nam from disk and prepare it for up to `maxBuffer` frames at
    // `sampleRate`. Heavy (parses weights + prewarms) => call off the audio thread.
    // Returns false and fills `error` on failure.
    bool load (const std::string& path, double sampleRate, int maxBuffer, std::string& error);

    // Audio thread: process a mono block in place (chunked to the model's max buffer).
    void processReplacing (float* block, int numSamples) noexcept override;
    void reset() noexcept override;

    const std::string& name() const noexcept { return modelName; }
    double expectedSampleRate() const noexcept { return expSR; }
    bool   hasLoudness() const noexcept { return loudHas; }
    double loudness() const noexcept { return loudVal; }

private:
    std::unique_ptr<nam::DSP> dsp;
    std::vector<float> outBuf;        // NAM writes to a separate output buffer
    int    maxBuffer = 0;
    std::string modelName;
    double expSR    = -1.0;
    bool   loudHas  = false;
    double loudVal  = 0.0;

    NamModel (const NamModel&) = delete;
    NamModel& operator= (const NamModel&) = delete;
};
