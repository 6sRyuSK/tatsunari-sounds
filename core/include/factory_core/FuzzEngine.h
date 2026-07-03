#pragma once
//
// factory_core/FuzzEngine.h — the Fuzznari fuzz engine: one continuous circuit
// that morphs from "plug in and it's fat" (Fuzz Face / Big Muff territory) to
// gated, sputtering, self-oscillating Fuzz Factory territory without discrete
// mode switches. Header-only, JUCE-independent, allocation-free after prepare().
//
// Signal chain (per channel, at the internal model rate):
//
//   input HP (25 Hz, DC only)  →  envelope follower (2 ms / 120 ms)
//   →  dynamic bias   b_eff = b_static + gate·kGateDepth·kKnee/(env + kKnee)
//   →  nonlinear loop  u = drive·x + b_eff + k_fb·fbSig (+ noise seed)
//                      y = tanh(u) − tanh(b_eff)          ← |y| < 2, always
//      feedback path   fbSig = HP150( LP_fc( delay(y, D) ) )
//   →  tone (tilt: low shelf −g / high shelf +g @ 800 Hz)
//   →  output DC blocker (15 Hz)  →  level, mix
//
// The whole chain runs inside RateBracket<PolyphaseResampler> at a model rate
// of at least 176.4 kHz (4x below 60 kHz hosts, 2x below 120 kHz, passthrough
// at 176.4/192 kHz), so aliasing from the nonlinearity is controlled by an
// absolute internal Nyquist rather than a fixed factor — and the headless DSP
// test exercises the production antialiasing path, not a JUCE-only stand-in.
//
// Why each piece sounds like a fuzz:
//  - tanh(drive·x + b_eff) − tanh(b_eff): bias = 0 is odd-symmetric (odd
//    harmonics only); bias ≠ 0 flattens one side first (even harmonics, the
//    Fuzz Face vibe). The −tanh(b_eff) term keeps f(0) = 0 so bias moves never
//    step the output DC directly.
//  - The gate is a *bias starve*, not a level gate: with no signal the
//    operating point slides into the flat top of the tanh, so the small-signal
//    gain sech²(b_eff) collapses smoothly (sech²(3.5) ≈ −48 dB). A decaying
//    note crackles and sputters as peaks momentarily poke out of cutoff —
//    there is no if/else threshold anywhere, so no discontinuity clicks.
//  - Stab feeds the shaper output back into its input through a short delay +
//    LP + HP. Past the small-signal onset (k_fb·sech²(b_idle)·|H| > 1) the
//    loop self-oscillates; every sample written into the loop is post-tanh so
//    the oscillation is amplitude-bounded (saturating relaxation oscillator,
//    never divergent). The delay length (stab) and loop LP cutoff (gate) set
//    the squeal pitch, and gate quenches it through sech² — the Fuzz Factory
//    interplay. The in-loop 150 Hz HP structurally removes the DC fixed point
//    y* = tanh(k·y*) that exists for k > 1.
//  - The osc (Squeal) switch is the hard safety rail: when OFF the feedback
//    gain is clamped to kFbMaxSafe = 0.9, and since sech² ≤ 1, |LP| ≤ 1,
//    |HP| < 1 and the interpolated delay read ≤ 1, the linearized loop gain is
//    < 1 at every in-range setting — the repo's feedback invariant holds
//    unconditionally. ON raises the ceiling to 2.0 (bounded oscillation).
//
// All continuous parameters are smoothed inside the engine (one-pole, ~15 ms
// at the model rate) so the smoothing itself is exercised by the headless
// tests; a bias step is a DC step into a ~40 dB gain stage, the loudest click
// a plugin can make. Filter cutoffs and tone shelf coefficients (which need
// exp/cos) are recomputed once per 32-sample sub-block from the smoothed
// values; everything else updates per sample.
//
#include "factory_core/Biquad.h"
#include "factory_core/DelayLine.h"
#include "factory_core/Filters.h"
#include "factory_core/OnePole.h"
#include "factory_core/RateBracket.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace factory_core
{
    class FuzzEngine
    {
    public:
        // ---- Voicing constants (also the independent-oracle inputs in tests) ----
        static constexpr double kInputHpHz     = 25.0;   // DC/rumble only — cutting real lows pre-clip thins the fuzz
        static constexpr double kEnvAttackSec  = 0.002;
        static constexpr double kEnvReleaseSec = 0.120;  // sputter at note-decay rate, not per-sample
        static constexpr double kGateDepth     = 3.5;    // idle bias at gate=1 → sech²(3.5) ≈ −48 dB
        static constexpr double kGateKnee      = 0.05;
        static constexpr double kBiasScale     = 1.2;    // b_static = kBiasScale · bias
        static constexpr double kFbMaxSafe     = 0.9;    // osc OFF: loop gain strictly < 1, always
        static constexpr double kFbMaxOsc      = 2.0;    // osc ON: bounded self-oscillation
        static constexpr double kFbDelayBaseMs = 0.15;
        static constexpr double kFbDelaySpanMs = 1.1;    // D = base + span·stab
        static constexpr double kFbLpBaseHz    = 6000.0;
        static constexpr double kFbLpRatio     = 12.0;   // LP_fc = base · ratio^(−gate): gate lowers the squeal pitch
        static constexpr double kFbHpHz        = 150.0;
        static constexpr double kToneFreqHz    = 800.0;
        static constexpr double kToneRangeDb   = 9.0;
        static constexpr double kDcBlockHz     = 15.0;
        static constexpr double kSmoothSec     = 0.015;
        static constexpr double kBypassFadeSec = 0.020;
        static constexpr double kNoiseAmp      = 1.0e-11; // deterministic seed so oscillation can start from silence

        // Internal model rate: the smallest 2^k multiple of the host rate that
        // reaches the top of the standard matrix (≥ 176.4 kHz). At 176.4/192 kHz
        // the bracket is a bit-exact zero-latency passthrough.
        static double modelRateFor (double hostRate) noexcept
        {
            if (hostRate < 60000.0)  return hostRate * 4.0;
            if (hostRate < 120000.0) return hostRate * 2.0;
            return hostRate;
        }

        // The static transfer at a fixed operating point — the editor's curve
        // display and the tests' analytic oracle share this exact function.
        static double shapeStatic (double x, double driveLin, double bEff) noexcept
        {
            return std::tanh (driveLin * x + bEff) - std::tanh (bEff);
        }

        // ---- Lifecycle ----------------------------------------------------------
        void prepare (double hostRate, int maxHostBlock)
        {
            modelR = modelRateFor (hostRate);
            bracket.prepare (hostRate, modelR, maxHostBlock);

            smoothCoeff = 1.0 - std::exp (-1.0 / (kSmoothSec * modelR));
            bypassCoeff = 1.0 - std::exp (-1.0 / (kBypassFadeSec * modelR));
            envAttack   = 1.0 - std::exp (-1.0 / (kEnvAttackSec * modelR));
            envRelease  = 1.0 - std::exp (-1.0 / (kEnvReleaseSec * modelR));

            const int maxDelay = (int) std::ceil ((kFbDelayBaseMs + kFbDelaySpanMs) * 1.0e-3 * modelR) + 4;
            for (auto& c : ch)
            {
                c.fbDelay.prepare (maxDelay);
                c.inputHp.setCutoff (kInputHpHz, modelR);
                c.fbHp.setCutoff (kFbHpHz, modelR);
                c.dcHp.setCutoff (kDcBlockHz, modelR);
            }
            reset();
        }

        // Clears all state and snaps every smoother to its target, so a fresh
        // prepare/reset produces bit-identical output for identical input
        // (regression class E: no state survives a transport/bypass transition).
        void reset() noexcept
        {
            bracket.reset();
            for (auto& c : ch)
                c.clear();
            noiseState = 0x9e3779b97f4a7c15ull;

            drive.snap(); bias.snap(); gate.snap(); kfb.snap();
            tone.snap();  level.snap(); mix.snap();  wetFade.snap();
            coeffCountdown = 0;
            updateBlockCoeffs();
        }

        int latencySamples() const noexcept { return bracket.latencySamples(); }

        // ---- Parameter targets (safe to call once per block from atomics) ------
        void setDriveDb (double db) noexcept  { drive.target = std::pow (10.0, db / 20.0); }
        void setBias (double b) noexcept      { bias.target = kBiasScale * std::clamp (b, -1.0, 1.0); }
        void setGate (double g) noexcept      { gate.target = std::clamp (g, 0.0, 1.0); }
        void setTone (double t) noexcept      { tone.target = std::clamp (t, -1.0, 1.0); }
        void setLevelDb (double db) noexcept  { level.target = std::pow (10.0, db / 20.0); }
        void setMix (double m) noexcept       { mix.target = std::clamp (m, 0.0, 1.0); }
        void setBypassed (bool b) noexcept    { wetFade.target = b ? 0.0 : 1.0; }

        void setStab (double s) noexcept
        {
            stabValue  = std::clamp (s, 0.0, 1.0);
            kfb.target = (oscEnabled ? kFbMaxOsc : kFbMaxSafe) * stabValue;
        }

        // Squeal switch. OFF clamps the feedback ceiling so the loop cannot
        // oscillate at any setting; the k_fb smoother turns flipping it while
        // squealing into a click-free ramp-down.
        void setOscEnabled (bool on) noexcept
        {
            oscEnabled = on;
            kfb.target = (oscEnabled ? kFbMaxOsc : kFbMaxSafe) * stabValue;
        }

        // ---- Audio --------------------------------------------------------------
        // Mono hosts pass the same pointer for both channels (RateBracket is
        // stereo-shaped; the nam-player precedent). in/out may alias.
        void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
        {
            bracket.process (inL, inR, outL, outR, n,
                             [this] (float* l, float* r, int m) { sectionProcess (l, r, m); });

            // Once the bypass fade has fully landed, clear the nonlinear state so
            // a squeal can never survive bypass and re-entry starts clean.
            if (wetFade.target == 0.0 && wetFade.value < 1.0e-4 && ! clearedWhileBypassed)
            {
                for (auto& c : ch)
                    c.clear();
                clearedWhileBypassed = true;
            }
            if (wetFade.target > 0.0)
                clearedWhileBypassed = false;
        }

    private:
        // One-pole parameter smoother. snap() jumps to the target (prepare/reset).
        struct Smoothed
        {
            double target = 0.0, value = 0.0;
            void snap() noexcept { value = target; }
            double next (double coeff) noexcept { value += coeff * (target - value); return value; }
        };

        struct ChannelState
        {
            OnePole   inputHp, fbLp, fbHp, dcHp;
            Biquad    toneLow, toneHigh;
            DelayLine fbDelay;
            double    env = 0.0;

            void clear() noexcept
            {
                inputHp.reset(); fbLp.reset(); fbHp.reset(); dcHp.reset();
                toneLow.reset(); toneHigh.reset();
                fbDelay.reset();
                env = 0.0;
            }

            // NaN/Inf self-heal (regression class C): one bad sample must not
            // permanently poison the feedback loop.
            void clearFeedback() noexcept
            {
                fbLp.reset(); fbHp.reset(); fbDelay.reset();
            }
        };

        static double flushDenorm (double v) noexcept { return std::abs (v) < 1.0e-30 ? 0.0 : v; }

        // xorshift64* mapped to [-1, 1]. Deterministic (reset() reseeds), so two
        // runs separated by reset() are bit-identical; at 1e-11 it only matters
        // as the perturbation that lets the loop leave the exact-zero fixed
        // point and start oscillating from digital silence.
        double nextNoise() noexcept
        {
            noiseState ^= noiseState >> 12;
            noiseState ^= noiseState << 25;
            noiseState ^= noiseState >> 27;
            const std::uint64_t r = noiseState * 0x2545f4914f6cdd1dull;
            return ((double) (std::int64_t) r) * (1.0 / 9.223372036854776e18);
        }

        // Coefficients that need exp/cos: loop LP cutoff (from gate), tone
        // shelves (from tone). Recomputed once per 32-sample sub-block.
        void updateBlockCoeffs() noexcept
        {
            if (modelR <= 0.0)
                return; // not prepared yet

            const double lpHz = kFbLpBaseHz * std::pow (kFbLpRatio, -gate.value);
            const double toneDb = kToneRangeDb * tone.value;
            const auto lowC  = designFilter (BandType::LowShelf,  kToneFreqHz, -toneDb, 0.70710678, modelR);
            const auto highC = designFilter (BandType::HighShelf, kToneFreqHz,  toneDb, 0.70710678, modelR);
            for (auto& c : ch)
            {
                c.fbLp.setCutoff (lpHz, modelR);
                c.toneLow.setCoeffs (lowC);
                c.toneHigh.setCoeffs (highC);
            }
        }

        double processChannel (ChannelState& c, double dry, double biasNow, double driveNow,
                               double kfbNow, double delaySamples, double levelNow,
                               double mixNow, double fadeNow, double noise) noexcept
        {
            const double x = c.inputHp.hp (dry);

            // Envelope follower (peak, attack/release) with an exact-zero floor:
            // digital silence keeps env at 0, so the gate formula produces the
            // full starve and the output stays silent (no phantom output).
            const double rect = std::abs (x);
            c.env += (rect > c.env ? envAttack : envRelease) * (rect - c.env);
            c.env  = flushDenorm (c.env);

            const double bEff = biasNow + gate.value * kGateDepth * kGateKnee / (c.env + kGateKnee);

            double fb = c.fbHp.hp (c.fbLp.lp (c.fbDelay.readInterpolated (delaySamples)));
            if (! std::isfinite (fb))
            {
                c.clearFeedback();
                fb = 0.0;
            }

            const double u = driveNow * x + bEff + kfbNow * fb + noise;
            // ADAA slot: if the alias gate ever needs tightening, replace this
            // tanh with the 1st-order antiderivative form (F(u) = ln cosh u,
            // y = (F(u)−F(u₋₁))/(u−u₋₁)) — worth ~12 dB, at the cost of half a
            // sample of delay that re-tunes the squeal (recalibrate test 5).
            double y = std::tanh (u) - std::tanh (bEff);
            if (! std::isfinite (y))
            {
                c.clearFeedback();
                y = 0.0;
            }
            c.fbDelay.write (flushDenorm (y));

            y = c.toneHigh.processSample (c.toneLow.processSample (y));
            y = c.dcHp.hp (y);
            y = flushDenorm (y);

            const double active = (1.0 - mixNow) * dry + mixNow * levelNow * y;
            return dry + fadeNow * (active - dry); // bypass fade, latency-aligned
        }

        void sectionProcess (float* l, float* r, int m) noexcept
        {
            int i = 0;
            while (i < m)
            {
                if (coeffCountdown <= 0)
                {
                    updateBlockCoeffs();
                    coeffCountdown = 32;
                }
                const int chunk = std::min (coeffCountdown, m - i);
                for (int k = 0; k < chunk; ++k, ++i)
                {
                    const double driveNow = drive.next (smoothCoeff);
                    const double biasNow  = bias.next (smoothCoeff);
                    gate.next (smoothCoeff);
                    const double kfbNow   = kfb.next (smoothCoeff);
                    tone.next (smoothCoeff);
                    const double levelNow = level.next (smoothCoeff);
                    const double mixNow   = mix.next (smoothCoeff);
                    const double fadeNow  = wetFade.next (bypassCoeff);

                    // Delay in samples from the smoothed feedback amount: D tracks
                    // stab through kfb so the squeal pitch glides with the knob.
                    const double stabNow = kfb.value / (oscEnabled ? kFbMaxOsc : kFbMaxSafe);
                    const double delaySamples =
                        (kFbDelayBaseMs + kFbDelaySpanMs * std::clamp (stabNow, 0.0, 1.0)) * 1.0e-3 * modelR;

                    l[i] = (float) processChannel (ch[0], (double) l[i], biasNow, driveNow, kfbNow,
                                                   delaySamples, levelNow, mixNow, fadeNow, kNoiseAmp * nextNoise());
                    r[i] = (float) processChannel (ch[1], (double) r[i], biasNow, driveNow, kfbNow,
                                                   delaySamples, levelNow, mixNow, fadeNow, kNoiseAmp * nextNoise());
                }
                coeffCountdown -= chunk;
            }
        }

        RateBracket<PolyphaseResampler> bracket;
        std::array<ChannelState, 2> ch;

        double modelR = 0.0;
        double smoothCoeff = 1.0, bypassCoeff = 1.0, envAttack = 1.0, envRelease = 1.0;
        int    coeffCountdown = 0;

        Smoothed drive { 1.0, 1.0 }, bias, gate, kfb, tone, level { 1.0, 1.0 }, mix { 1.0, 1.0 },
                 wetFade { 1.0, 1.0 };
        double stabValue = 0.0;
        bool   oscEnabled = false;
        bool   clearedWhileBypassed = false;

        std::uint64_t noiseState = 0x9e3779b97f4a7c15ull;
    };
} // namespace factory_core
