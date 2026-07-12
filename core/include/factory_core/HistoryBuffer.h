#pragma once
//
// factory_core/HistoryBuffer.h -- a long-history ring buffer with a
// fractional (linearly interpolated), variable-age read head. This is the
// "tape" primitive behind OmoideEcho's 120-second memory: every prepared
// sample rate tick records ALL channels through write(), a single shared
// writePos then advance()s once, and readAtAge() retrieves any past instant
// by its age in samples (fractional ages interpolate). No feedback, no
// nonlinearity, no smoothing -- purely a recorder + variable-delay reader.
// Header-only, JUCE-independent. prepare() allocates; reset()/write()/
// advance()/readAtAge() are allocation-free, lock-free and noexcept.
//
// Constants (spec'd, tests depend on them):
//   kMaxChannels   = 2
//   kMaxHistorySec = 120.0   -- the reference/designed history length (the
//                               24 kHz/2-channel/120 s configuration used by
//                               OmoideEcho is float storage of
//                               120 * 24000 * 2 * 4 bytes ~= 23 MB). This
//                               primitive is generic: prepare() honours
//                               whatever historySeconds it is given (no
//                               internal clamp to this constant) -- it is
//                               published as documentation of the intended
//                               worst-case budget, not an enforced ceiling.
//   kMinReadAgeSec = 0.0     -- the NOMINAL minimum age ("now", the write
//                               position). The actual usable minimum is 1
//                               sample (see readAtAge's causality note
//                               below); kMinReadAgeSec documents the
//                               conceptual floor a caller reasons about in
//                               seconds, not the raw clamp bound in samples.
//
// Storage contract: each channel's history is stored as FLOAT (not double)
// specifically to hit the ~23 MB budget above; all arithmetic (indexing,
// interpolation) is done in double and only the final stored/loaded value is
// narrowed to/from float.
//
// write/advance/readAtAge ordering (READ-BEFORE-WRITE per tick, mandatory):
// write(ch, x) stores x at the CURRENT writePos slot but does NOT move the
// pointer; advance() moves writePos forward by exactly one sample and is
// shared across channels, so the intended per-tick usage is:
//   history.write(0, l); history.write(1, r); history.advance();
// A caller that wants to read from history to help compute the very value it
// is about to write (e.g. an echo/feedback tap) MUST call readAtAge() before
// write() for that same tick -- write() has not happened yet, so writePos
// still points at "now" (not yet recorded). This is why readAtAge's minimum
// honoured age is 1, not 0: age 0 would name the in-flight, not-yet-written
// sample, which is a causality violation, not merely a buffer-safety margin.
//
// Zero-history contract: prepare()/reset() zero every sample of storage.
// Because advance() only overwrites one ring slot per sample and a full trip
// around the ring takes exactly `capacity` calls, ANY age that names a slot
// not yet written since the last reset() reads back as exactly 0.0 (the
// slot's untouched initial value) -- there is no separate "valid data"
// tracking; the zero-fill IS the "nothing recorded here yet" answer.
//
// readAtAge clamp / saturation contract (tests depend on this exact bound):
// ageSamples is clamped to [1, capacity - 2] before use, where
// capacity = ceil(historySeconds * sampleRate) + 8 (prepare()'s allocation).
// The lower bound (1) is the causality floor above. The upper bound
// (capacity - 2) keeps the interpolation neighbour (floor(age) + 1) safely
// inside the allocated ring even at the oldest requested age; ages beyond it
// SATURATE to capacity - 2 rather than wrapping onto not-yet-overwritten (or
// about-to-be-overwritten) data -- callers should treat this as a defensive
// backstop only: the engine (OmoideEcho) pre-clamps every age it requests to
// well inside [1, capacity - 2] (max delay 10 s, max scan age 119 s, against
// a 120 s capacity), so this saturation is not normally reached.
//
// readAtAge age-exactness contract (tests depend on this): with age == A
// (an exact integer, no clamping in effect), readAtAge returns EXACTLY the
// value stored by the write() call A samples before the current tick -- i.e.
// there is no off-by-one baked into this primitive. (This differs from
// factory_core::DelayLine's readInterpolated(), whose write() advances its
// own pointer immediately, so age 0 there means "the last completed write";
// here, because advance() is a separate, shared, once-per-tick call and
// readAtAge happens before this tick's own write(), age 0 would name the
// current in-flight sample -- hence the minimum usable age is 1, as above.)
//
#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class HistoryBuffer
    {
    public:
        static constexpr int    kMaxChannels   = 2;
        static constexpr double kMaxHistorySec = 120.0;
        static constexpr double kMinReadAgeSec = 0.0;

        // Allocates ceil(historySeconds * sampleRate) + 8 samples of storage
        // PER CHANNEL (always kMaxChannels lanes are allocated regardless of
        // numChannels -- the shared writePos design and the ~23 MB budget
        // above both assume the fixed 2-lane shape, and kMaxChannels is small
        // enough that always allocating both costs nothing worth avoiding).
        // Not real-time safe (allocates); call from prepareToPlay only.
        void prepare (double sampleRate, int numChannels, double historySeconds)
        {
            fs       = (sampleRate > 0.0) ? sampleRate : 1.0;
            channels = std::clamp (numChannels, 1, kMaxChannels);

            const double hs = std::max (0.0, historySeconds);
            capacity = std::max (8, (int) std::ceil (hs * fs) + 8);

            for (int ch = 0; ch < kMaxChannels; ++ch)
                buf[ch].assign ((size_t) capacity, 0.0f);

            reset();
        }

        // Deterministic: zeroes all storage and resets the write pointer to 0.
        // Two runs from reset() with identical write()/advance() calls are
        // bit-identical.
        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
                std::fill (buf[ch].begin(), buf[ch].end(), 0.0f);
            writePos = 0;
        }

        // Stores x at the current write slot for channel ch. Non-finite
        // values are ignored -- written as 0.0f instead (the previous content
        // of that ring slot, if any, is still overwritten; there is no
        // "keep previous" semantics here, unlike the engine-level setters --
        // this is a recording primitive, not a smoothed parameter).
        void write (int ch, double x) noexcept
        {
            const int c = std::clamp (ch, 0, kMaxChannels - 1);
            const float v = std::isfinite (x) ? (float) x : 0.0f;
            buf[c][(size_t) writePos] = v;
        }

        // Advances the shared write pointer by one sample. Call once per
        // sample tick, after write() has been called for every active
        // channel that tick.
        void advance() noexcept
        {
            if (++writePos >= capacity) writePos = 0;
        }

        // Linearly interpolated read at `ageSamples` samples before "now"
        // (see the header's age-exactness and clamp/saturation contracts).
        double readAtAge (int ch, double ageSamples) const noexcept
        {
            const int c = std::clamp (ch, 0, kMaxChannels - 1);
            const double a = std::clamp (ageSamples, 1.0, (double) (capacity - 2));

            double pos = (double) writePos - a;
            while (pos < 0.0)             pos += (double) capacity;
            while (pos >= (double) capacity) pos -= (double) capacity;

            const int i0 = (int) pos;
            const double frac = pos - (double) i0;
            const int i1 = (i0 + 1 >= capacity) ? 0 : i0 + 1;

            const double v0 = (double) buf[c][(size_t) i0];
            const double v1 = (double) buf[c][(size_t) i1];
            return v0 + frac * (v1 - v0);
        }

        int    numChannels()      const noexcept { return channels; }
        int    capacitySamples()  const noexcept { return capacity; }
        double sampleRate()       const noexcept { return fs; }

    private:
        double fs        = 1.0;
        int    channels  = kMaxChannels;
        int    capacity  = 8;
        int    writePos  = 0;

        std::vector<float> buf[kMaxChannels];
    };
} // namespace factory_core
