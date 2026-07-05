//
// dsp_test.cpp — headless verification of the NAM Player DSP core. Links only
// factory_core (no JUCE, no NeuralAmpModelerCore): the NAM inference is abstracted
// behind factory_core::MonoProcessor, so the routing engine is exercised by injecting
// a KNOWN nonlinearity (tanh) and comparing against an INDEPENDENT oracle that
// recomputes the routing spec in a separate code path. The (linear) IR convolver is
// checked against an independent time-domain convolution oracle and the impulse
// identity. The resampler latency formula is checked for its invariants. Everything
// runs across the standard sample-rate matrix.
//
#include "factory_core/NamRoutingEngine.h"
#include "factory_core/FftConvolver.h"
#include "factory_core/PartitionedConvolver.h"
#include "factory_core/Resampler.h"
#include "factory_core/PolyphaseResampler.h"
#include "factory_core/ResamplerLatency.h"
#include "factory_core/RateBracket.h"
#include "factory_core/Biquad.h"
#include "factory_core/Filters.h"
#include "factory_core/testing/DspInvariants.h"

#include "../Source/OfflineReamp.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace
{
    using Mode = factory_core::NamRoutingEngine::Mode;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }
    void check (bool ok, const std::string& m) { if (! ok) fail (m); }

    constexpr double kPi = 3.14159265358979323846;

    // Injected known nonlinearity: x -> tanh(g*x). Memoryless, so per-sample and
    // per-block processing agree — which lets the oracle stay per-sample.
    struct TanhModel : factory_core::MonoProcessor
    {
        float g = 1.0f;
        TanhModel() = default;
        explicit TanhModel (float gain) : g (gain) {}
        void processReplacing (float* b, int n) noexcept override
        {
            for (int i = 0; i < n; ++i) b[i] = std::tanh (g * b[i]);
        }
    };

    // Stateful model injected only for the reset() test: a one-pole leaky
    // integrator (y[n] = decay*y[n-1] + x[n]) so the engine's reset() has real
    // per-model memory to clear (TanhModel above is memoryless, so it can't
    // exercise reset()'s "propagate to model.reset()" contract, class E in
    // docs/regression-policy.md).
    struct LeakyModel : factory_core::MonoProcessor
    {
        float decay = 0.9f;
        float state = 0.0f;
        void processReplacing (float* b, int n) noexcept override
        {
            for (int i = 0; i < n; ++i) { state = decay * state + b[i]; b[i] = state; }
        }
        void reset() noexcept override { state = 0.0f; }
    };

    // Identity model injected only for the ramp-trajectory test: leaves the
    // signal unchanged, so the engine's per-sample output is exactly
    // in[i] * bufInGain[i] * bufOut[i] with no nonlinearity in the way of the
    // linear-ramp oracle.
    struct WireModel : factory_core::MonoProcessor
    {
        void processReplacing (float*, int) noexcept override {}
    };

    // Independent restatement of the ~5 ms ramp length documented in
    // NamRoutingEngine.h (NamRoutingEngine::prepare): the per-slot gain/balance
    // smoothers run over this many samples. Used as the oracle's ramp duration,
    // not read out of the engine at runtime.
    int rampLenSamples (double fs) { return std::max (1, (int) std::lround (0.005 * fs)); }

    struct SlotCfg { bool en; Mode mode; float inGain; float out; float bal; float tanhG; bool loaded; };

    // Independent balance law (separate code path from the engine).
    float oracleBalance (float b, int ch) noexcept
    {
        return ch == 0 ? std::min (1.0f, 1.0f - b) : std::min (1.0f, 1.0f + b);
    }

    // Independent oracle: recompute the routing spec from scratch, per sample.
    void routeOracle (const std::array<SlotCfg, 3>& s,
                      const std::vector<float>& inL, const std::vector<float>& inR,
                      std::vector<float>& outL, std::vector<float>& outR)
    {
        const int n = (int) inL.size();
        outL.assign ((size_t) n, 0.0f);
        outR.assign ((size_t) n, 0.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& in = ch == 0 ? inL : inR;
            auto&       out = ch == 0 ? outL : outR;
            for (int i = 0; i < n; ++i)
            {
                float running = in[(size_t) i];
                float par = 0.0f;
                bool anyS = false, anyP = false;
                for (int k = 0; k < 3; ++k)
                {
                    if (! s[k].en || ! s[k].loaded) continue;
                    float x = running * s[k].inGain;
                    x = std::tanh (s[k].tanhG * x);
                    x *= s[k].out;
                    if (s[k].mode == Mode::Series) { running = x; anyS = true; }
                    else                           { par += x * oracleBalance (s[k].bal, ch); anyP = true; }
                }
                out[(size_t) i] = (anyS || anyP) ? ((anyS ? running : 0.0f) + par) : in[(size_t) i];
            }
        }
    }

    double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
    {
        double m = 0.0;
        for (size_t i = 0; i < a.size(); ++i) m = std::max (m, (double) std::abs (a[i] - b[i]));
        return m;
    }

    void configEngine (factory_core::NamRoutingEngine& eng, const std::array<SlotCfg, 3>& s,
                       std::array<TanhModel, 3>& models, double fs, int block)
    {
        eng.prepare (fs, block);
        for (int k = 0; k < 3; ++k)
        {
            models[(size_t) k].g = s[k].tanhG;
            factory_core::MonoProcessor* m = s[k].loaded ? &models[(size_t) k] : nullptr;
            eng.setModel (k, 0, m);
            eng.setModel (k, 1, m);
            eng.setSlot (k, s[k].en, s[k].mode, s[k].inGain, s[k].out, s[k].bal);
        }
        eng.snap();  // settle gains so the oracle can use targets directly (exact match)
    }

    std::vector<float> randomSignal (std::mt19937& rng, int n, float amp)
    {
        std::uniform_real_distribution<float> d (-amp, amp);
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i) v[(size_t) i] = d (rng);
        return v;
    }

    // ---- Routing engine: oracle match + invariants -------------------------------
    void routingTests (double fs)
    {
        std::printf ("Routing @ Fs=%.0f\n", fs);
        std::mt19937 rng (1234567u ^ (unsigned) (long long) fs);
        const int n = 200;                       // non-power-of-two on purpose

        const std::array<std::array<SlotCfg, 3>, 6> configs { {
            // all series
            { SlotCfg{ true,  Mode::Series,   1.5f, 0.8f,  0.0f, 3.0f, true },
              SlotCfg{ true,  Mode::Series,   2.0f, 0.9f,  0.0f, 2.0f, true },
              SlotCfg{ true,  Mode::Series,   1.0f, 1.0f,  0.0f, 4.0f, true } },
            // series + parallel mix, with balance
            { SlotCfg{ true,  Mode::Series,   2.0f, 1.0f,  0.0f, 3.0f, true },
              SlotCfg{ true,  Mode::Parallel, 1.2f, 0.7f, -0.5f, 5.0f, true },
              SlotCfg{ true,  Mode::Parallel, 0.9f, 0.6f,  0.7f, 2.5f, true } },
            // all parallel
            { SlotCfg{ true,  Mode::Parallel, 1.0f, 0.9f, -0.3f, 4.0f, true },
              SlotCfg{ true,  Mode::Parallel, 1.4f, 0.5f,  0.0f, 2.0f, true },
              SlotCfg{ true,  Mode::Parallel, 0.8f, 0.8f,  0.4f, 3.0f, true } },
            // some disabled / some unloaded
            { SlotCfg{ false, Mode::Series,   2.0f, 1.0f,  0.0f, 3.0f, true },
              SlotCfg{ true,  Mode::Series,   1.5f, 0.9f,  0.0f, 2.0f, false },  // enabled but unloaded => no-op
              SlotCfg{ true,  Mode::Series,   1.1f, 1.0f,  0.0f, 4.0f, true } },
            // parallel then series (order matters)
            { SlotCfg{ true,  Mode::Parallel, 1.3f, 0.8f,  0.2f, 3.5f, true },
              SlotCfg{ true,  Mode::Series,   1.0f, 1.0f,  0.0f, 2.0f, true },
              SlotCfg{ false, Mode::Series,   1.0f, 1.0f,  0.0f, 1.0f, true } },
            // single series only
            { SlotCfg{ true,  Mode::Series,   3.0f, 0.7f,  0.0f, 6.0f, true },
              SlotCfg{ false, Mode::Series,   1.0f, 1.0f,  0.0f, 1.0f, true },
              SlotCfg{ false, Mode::Series,   1.0f, 1.0f,  0.0f, 1.0f, true } },
        } };

        for (size_t ci = 0; ci < configs.size(); ++ci)
        {
            const auto inL = randomSignal (rng, n, 0.8f);
            const auto inR = randomSignal (rng, n, 0.8f);

            factory_core::NamRoutingEngine eng;
            std::array<TanhModel, 3> models;
            configEngine (eng, configs[ci], models, fs, n);

            std::vector<float> outL ((size_t) n), outR ((size_t) n);
            eng.process (inL.data(), inR.data(), outL.data(), outR.data(), n);

            std::vector<float> refL, refR;
            routeOracle (configs[ci], inL, inR, refL, refR);

            const double dl = maxAbsDiff (outL, refL);
            const double dr = maxAbsDiff (outR, refR);
            if (dl > 2e-5 || dr > 2e-5)
                fail ("config " + std::to_string (ci) + ": engine vs oracle diff L=" + std::to_string (dl)
                      + " R=" + std::to_string (dr));
        }

        // Invariant: no active slot => exact passthrough (all disabled).
        {
            std::array<SlotCfg, 3> s { {
                { false, Mode::Series, 2.0f, 1.0f, 0.0f, 3.0f, true },
                { false, Mode::Series, 2.0f, 1.0f, 0.0f, 3.0f, true },
                { false, Mode::Series, 2.0f, 1.0f, 0.0f, 3.0f, true } } };
            const auto inL = randomSignal (rng, n, 0.8f);
            const auto inR = randomSignal (rng, n, 0.8f);
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, n);
            std::vector<float> outL ((size_t) n), outR ((size_t) n);
            eng.process (inL.data(), inR.data(), outL.data(), outR.data(), n);
            check (maxAbsDiff (outL, inL) == 0.0 && maxAbsDiff (outR, inR) == 0.0,
                   "all-disabled not bit-exact passthrough");
        }

        // Invariant: enabled but unloaded (null model) is a no-op => passthrough.
        {
            std::array<SlotCfg, 3> s { {
                { true,  Mode::Series, 2.0f, 1.0f, 0.0f, 3.0f, false },
                { false, Mode::Series, 1.0f, 1.0f, 0.0f, 1.0f, true },
                { false, Mode::Series, 1.0f, 1.0f, 0.0f, 1.0f, true } } };
            const auto inL = randomSignal (rng, n, 0.8f);
            const auto inR = randomSignal (rng, n, 0.8f);
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, n);
            std::vector<float> outL ((size_t) n), outR ((size_t) n);
            eng.process (inL.data(), inR.data(), outL.data(), outR.data(), n);
            check (maxAbsDiff (outL, inL) == 0.0, "unloaded slot not a no-op");
        }

        // Invariant: a lone Parallel slot with out=0 => silence (no dry-center leak),
        // distinguishing the parallel path from empty passthrough.
        {
            std::array<SlotCfg, 3> s { {
                { true,  Mode::Parallel, 1.0f, 0.0f, 0.0f, 3.0f, true },
                { false, Mode::Series,   1.0f, 1.0f, 0.0f, 1.0f, true },
                { false, Mode::Series,   1.0f, 1.0f, 0.0f, 1.0f, true } } };
            const auto inL = randomSignal (rng, n, 0.8f);
            const auto inR = randomSignal (rng, n, 0.8f);
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, n);
            std::vector<float> outL ((size_t) n), outR ((size_t) n);
            eng.process (inL.data(), inR.data(), outL.data(), outR.data(), n);
            double p = 0.0; for (float v : outL) p = std::max (p, (double) std::abs (v));
            check (p == 0.0, "parallel out=0 leaked dry signal");
        }

        // Invariant: slot order matters for a nonlinear series chain.
        {
            std::array<SlotCfg, 3> a { {
                { true, Mode::Series, 2.0f, 0.9f, 0.0f, 3.0f, true },
                { true, Mode::Series, 5.0f, 0.8f, 0.0f, 6.0f, true },
                { false, Mode::Series, 1.0f, 1.0f, 0.0f, 1.0f, true } } };
            std::array<SlotCfg, 3> b = a;
            std::swap (b[0], b[1]);
            const auto inL = randomSignal (rng, n, 0.8f);
            const auto inR = randomSignal (rng, n, 0.8f);
            factory_core::NamRoutingEngine e1, e2; std::array<TanhModel, 3> m1, m2;
            configEngine (e1, a, m1, fs, n);
            configEngine (e2, b, m2, fs, n);
            std::vector<float> o1 ((size_t) n), o1r ((size_t) n), o2 ((size_t) n), o2r ((size_t) n);
            e1.process (inL.data(), inR.data(), o1.data(),  o1r.data(), n);
            e2.process (inL.data(), inR.data(), o2.data(),  o2r.data(), n);
            check (maxAbsDiff (o1, o2) > 1e-3, "slot order had no effect on a nonlinear chain");
        }

        // Stability/finiteness with the smoothers ACTIVE (no snap): loud input, long
        // hold, aggressive gains. Output must stay finite and realistically bounded.
        {
            std::array<SlotCfg, 3> s { {
                { true, Mode::Series,   4.0f, 2.0f, 0.0f, 8.0f, true },
                { true, Mode::Parallel, 3.0f, 2.0f, -0.6f, 6.0f, true },
                { true, Mode::Series,   2.0f, 2.0f, 0.0f, 5.0f, true } } };
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            eng.prepare (fs, 256);
            for (int k = 0; k < 3; ++k)
            {
                models[(size_t) k].g = s[k].tanhG;
                eng.setModel (k, 0, &models[(size_t) k]);
                eng.setModel (k, 1, &models[(size_t) k]);
                eng.setSlot (k, s[k].en, s[k].mode, s[k].inGain, s[k].out, s[k].bal);
            }
            // NOTE: deliberately no snap() => ramps are active.
            std::vector<double> collected;
            std::vector<float> outL (256), outR (256);
            for (int blk = 0; blk < 400; ++blk)
            {
                const auto inL = randomSignal (rng, 256, 2.0f);
                const auto inR = randomSignal (rng, 256, 2.0f);
                eng.process (inL.data(), inR.data(), outL.data(), outR.data(), 256);
                for (int i = 0; i < 256; ++i) { collected.push_back (outL[(size_t) i]); collected.push_back (outR[(size_t) i]); }
            }
            check (factory_core::testing::allFinite (collected), "routing produced NaN/Inf under load");
            // out gains are 2.0; series ends at |tanh|*2 <= 2, parallel sums 3 taps each
            // <= 2 => a comfortable ceiling well under 12.
            check (factory_core::testing::peakAbs (collected) < 12.0, "routing exceeded a realistic peak bound");
        }
    }

    // ---- NamRoutingEngine::reset(): buses cleared, models reset, ramps settled ---
    //
    // reset() (NamRoutingEngine.h ~89-98) fills running/parBus with 0, propagates
    // to every loaded model's reset(), then calls snap() to settle all gain/
    // balance ramps onto their current targets. None of that is directly
    // observable from outside the engine except by its effect on subsequent
    // output, so the oracle here is behavioural: an engine that has accumulated
    // real held state (nonzero model memory, a still-in-flight ramp) and is then
    // reset() must produce EXACTLY the same output, sample-for-sample, on a
    // subsequent input as a freshly-constructed engine configured identically
    // and explicitly snap()ped. This is the class-E regression gate (#30/#39/#41)
    // applied to the routing engine itself, which previously had no test at all.
    void resetTests (double fs)
    {
        std::printf ("Engine reset() @ Fs=%.0f\n", fs);
        std::mt19937 rng (0xA55E7u ^ (unsigned) (long long) fs);
        const int maxBlock = 4096;
        const int rl = rampLenSamples (fs);

        auto buildEngine = [&] (factory_core::NamRoutingEngine& eng, std::array<LeakyModel, 3>& models)
        {
            eng.prepare (fs, maxBlock);
            for (int k = 0; k < 3; ++k)
            {
                eng.setModel (k, 0, &models[(size_t) k]);
                eng.setModel (k, 1, &models[(size_t) k]);
            }
            eng.setSlot (0, true,  Mode::Series,   1.4f, 0.85f,  0.0f);
            eng.setSlot (1, true,  Mode::Parallel, 0.9f, 0.6f,  -0.4f);
            eng.setSlot (2, false, Mode::Series,   1.0f, 1.0f,   0.0f);
        };

        // Engine A: build, settle to the initial targets, THEN retarget slots 0/1
        // (creating still-active, in-flight ramps -- no snap()) and stream a
        // priming signal so the leaky models accumulate real, nonzero memory.
        // Prime with FEWER samples than the ramp length so the ramps are
        // genuinely mid-flight at the moment of reset() (not already converged
        // by coincidence).
        factory_core::NamRoutingEngine engA;
        std::array<LeakyModel, 3> modelsA;
        buildEngine (engA, modelsA);
        engA.snap();
        engA.setSlot (0, true, Mode::Series,   2.2f, 0.5f, 0.0f);
        engA.setSlot (1, true, Mode::Parallel, 1.6f, 0.4f, 0.5f);

        const int primeN = std::max (1, rl / 2);
        {
            const auto inL = randomSignal (rng, primeN, 0.8f);
            const auto inR = randomSignal (rng, primeN, 0.8f);
            std::vector<float> outL ((size_t) primeN), outR ((size_t) primeN);
            engA.process (inL.data(), inR.data(), outL.data(), outR.data(), primeN);
        }
        check (modelsA[0].state != 0.0f,
               "reset test setup: series model never accumulated state (test would be vacuous)");

        engA.reset();
        check (modelsA[0].state == 0.0f && modelsA[1].state == 0.0f,
               "engine reset() did not propagate to model.reset()");

        // Engine B: fresh construction, configured to exactly the targets engine
        // A held at reset time, then explicitly snap()ped -- the documented
        // post-reset contract.
        factory_core::NamRoutingEngine engB;
        std::array<LeakyModel, 3> modelsB;
        buildEngine (engB, modelsB);
        engB.setSlot (0, true, Mode::Series,   2.2f, 0.5f, 0.0f);
        engB.setSlot (1, true, Mode::Parallel, 1.6f, 0.4f, 0.5f);
        engB.snap();

        // Drive the SAME subsequent input through both and compare.
        const int n = 1000;
        const auto inL = randomSignal (rng, n, 0.7f);
        const auto inR = randomSignal (rng, n, 0.7f);
        std::vector<float> outAL ((size_t) n), outAR ((size_t) n);
        std::vector<float> outBL ((size_t) n), outBR ((size_t) n);
        engA.process (inL.data(), inR.data(), outAL.data(), outAR.data(), n);
        engB.process (inL.data(), inR.data(), outBL.data(), outBR.data(), n);

        const double dl = maxAbsDiff (outAL, outBL);
        const double dr = maxAbsDiff (outAR, outBR);
        check (dl < 1e-6 && dr < 1e-6,
               "reset() did not converge to a freshly-configured engine (buses/ramps/model state), diff L="
               + std::to_string (dl) + " R=" + std::to_string (dr));
    }

    // ---- Ramp trajectory: linear-interpolation oracle over the documented ramp --
    //
    // The pre-existing "ramp active" test only asserted finite + peak<12 (no
    // oracle on the actual trajectory). Here: a steady sine through a Series
    // slot with an identity (Wire) model, so output[i] == input[i] * inGain(i)
    // exactly, with inGain(i) driven by a single retarget (no snap()). The
    // independent oracle re-derives Ramp::next()'s documented linear ramp
    // (NamRoutingEngine.h ~156-160: cur += inc each sample, exactly hitting
    // target at sample rl-1, held after) using rampLenSamples() (the ~5 ms
    // constant from the header, not read out of the engine). Both an instant
    // jump (no ramp) and a wrong-duration ramp diverge from this oracle and
    // must fail.
    void rampTrajectoryTests (double fs)
    {
        std::printf ("Ramp trajectory @ Fs=%.0f\n", fs);
        const int rl = rampLenSamples (fs);
        const int n  = rl + 2000;                 // full ramp + a settled tail, one block

        const float oldGain = 0.2f;
        const float newGain = 1.6f;
        const float A = 0.5f;
        const double f = 500.0;

        factory_core::NamRoutingEngine eng;
        WireModel model;
        eng.prepare (fs, n);
        eng.setModel (0, 0, &model);
        eng.setModel (0, 1, &model);
        eng.setSlot (0, true,  Mode::Series, oldGain, 1.0f, 0.0f);
        eng.setSlot (1, false, Mode::Series, 1.0f,    1.0f, 0.0f);
        eng.setSlot (2, false, Mode::Series, 1.0f,    1.0f, 0.0f);
        eng.snap();

        // Retarget WITHOUT snap() -> the engine must ramp inGain from oldGain to
        // newGain over exactly rl samples.
        eng.setSlot (0, true, Mode::Series, newGain, 1.0f, 0.0f);

        std::vector<float> inL ((size_t) n), inR ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float s = A * (float) std::sin (2.0 * kPi * f * i / fs);
            inL[(size_t) i] = s; inR[(size_t) i] = s;
        }
        std::vector<float> outL ((size_t) n), outR ((size_t) n);
        eng.process (inL.data(), inR.data(), outL.data(), outR.data(), n);

        // Independent oracle: Ramp::next() returns cur_0 + (i+1)*inc for
        // i = 0..rl-2, and exactly `target` at i == rl-1 and after.
        double maxErr = 0.0, maxErrPostRamp = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double gain = (i < rl) ? (oldGain + (double) (i + 1) / rl * (newGain - oldGain)) : newGain;
            const double expected = gain * inL[(size_t) i];
            const double err = std::abs ((double) outL[(size_t) i] - expected);
            maxErr = std::max (maxErr, err);
            if (i >= rl) maxErrPostRamp = std::max (maxErrPostRamp, err);
        }
        check (maxErr < 2e-5, "ramp trajectory diverged from the linear-interpolation oracle, maxErr="
               + std::to_string (maxErr));
        check (maxErrPostRamp < 2e-5,
               "ramp did not settle to newGain by the documented rate-dependent sample count rl="
               + std::to_string (rl));

        // Explicit "no-ramp / wrong-duration must fail" gate: near the temporal
        // midpoint of the ramp (picking a sample with enough signal amplitude to
        // avoid a near-zero sine crossing), the output must sit meaningfully
        // between the "already at newGain" (instant jump) and "still at oldGain"
        // (too-slow / wrong-duration ramp) hypotheses, matching only the linear
        // oracle.
        {
            int idx = rl / 2;
            while (idx < n - 1 && std::abs (inL[(size_t) idx]) < 0.3f * A) ++idx;
            const double instantJumpOut = (double) newGain * inL[(size_t) idx];
            const double staticOut      = (double) oldGain * inL[(size_t) idx];
            const double span = std::abs (instantJumpOut - staticOut);
            check (std::abs ((double) outL[(size_t) idx] - instantJumpOut) > 0.2 * span,
                   "ramp jumped to newGain instantly at rl/2 (no ramp)");
            check (std::abs ((double) outL[(size_t) idx] - staticOut) > 0.2 * span,
                   "ramp still at oldGain at rl/2 (too slow / wrong duration)");
        }
    }

    // ---- FFT convolver: impulse identity + independent time-domain oracle --------
    std::vector<float> naiveConv (const std::vector<float>& x, const std::vector<float>& h, int outLen)
    {
        std::vector<float> y ((size_t) outLen, 0.0f);
        for (int m = 0; m < outLen; ++m)
        {
            double acc = 0.0;
            const int jmax = std::min ((int) h.size() - 1, m);
            for (int j = 0; j <= jmax; ++j)
                acc += (double) h[(size_t) j] * (double) x[(size_t) (m - j)];
            y[(size_t) m] = (float) acc;
        }
        return y;
    }

    void convolutionTests()
    {
        std::printf ("FFT convolver\n");
        std::mt19937 rng (999u);
        const int irLen = 200;
        const auto ir = randomSignal (rng, irLen, 1.0f);

        // Impulse identity + zero latency: out[0..irLen-1] == ir, rest ~0.
        {
            factory_core::FftConvolver conv;
            conv.prepare (/*maxBlock*/ 64, /*maxIrLen*/ 256);
            std::vector<std::complex<double>> H;
            conv.buildKernel (ir.data(), irLen, H);
            const int total = irLen + 300;
            std::vector<float> stream ((size_t) total, 0.0f);
            stream[0] = 1.0f;
            const int B = 64;
            for (int p = 0; p < total; p += B)
            {
                const int m = std::min (B, total - p);
                conv.process (stream.data() + p, m, H);
            }
            double maxErr = 0.0;
            for (int i = 0; i < irLen; ++i)
                maxErr = std::max (maxErr, (double) std::abs (stream[(size_t) i] - ir[(size_t) i]));
            double tailPeak = 0.0;
            for (int i = irLen; i < total; ++i) tailPeak = std::max (tailPeak, (double) std::abs (stream[(size_t) i]));
            check (maxErr < 1e-4, "convolver impulse response != IR (zero-latency identity), err=" + std::to_string (maxErr));
            check (tailPeak < 1e-4, "convolver produced energy past the IR length");
        }

        // Random signal vs independent naive convolution, across several block sizes.
        for (int B : { 1, 7, 64 })
        {
            factory_core::FftConvolver conv;
            conv.prepare (/*maxBlock*/ 64, /*maxIrLen*/ 256);
            std::vector<std::complex<double>> H;
            conv.buildKernel (ir.data(), irLen, H);
            const int total = 2000;
            const auto input = randomSignal (rng, total, 0.7f);
            std::vector<float> stream = input;
            for (int p = 0; p < total; p += B)
            {
                const int m = std::min (B, total - p);
                conv.process (stream.data() + p, m, H);
            }
            const auto ref = naiveConv (input, ir, total);
            double err = 0.0, ref_peak = 0.0;
            for (int i = 0; i < total; ++i)
            {
                err = std::max (err, (double) std::abs (stream[(size_t) i] - ref[(size_t) i]));
                ref_peak = std::max (ref_peak, (double) std::abs (ref[(size_t) i]));
            }
            check (err < 1e-3 * std::max (1.0, ref_peak),
                   "convolver vs naive conv mismatch (B=" + std::to_string (B) + "), err=" + std::to_string (err));
        }

        // Empty kernel => passthrough.
        {
            factory_core::FftConvolver conv;
            conv.prepare (64, 256);
            const std::vector<std::complex<double>> emptyH;
            const auto input = randomSignal (rng, 300, 0.5f);
            std::vector<float> stream = input;
            for (int p = 0; p < 300; p += 64) conv.process (stream.data() + p, std::min (64, 300 - p), emptyH);
            check (maxAbsDiff (stream, input) == 0.0, "convolver with empty kernel is not passthrough");
        }
    }

    // ---- Partitioned convolver: zero-latency identity + independent naive oracle -
    //
    // The plugin's cab IR now uses the zero-latency uniform-partitioned convolver
    // (it caps the FFT size at high rates). Same independent time-domain oracle as
    // the single-partition convolver, exercised across block sizes and IR lengths
    // spanning many partitions, plus a rate-invariance check (a time-specified IR
    // has the same effective length in seconds at every sample rate).
    void partitionedConvolverTests()
    {
        std::printf ("Partitioned convolver\n");
        std::mt19937 rng (20260702u);

        for (int maxBlock : { 64, 128, 256, 512 })
            for (int irLen : { 50, 200, 1000, 5000 })
            {
                const auto ir = randomSignal (rng, irLen, 1.0f);

                // Impulse identity + zero latency + no energy past the IR length.
                {
                    factory_core::PartitionedConvolver conv; conv.prepare (maxBlock, 6000);
                    factory_core::PartitionedConvolver::Kernel K; conv.buildKernel (ir.data(), irLen, K);
                    const int total = irLen + 300;
                    std::vector<float> stream ((size_t) total, 0.0f); stream[0] = 1.0f;
                    for (int p = 0; p < total; p += maxBlock)
                        conv.process (stream.data() + p, std::min (maxBlock, total - p), K);
                    double maxErr = 0.0, tail = 0.0;
                    for (int i = 0; i < irLen; ++i)     maxErr = std::max (maxErr, (double) std::abs (stream[(size_t) i] - ir[(size_t) i]));
                    for (int i = irLen; i < total; ++i) tail   = std::max (tail,   (double) std::abs (stream[(size_t) i]));
                    check (maxErr < 1e-4, "partitioned impulse != IR (mB=" + std::to_string (maxBlock)
                           + " L=" + std::to_string (irLen) + "), err=" + std::to_string (maxErr));
                    check (tail < 1e-4, "partitioned energy past IR length (mB=" + std::to_string (maxBlock)
                           + " L=" + std::to_string (irLen) + ")");
                }

                // Random signal vs independent naive convolution across block sizes.
                for (int B : { 1, 7, 64, 512 })
                {
                    factory_core::PartitionedConvolver conv; conv.prepare (maxBlock, 6000);
                    factory_core::PartitionedConvolver::Kernel K; conv.buildKernel (ir.data(), irLen, K);
                    const int total = 3000;
                    const auto input = randomSignal (rng, total, 0.7f);
                    std::vector<float> stream = input;
                    for (int p = 0; p < total; p += B)
                        conv.process (stream.data() + p, std::min (B, total - p), K);
                    const auto ref = naiveConv (input, ir, total);
                    double err = 0.0, pk = 0.0;
                    for (int i = 0; i < total; ++i)
                    { err = std::max (err, (double) std::abs (stream[(size_t) i] - ref[(size_t) i])); pk = std::max (pk, (double) std::abs (ref[(size_t) i])); }
                    check (err < 1e-3 * std::max (1.0, pk), "partitioned vs naive (mB=" + std::to_string (maxBlock)
                           + " L=" + std::to_string (irLen) + " B=" + std::to_string (B) + "), err=" + std::to_string (err));
                }
            }

        // Empty kernel => passthrough.
        {
            factory_core::PartitionedConvolver conv; conv.prepare (128, 6000);
            factory_core::PartitionedConvolver::Kernel K;                 // empty head
            const auto input = randomSignal (rng, 300, 0.5f);
            std::vector<float> stream = input;
            for (int p = 0; p < 300; p += 128) conv.process (stream.data() + p, std::min (128, 300 - p), K);
            check (maxAbsDiff (stream, input) == 0.0, "partitioned empty kernel not passthrough");
        }

        // Rate invariance: an IR specified as a DURATION (0.05 s) has the same effective
        // length in seconds at every sample rate — its response is contained within
        // round(0.05*fs) samples and silent after (the time-based IR-cap semantics).
        for (double fs : factory_core::testing::kStandardSampleRates())
        {
            const int L = (int) std::lround (0.05 * fs);
            const auto ir = randomSignal (rng, L, 1.0f);
            factory_core::PartitionedConvolver conv; conv.prepare (256, (int) std::lround (0.17 * fs));
            factory_core::PartitionedConvolver::Kernel K; conv.buildKernel (ir.data(), L, K);
            const int total = L + 500;
            std::vector<float> stream ((size_t) total, 0.0f); stream[0] = 1.0f;
            for (int p = 0; p < total; p += 256) conv.process (stream.data() + p, std::min (256, total - p), K);
            double tail = 0.0;
            for (int i = L; i < total; ++i) tail = std::max (tail, (double) std::abs (stream[(size_t) i]));
            check (tail < 1e-4, "partitioned IR duration not rate-invariant at Fs=" + std::to_string ((int) fs));
        }
    }

    // ---- Streaming resampler: exact 1:1 delay + amplitude/rate preservation ------
    void resamplerTests()
    {
        std::printf ("Resampler\n");
        constexpr double pi = 3.14159265358979323846;
        std::mt19937 rng (4242u);

        // Ratio 1:1 is an exact 2-sample delay (Catmull-Rom at frac=0 returns s1).
        {
            factory_core::Resampler rs; rs.prepare (48000.0, 48000.0);
            const int N = 1000;
            const auto in = randomSignal (rng, N, 0.8f);
            std::vector<float> out ((size_t) (N + 8), 0.0f);
            const int m = rs.process (in.data(), N, out.data(), (int) out.size());
            double err = 0.0; int cmp = 0;
            for (int k = 2; k < m && k < N; ++k) { err = std::max (err, (double) std::abs (out[(size_t) k] - in[(size_t) (k - 2)])); ++cmp; }
            check (cmp > 0 && err < 1e-6, "resampler 1:1 is not a clean 2-sample delay, err=" + std::to_string (err));
        }

        // A low-frequency sine (well below both Nyquists) keeps its amplitude and maps
        // to the expected number of output samples. Expected RMS of a 0.5-amp sine is
        // 0.5/sqrt(2) ~= 0.35355 (an analytic, implementation-independent oracle).
        const double pairs[][2] = { { 48000.0, 96000.0 }, { 48000.0, 44100.0 },
                                    { 96000.0, 48000.0 }, { 44100.0, 48000.0 } };
        for (const auto& pr : pairs)
        {
            const double a = pr[0], b = pr[1];
            factory_core::Resampler rs; rs.prepare (a, b);
            const int N = 8000;
            std::vector<float> in ((size_t) N);
            for (int i = 0; i < N; ++i) in[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * 300.0 * i / a);
            const int outCap = (int) (N * b / a) + 16;
            std::vector<float> out ((size_t) outCap, 0.0f);
            const int m = rs.process (in.data(), N, out.data(), outCap);

            const double expectedM = N * b / a;
            check (std::abs (m - expectedM) < 4.0, "resampler output count off (a=" + std::to_string (a)
                   + " b=" + std::to_string (b) + "): " + std::to_string (m) + " vs " + std::to_string (expectedM));

            bool finite = true; double sumsq = 0.0; int c = 0;
            for (int i = 100; i < m - 100; ++i) { const float v = out[(size_t) i]; if (! std::isfinite (v)) finite = false; sumsq += (double) v * v; ++c; }
            check (finite, "resampler produced NaN/Inf");
            const double rms = c > 0 ? std::sqrt (sumsq / c) : 0.0;
            check (std::abs (rms - 0.35355) < 0.02, "resampler amplitude not preserved (a=" + std::to_string (a)
                   + " b=" + std::to_string (b) + "), rms=" + std::to_string (rms));
        }
    }

    // ---- Resampler round-trip latency invariants ---------------------------------
    void latencyTests()
    {
        std::printf ("Resampler latency\n");
        using factory_core::resamplerRoundTripLatency;
        check (resamplerRoundTripLatency (48000.0, 48000.0, 2) == 0, "latency at model rate != 0");
        check (resamplerRoundTripLatency (44100.0, 48000.0, 0) == 0, "latency with baseLatency 0 != 0");

        const double rates[] = { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        int prev = -1;
        for (double r : rates)
        {
            const int l = resamplerRoundTripLatency (r, 48000.0, 2);
            check (l > 0, "latency at " + std::to_string (r) + " not positive");
            check (l >= prev, "latency not non-decreasing at " + std::to_string (r));
            prev = l;
        }
    }

    // ---- PolyphaseResampler: band-limiting (alias/image) suppression -------------
    //
    // Independent spectral oracle: a windowed single-bin DFT (a separate code path
    // from the resampler) measures the amplitude of a sinusoid at a target frequency.
    // The Catmull-Rom Resampler folds near-Nyquist tones back at ~0 dB; the
    // band-limited PolyphaseResampler must keep every alias/image bin <= -70 dB
    // (designed for 80 dB). Passband tones must be preserved (±0.5 dB).

    double toneAmplitude (const std::vector<float>& x, int start, int len, double f, double fs)
    {
        double re = 0.0, im = 0.0, wsum = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double w  = 0.5 - 0.5 * std::cos (2.0 * kPi * i / (len - 1));   // Hann
            const double ph = 2.0 * kPi * f * (start + i) / fs;
            re += w * x[(size_t) (start + i)] * std::cos (ph);
            im -= w * x[(size_t) (start + i)] * std::sin (ph);
            wsum += w;
        }
        return 2.0 * std::sqrt (re * re + im * im) / wsum;                        // sinusoid amplitude
    }
    double toneDb (const std::vector<float>& x, int start, int len, double f, double fs)
    {
        return 20.0 * std::log10 (toneAmplitude (x, start, len, f, fs) + 1e-20);
    }

    std::vector<float> polyResample (double inR, double outR, const std::vector<float>& in)
    {
        factory_core::PolyphaseResampler rs; rs.prepare (inR, outR);
        const int cap = (int) (in.size() * outR / inR) + 64;
        std::vector<float> out ((size_t) std::max (1, cap), 0.0f);
        const int m = rs.process (in.data(), (int) in.size(), out.data(), cap);
        out.resize ((size_t) std::max (0, m));
        return out;
    }

    std::vector<float> sineAt (double fs, double f, int n, float amp = 1.0f)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2.0 * kPi * f * i / fs);
        return v;
    }

    // Frequency a tone `f` (above outNyq) folds to when sampled at outRate.
    double foldFreq (double f, double outRate)
    {
        const double k = std::round (f / outRate);
        return std::abs (f - k * outRate);
    }

    void polyphaseResamplerTests()
    {
        std::printf ("PolyphaseResampler alias/image suppression\n");
        const double gate = -70.0;

        // 1:1 group delay = D input samples, and a low-frequency tone passes at unity.
        {
            factory_core::PolyphaseResampler rs; rs.prepare (48000.0, 48000.0);
            const int D = rs.groupDelayInputSamples();
            const int N = 400; std::vector<float> in ((size_t) N, 0.0f); in[50] = 1.0f;
            std::vector<float> out ((size_t) (N + 8), 0.0f);
            const int m = rs.process (in.data(), N, out.data(), (int) out.size());
            int peak = 0; double pv = 0.0;
            for (int i = 0; i < m; ++i) if (std::abs (out[(size_t) i]) > pv) { pv = std::abs (out[(size_t) i]); peak = i; }
            check (peak == 50 + D, "PolyphaseResampler 1:1 impulse peak at " + std::to_string (peak)
                   + " != 50+D=" + std::to_string (50 + D));
        }

        // Aliasing on decimation host->48k: tones past the stop-band edge must fold
        // back below the gate (0.1.0 folded these at ~0 dB).
        struct DownCase { double in, tone; };
        for (const DownCase& c : std::vector<DownCase> {
                 { 88200.0, 30000.0 }, { 88200.0, 40000.0 }, { 88200.0, 0.9 * 44100.0 },
                 { 96000.0, 30000.0 }, { 96000.0, 40000.0 }, { 96000.0, 0.9 * 48000.0 },
                 { 176400.0, 30000.0 }, { 176400.0, 40000.0 }, { 176400.0, 0.9 * 88200.0 },
                 { 192000.0, 30000.0 }, { 192000.0, 40000.0 }, { 192000.0, 0.9 * 96000.0 } })
        {
            const auto in  = sineAt (c.in, c.tone, (int) (c.in * 0.2));
            const auto out = polyResample (c.in, 48000.0, in);
            const double aliasHz = foldFreq (c.tone, 48000.0);
            const double db = toneDb (out, (int) out.size() / 4, (int) out.size() / 2, aliasHz, 48000.0);
            check (db <= gate, "alias " + std::to_string ((int) c.in) + "->48k tone "
                   + std::to_string ((int) c.tone) + " folds to " + std::to_string ((int) aliasHz)
                   + " at " + std::to_string (db) + " dB (> " + std::to_string (gate) + ")");
        }

        // Aliasing on the near-unity decimation 48->44.1k (0.1.0: -5.4 dB).
        {
            const auto in  = sineAt (48000.0, 23000.0, (int) (48000.0 * 0.2));
            const auto out = polyResample (48000.0, 44100.0, in);
            const double db = toneDb (out, (int) out.size() / 4, (int) out.size() / 2, 21100.0, 44100.0);
            check (db <= gate, "alias 48->44.1k 23k->21.1k at " + std::to_string (db) + " dB");
        }

        // Imaging on interpolation 48k->host (0.1.0: -11 dB): the 48k-18k image bin.
        for (double host : { 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            const auto in  = sineAt (48000.0, 18000.0, (int) (48000.0 * 0.2));
            const auto out = polyResample (48000.0, host, in);
            const double img = 48000.0 - 18000.0;   // 30 kHz image
            const double db  = toneDb (out, (int) out.size() / 4, (int) out.size() / 2, img, host);
            check (db <= gate, "image 48k->" + std::to_string ((int) host) + " 18k image@30k at "
                   + std::to_string (db) + " dB");
        }

        // Passband amplitude preservation (±0.5 dB) at 300 Hz and 0.3*min — both
        // directions of every standard pair. (0.4*min is the transition edge for the
        // 4x decimation case, which 63 taps cannot hold flat by design; 0.3*min is
        // inside the flat band for every ratio.)
        for (double host : factory_core::testing::kStandardSampleRates())
        {
            if (std::abs (host - 48000.0) < 1.0) continue;
            for (int dir = 0; dir < 2; ++dir)
            {
                const double a = dir == 0 ? host : 48000.0;
                const double b = dir == 0 ? 48000.0 : host;
                for (double f : { 300.0, 0.3 * std::min (a, b) })
                {
                    const auto in  = sineAt (a, f, (int) (a * 0.25), 0.5f);
                    const auto out = polyResample (a, b, in);
                    const double db = toneDb (out, (int) out.size() / 4, (int) out.size() / 2, f, b)
                                    - 20.0 * std::log10 (0.5);
                    check (std::abs (db) <= 0.5, "passband " + std::to_string ((int) a) + "->"
                           + std::to_string ((int) b) + " @" + std::to_string ((int) f) + "Hz off by "
                           + std::to_string (db) + " dB");
                }
            }
        }
    }

    // ---- OfflineReamp: reamp-pair render matches independent oracles --------------
    //
    // The MERGE export renders the wet chain offline. OfflineReamp composes the real
    // factory_core blocks, so it is verified against INDEPENDENT oracles: the routing
    // matches routeOracle (mono, Balance centred), the IR path matches an independent
    // naive convolution, and the tone is applied in the right place. Reamp is always
    // 48 kHz (the trainer-calibration rate), so this runs once.
    void offlineReampTests()
    {
        std::printf ("OfflineReamp\n");
        const double fs = 48000.0;
        const int chunk = 512, N = 4000;
        std::mt19937 rng (0x0FF11Eu);
        const auto input = randomSignal (rng, N, 0.6f);

        const std::array<SlotCfg, 3> s { {
            { true, Mode::Series,   1.5f, 0.9f, 0.0f, 3.0f, true },
            { true, Mode::Parallel, 1.2f, 0.7f, 0.0f, 4.0f, true },
            { true, Mode::Series,   1.0f, 1.0f, 0.0f, 2.0f, true } } };

        // Wiring: engine-only render == independent routeOracle mono (Balance 0).
        {
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, chunk);
            auto out = OfflineReamp::render (eng, chunk, input, false, nullptr, nullptr, 1.0f, nullptr, nullptr);
            std::vector<float> refL, refR; routeOracle (s, input, input, refL, refR);
            check ((int) out.size() == N, "reamp output length wrong");
            check (maxAbsDiff (out, refL) < 2e-5, "reamp engine wiring != oracle, diff="
                   + std::to_string (maxAbsDiff (out, refL)));
        }

        // IR path: engine output -> independent naive convolution -> IR level.
        {
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, chunk);
            const int irLen = 300; const auto ir = randomSignal (rng, irLen, 0.5f);
            factory_core::PartitionedConvolver conv; conv.prepare (chunk, 512);
            factory_core::PartitionedConvolver::Kernel K; conv.buildKernel (ir.data(), irLen, K);
            const float irLevel = 1.3f;
            auto out = OfflineReamp::render (eng, chunk, input, true, &conv, &K, irLevel, nullptr, nullptr);
            std::vector<float> refL, refR; routeOracle (s, input, input, refL, refR);
            auto conved = naiveConv (refL, ir, N);
            for (auto& v : conved) v *= irLevel;
            double d = 0.0; for (int i = 0; i < N; ++i) d = std::max (d, (double) std::abs (out[(size_t) i] - conved[(size_t) i]));
            check (d < 1e-3, "reamp IR path != engine->naiveConv->level, diff=" + std::to_string (d));
        }

        // Tone applied after the engine (Lo/Hi-Cut in the right place, offset 0).
        {
            factory_core::NamRoutingEngine eng; std::array<TanhModel, 3> models;
            configEngine (eng, s, models, fs, chunk);
            const auto lc = factory_core::designFilter (factory_core::BandType::HighPass, 120.0,  0.0, 0.70710678, fs);
            const auto hc = factory_core::designFilter (factory_core::BandType::LowPass,  6000.0, 0.0, 0.70710678, fs);
            auto out = OfflineReamp::render (eng, chunk, input, true, nullptr, nullptr, 1.0f, &lc, &hc);
            std::vector<float> refL, refR; routeOracle (s, input, input, refL, refR);
            factory_core::Biquad rlc, rhc; rlc.setCoeffs (lc); rhc.setCoeffs (hc);
            rlc.process (refL.data(), N); rhc.process (refL.data(), N);
            check (maxAbsDiff (out, refL) < 1e-4, "reamp tone not applied after engine (offset 0)");
        }
    }

    // ---- RateBracket end-to-end wet/dry alignment (the real production path) ------
    //
    // RateBracket is the exact code the plugin runs (host<->48k resampling bracket +
    // output FIFO), so testing it headless tests the production alignment, not a
    // re-implementation. With an identity section the wet output must lag the input
    // by EXACTLY latencySamples() — the integer that the dry path is delayed by —
    // with high correlation (no FIFO underrun / zero-insertion). This fails against
    // the 0.1.0 double-counted prefill (wet lagged the report by the round-trip g).
    void rateBracketTests (double fs)
    {
        std::printf ("RateBracket E2E @ Fs=%.0f\n", fs);
        std::mt19937 rng (0x0B1A5u ^ (unsigned) (long long) fs);

        for (int blk : { 64, 480, 512, 2048 })   // 480 is a deliberate non-power-of-two
        {
            factory_core::RateBracket<> br;
            br.prepare (fs, 48000.0, blk);
            const int lat = br.latencySamples();

            // Band-limited input at the host rate: white noise through a 3-pole
            // low-pass well inside the resampler passband so the identity round-trip
            // stays > 0.99 correlated, with a single sharp autocorrelation peak.
            const int total = blk * 300;
            std::vector<float> in ((size_t) total);
            {
                std::uniform_real_distribution<float> d (-1.0f, 1.0f);
                for (int i = 0; i < total; ++i) in[(size_t) i] = d (rng);
                const double fc = 0.10 * std::min (fs, 48000.0);
                const auto c = factory_core::designFilter (factory_core::BandType::LowPass, fc, 0.0, 0.70710678, fs);
                factory_core::Biquad lp[3];
                for (auto& b : lp) { b.setCoeffs (c); b.process (in.data(), total); }
                double pk = 0.0; for (float v : in) pk = std::max (pk, (double) std::abs (v));
                if (pk > 0.0) for (auto& v : in) v = (float) (v / pk * 0.7);
            }

            std::vector<float> out ((size_t) total, 0.0f);
            std::vector<float> ob0 ((size_t) blk), ob1 ((size_t) blk);
            for (int p = 0; p < total; p += blk)
            {
                const int m = std::min (blk, total - p);
                br.process (in.data() + p, in.data() + p, ob0.data(), ob1.data(), m,
                            [] (float*, float*, int) noexcept { /* identity section */ });
                for (int i = 0; i < m; ++i) out[(size_t) (p + i)] = ob0[(size_t) i];
            }

            if (fs == 48000.0)   // resampler bypassed: zero latency, bit-exact passthrough
            {
                check (lat == 0, "RateBracket latency at model rate != 0 (blk=" + std::to_string (blk) + ")");
                check (maxAbsDiff (out, in) == 0.0, "RateBracket 48k not bit-exact passthrough (blk=" + std::to_string (blk) + ")");
                continue;
            }

            // Find the integer lag maximizing normalized cross-correlation of out vs in,
            // over a steady-state window. Search a range covering every rate's latency
            // AND the buggy (round-trip-shifted) value, so the alignment fix is gated.
            const int W = std::min (8192, total / 4);
            const int s = total / 2;
            double outEnergy = 0.0;
            for (int i = 0; i < W; ++i) { const double o = out[(size_t) (s + i)]; outEnergy += o * o; }
            int    bestLag  = -1;
            double bestCorr = -1.0;
            for (int L = 0; L <= 300; ++L)
            {
                double cc = 0.0, ie = 0.0;
                for (int i = 0; i < W; ++i)
                {
                    const double o = out[(size_t) (s + i)];
                    const double x = in[(size_t) (s + i - L)];
                    cc += o * x; ie += x * x;
                }
                const double corr = cc / std::sqrt (std::max (1e-300, outEnergy * ie));
                if (corr > bestCorr) { bestCorr = corr; bestLag = L; }
            }
            // Integer-lag equality is the strong alignment gate. The correlation gate
            // is a FIFO-underrun detector (a zero-insertion collapses it well below
            // 0.9); 0.98 tolerates the sub-sample round-trip residual (<= 0.5 sample,
            // e.g. 0.48 at 44.1k) the plan permits, which only nicks the top of the band.
            check (bestLag == lat, "RateBracket wet lag " + std::to_string (bestLag)
                   + " != reported latency " + std::to_string (lat) + " (Fs=" + std::to_string ((int) fs)
                   + ", blk=" + std::to_string (blk) + ")");
            check (bestCorr > 0.98, "RateBracket wet/dry correlation too low: " + std::to_string (bestCorr)
                   + " (Fs=" + std::to_string ((int) fs) + ", blk=" + std::to_string (blk) + ")");
        }
    }
}

int main (int argc, char** argv)
{
    convolutionTests();
    partitionedConvolverTests();
    resamplerTests();
    polyphaseResamplerTests();
    latencyTests();
    offlineReampTests();

    const auto rates = factory_core::testing::sampleRatesFromArgs (argc, argv);
    for (double fs : rates)
    {
        routingTests (fs);
        resetTests (fs);
        rampTrajectoryTests (fs);
        rateBracketTests (fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
