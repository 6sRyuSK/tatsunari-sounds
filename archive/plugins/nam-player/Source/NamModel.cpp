#include "NamModel.h"

#include "NAM/get_dsp.h"
#include "NAM/dsp.h"

#include <algorithm>
#include <exception>
#include <filesystem>

NamModel::NamModel() = default;
NamModel::~NamModel() = default;   // out-of-line: nam::DSP is complete in this TU

bool NamModel::load (const std::string& path, double sampleRate, int maxBuf, std::string& error)
{
    try
    {
        auto d = nam::get_dsp (std::filesystem::path (path));
        if (! d) { error = "get_dsp returned null"; return false; }

        d->Reset (sampleRate, maxBuf);     // allocates internal buffers + prewarms
        dsp        = std::move (d);
        maxBuffer  = std::max (1, maxBuf);
        outBuf.assign ((size_t) maxBuffer, 0.0f);
        modelName  = std::filesystem::path (path).stem().string();
        expSR      = dsp->GetExpectedSampleRate();
        loudHas    = dsp->HasLoudness();
        loudVal    = loudHas ? dsp->GetLoudness() : 0.0;
        return true;
    }
    catch (const std::exception& e) { error = e.what();  dsp.reset(); return false; }
    catch (...)                     { error = "unknown"; dsp.reset(); return false; }
}

void NamModel::processReplacing (float* block, int numSamples) noexcept
{
    if (dsp == nullptr || numSamples <= 0)
        return;

    int done = 0;
    while (done < numSamples)
    {
        const int chunk = std::min (numSamples - done, maxBuffer);
        NAM_SAMPLE* in  = block + done;      // NAM_SAMPLE == float (NAM_SAMPLE_FLOAT)
        NAM_SAMPLE* out = outBuf.data();
        dsp->process (&in, &out, chunk);
        std::copy (out, out + chunk, block + done);
        done += chunk;
    }
}

void NamModel::reset() noexcept
{
    // NAM's Reset() reallocates (not audio-thread-safe), and there is no cheap state
    // clear, so this is a no-op: the model's internal state flushes within its
    // receptive field after a transition. State is (re)initialised at load time.
}
