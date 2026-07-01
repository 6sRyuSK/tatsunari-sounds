#pragma once
//
// factory_core/NamRoutingEngine.h — headless, allocation-free routing engine for the
// NAM Player. It composes up to three amp "slots" (each a MonoProcessor) and mixes
// them per the user's per-slot Series/Parallel choice. The concrete binding to
// NeuralAmpModelerCore lives in the JUCE wrapper (Source/NamModel.*); the engine only
// sees the abstract MonoProcessor, so the routing algebra is exercised headless by
// injecting a KNOWN nonlinearity (e.g. tanh) with an independent oracle.
//
// True stereo: NAM models are mono and stateful, so L and R must run through separate
// model instances that never share state. The engine therefore holds a MonoProcessor*
// per (slot, channel) and applies the same routing/gains to both channels with the
// channel-specific models.
//
// Per-slot gains are smoothed inside the engine with a linear ramp whose length tracks
// the sample rate (so the sample-rate matrix exercises it). The ramps are filled once
// per block and shared by both channels, so a smoother advances exactly once per
// sample regardless of channel count.
//
// Routing spec (per channel c, slots in fixed order 0,1,2):
//     running = in_c ; par = 0 ; anySeries = anyParallel = false
//     for each slot k:
//         if !enabled[k] or model[k][c] == null: continue          // no-op
//         s = running * inGain[k]                                   // tap current series state
//         s = model[k][c](s)                                        // nonlinear amp
//         s *= out[k]
//         if Series:   running = s ; anySeries   = true
//         else:        par += s * balanceGain(balance[k], c) ; anyParallel = true
//     out_c = (anySeries || anyParallel) ? ((anySeries ? running : 0) + par) : in_c
//
// Consequences (asserted by the headless test): a Parallel slot taps the series signal
// at its position, so slot order matters; a disabled or unloaded slot is a true no-op;
// all-parallel mutes the series center; and with no active slot the engine passes the
// input through unchanged (important for pluginval with no model loaded, and UX).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace factory_core
{
    // A mono, in-place, real-time-safe processor. Concrete NAM binding lives in the
    // wrapper; tests inject a known nonlinearity to give the routing an oracle.
    struct MonoProcessor
    {
        virtual ~MonoProcessor() = default;
        virtual void processReplacing (float* block, int numSamples) noexcept = 0; // in == out
        virtual void reset() noexcept {}
    };

    class NamRoutingEngine
    {
    public:
        enum class Mode { Series, Parallel };
        static constexpr int kNumSlots = 3;
        static constexpr int kNumChannels = 2;

        NamRoutingEngine()
        {
            for (int k = 0; k < kNumSlots; ++k)
            {
                sInGain[k].setTarget (1.0f); sInGain[k].snap();
                sOut[k].setTarget    (1.0f); sOut[k].snap();
                sBal[k].setTarget    (0.0f); sBal[k].snap();
            }
        }

        void prepare (double sampleRate, int maxBlock)
        {
            maxBlk = std::max (1, maxBlock);
            const int rampLen = std::max (1, (int) std::lround (0.005 * sampleRate)); // ~5 ms
            for (int k = 0; k < kNumSlots; ++k)
            {
                sInGain[k].setLen (rampLen);
                sOut[k].setLen    (rampLen);
                sBal[k].setLen    (rampLen);
                bufInGain[k].assign ((size_t) maxBlk, 0.0f);
                bufOut[k].assign    ((size_t) maxBlk, 0.0f);
                bufBal[k].assign    ((size_t) maxBlk, 0.0f);
            }
            running.assign ((size_t) maxBlk, 0.0f);
            parBus.assign  ((size_t) maxBlk, 0.0f);
            scratch.assign ((size_t) maxBlk, 0.0f);
        }

        // Clear audio-adjacent state and settle the smoothers on their targets. Also
        // resets any currently held models.
        void reset() noexcept
        {
            std::fill (running.begin(), running.end(), 0.0f);
            std::fill (parBus.begin(),  parBus.end(),  0.0f);
            for (int k = 0; k < kNumSlots; ++k)
                for (int c = 0; c < kNumChannels; ++c)
                    if (model[k][c] != nullptr)
                        model[k][c]->reset();
            snap();
        }

        void snap() noexcept
        {
            for (int k = 0; k < kNumSlots; ++k) { sInGain[k].snap(); sOut[k].snap(); sBal[k].snap(); }
        }

        void setModel (int slot, int ch, MonoProcessor* m) noexcept { model[slot][ch] = m; }

        // Update a slot's routing and (smoothed) gains. inGainLin/outLin are linear.
        void setSlot (int slot, bool en, Mode m, float inGainLin, float outLin, float balance) noexcept
        {
            enabled[slot] = en;
            mode[slot]    = m;
            sInGain[slot].setTarget (inGainLin);
            sOut[slot].setTarget    (outLin);
            sBal[slot].setTarget    (balance);
        }

        void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
        {
            const int n = std::min (numSamples, maxBlk);
            if (n <= 0) return;

            // Advance the smoothers once for the block; both channels read the ramps.
            for (int k = 0; k < kNumSlots; ++k)
                for (int i = 0; i < n; ++i)
                {
                    bufInGain[k][(size_t) i] = sInGain[k].next();
                    bufOut[k][(size_t) i]    = sOut[k].next();
                    bufBal[k][(size_t) i]    = sBal[k].next();
                }

            processChannel (0, inL, outL, n);
            processChannel (1, inR, outR, n);
        }

        // Stereo "balance" law: b in [-1,1]. b=0 keeps both channels at unity; b<0
        // attenuates R, b>0 attenuates L.
        static float balanceGain (float b, int ch) noexcept
        {
            return ch == 0 ? std::min (1.0f, 1.0f - b) : std::min (1.0f, 1.0f + b);
        }

    private:
        struct Ramp
        {
            float cur = 0.0f, target = 0.0f, inc = 0.0f;
            int   countdown = 0, len = 1;
            void setLen (int l) noexcept { len = std::max (1, l); }
            void setTarget (float t) noexcept
            {
                if (t == target) return;
                target = t;
                inc = (target - cur) / (float) len;
                countdown = len;
            }
            void snap() noexcept { cur = target; countdown = 0; inc = 0.0f; }
            float next() noexcept
            {
                if (countdown > 0) { cur += inc; if (--countdown == 0) cur = target; }
                return cur;
            }
        };

        void processChannel (int ch, const float* in, float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i) { running[(size_t) i] = in[i]; parBus[(size_t) i] = 0.0f; }

            bool anySeries = false, anyParallel = false;
            for (int k = 0; k < kNumSlots; ++k)
            {
                if (! enabled[k] || model[k][ch] == nullptr)
                    continue;

                for (int i = 0; i < n; ++i)
                    scratch[(size_t) i] = running[(size_t) i] * bufInGain[k][(size_t) i];

                model[k][ch]->processReplacing (scratch.data(), n);

                for (int i = 0; i < n; ++i)
                    scratch[(size_t) i] *= bufOut[k][(size_t) i];

                if (mode[k] == Mode::Series)
                {
                    for (int i = 0; i < n; ++i) running[(size_t) i] = scratch[(size_t) i];
                    anySeries = true;
                }
                else
                {
                    for (int i = 0; i < n; ++i)
                        parBus[(size_t) i] += scratch[(size_t) i] * balanceGain (bufBal[k][(size_t) i], ch);
                    anyParallel = true;
                }
            }

            if (anySeries || anyParallel)
                for (int i = 0; i < n; ++i)
                    out[i] = (anySeries ? running[(size_t) i] : 0.0f) + parBus[(size_t) i];
            else
                for (int i = 0; i < n; ++i)
                    out[i] = in[i];
        }

        MonoProcessor* model[kNumSlots][kNumChannels] { { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr } };
        bool  enabled[kNumSlots] { false, false, false };
        Mode  mode[kNumSlots]    { Mode::Series, Mode::Series, Mode::Series };

        Ramp sInGain[kNumSlots], sOut[kNumSlots], sBal[kNumSlots];
        std::array<std::vector<float>, kNumSlots> bufInGain, bufOut, bufBal;
        std::vector<float> running, parBus, scratch;
        int maxBlk = 0;
    };
} // namespace factory_core
