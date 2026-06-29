#pragma once
//
// factory_core/DelayLine.h — a circular delay line with fractional (linearly
// interpolated) reads. The buffer is allocated in prepare(); read/write are
// allocation-free and safe on the audio thread. Header-only, JUCE-independent.
//
#include <algorithm>
#include <vector>

namespace factory_core
{
    class DelayLine
    {
    public:
        void prepare (int maxSamples)
        {
            size = std::max (4, maxSamples);
            buffer.assign ((size_t) size, 0.0);
            writePos = 0;
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0);
            writePos = 0;
        }

        void write (double x) noexcept
        {
            buffer[(size_t) writePos] = x;
            if (++writePos >= size) writePos = 0;
        }

        // Sample `delaySamples` behind the most recently written sample,
        // linearly interpolated. delaySamples == 0 returns the last write.
        double readInterpolated (double delaySamples) const noexcept
        {
            double readPos = (double) (writePos - 1) - delaySamples;
            while (readPos < 0.0) readPos += (double) size;
            while (readPos >= (double) size) readPos -= (double) size;

            const int i0 = (int) readPos;
            const double frac = readPos - (double) i0;
            const int i1 = (i0 + 1 >= size) ? 0 : i0 + 1;
            return buffer[(size_t) i0] + frac * (buffer[(size_t) i1] - buffer[(size_t) i0]);
        }

        int getSize() const noexcept { return size; }

    private:
        std::vector<double> buffer;
        int size = 0;
        int writePos = 0;
    };
} // namespace factory_core
