#pragma once
//
// factory_core/OmoideEcho.h -- a Chase Bliss Habit-style "echo that
// remembers": a conventional feedback delay (echoTap) plus a continuously
// recorded 120-second history that a second, feedback-free read head (SCAN)
// can revisit at any past instant (scanTap). History is kept at a FIXED
// internal rate, kInternalRateHz = 24000 Hz (lower than every supported host
// rate), both for the lo-fi "tape memory" character and to bound the
// history footprint (~23 MB; see HistoryBuffer.h). Header-only,
// JUCE-independent, headless-testable.
//
// Resampler choice (SUPERSEDES an earlier ticket note that reused
// RateBracket<PolyphaseResampler> as-is): independent review found that
// PolyphaseResampler's down-conversion kernel is NOT index-stretched -- its
// half-width is a FIXED kHalfTaps = 31 input samples regardless of the
// decimation ratio, so at high host rates (176.4/192 kHz, an ~8:1
// decimation down to the 24 kHz internal rate) the same 63-tap kernel is
// asked to carve a much narrower absolute cutoff, widening the transition
// band well past the intended stopband edge -- the measured alias floor was
// only -18..-20 dB at 176.4/192 kHz (spec requires <= -40 dB; 44.1-96 kHz
// were fine at -46..-85 dB, since their decimation ratio is smaller). This
// is a genuine in-band alias defect, not HF rolloff, and is fixed here by
// replacing BOTH the down- and up-conversion stages with
// factory_core::VariPolyphaseResampler run at a FIXED ratio -- its
// index-stretch mechanism scales the effective kernel width with the ratio,
// exactly the technique already proven in factory_core::Madoromi's engine
// (see that header and VariPolyphaseResampler.h's own design contract).
// Each instance's setTargetRatio() is called exactly ONCE, in prepare(), to
// the engine's fixed host<->24 kHz ratio and never again -- there is no
// runtime ratio sweep here (unlike Madoromi's sweepable CLOCK), so the
// per-output step ramp inside VariPolyphaseResampler sits permanently at its
// target (zero drift) for the whole life of a prepare() call.
// RateBracket<> itself is NOT reused: its process()/prepare() call sites are
// hard-wired to PolyphaseResampler's 2-argument prepare() and no-argument
// groupDelayInputSamples(), neither of which match VariPolyphaseResampler's
// signatures (3-argument prepare(), ratio-parameterised
// groupDelayInputSamples(double)), and RateBracket.h may not be modified (it
// is a shared primitive other plugins depend on) -- so this header hosts a
// small, private down/section/up/FIFO bracket directly, mirroring
// RateBracket's own internal shape INCLUDING its latency-correctness
// pattern (see the Latency contract below, which also documents a
// class-of-bug this bracket deliberately avoids).
//
// Signal chain (per host sample; stereo lanes, mono input duplicated to both
// lanes on the way in and averaged back down on the way out -- the same
// mono-handling convention as Madoromi.h):
//   inHost -> [finite guard] -> down-resample (host -> 24 kHz, FIXED ratio,
//     VariPolyphaseResampler)
//     -- per INTERNAL (24 kHz) sample, per channel (echoTap/scanTap read
//        BEFORE this sample's own write -- see HistoryBuffer.h's
//        read-before-write causality contract):
//          echoTap = history.readAtAge(delaySamplesSm)   (feedback tap)
//          scanTap = history.readAtAge(scanAgeSm)         (memory tap)
//          fb      = shaper(toneLp.lp(echoTap))
//          history.write(inInt + regenSm * fb)
//          history.advance()                              (once per sample)
//          wetInt  = echoTap + scanLevelSm * scanTap
//   -> up-resample (24 kHz -> host, FIXED ratio, VariPolyphaseResampler) ->
//     output FIFO (delivers exactly h host samples every process() slice) ->
//     wetHost
//   out = (1 - mixSm) * dryDelayed + mixSm * wetHost
// dryDelayed is the input delayed by exactly latencySamples() INTEGER host
// samples, so mix = 0 reproduces the input bit-exactly delayed by L (the
// standard wet/dry latency-compensation contract used across this repo).
//
// IMPORTANT: wetInt never includes a bare "inInt" passthrough term -- unlike
// Madoromi's wash (itself the always-on live signal), Omoide Echo's wet
// signal is the echo(es) ONLY; the outer dry path (outside the bracket, see
// above) is what supplies the un-delayed input at mix < 1.
//
// Loop-gain contract (regression-policy hard rule: gain < 1 at EVERY
// in-range setting):
//   shaper(x) = (1/1.5) * tanh(1.5 * x)  -- small-signal slope exactly 1
//     (d/dx at x=0 is sech^2(0) == 1); ANY-input ceiling exactly 2/3 (tanh
//     saturates to +-1 for every finite x, however large -- this bound is
//     UNCONDITIONAL, not just a small-signal property; see the peak-bound
//     argument below).
//   regen effective = regen01 * kMaxRegen, kMaxRegen = 0.95 (setRegen01).
//   toneLp: one-pole lowpass (OnePole.h), |H(z)| <= 1 at every frequency (a
//     convex-combination recursion z=(1-a)x+az_prev, so its output is
//     bounded by the max magnitude of its own recent inputs).
//   readAtAge(): linear interpolation between two stored samples -- a convex
//     combination, so its output magnitude is bounded by the larger of the
//     two -- gain <= 1.
// Small-signal loop gain = 0.95 * 1 * (<=1) * (<=1) < 1 at every in-range
// regen/tone/delay setting -- the impulseResponseNonIncreasing gate (worst
// case: regen01 = 1 -> effective 0.95, tone fully open) holds. This loop
// lives entirely INSIDE the 24 kHz model domain (runModelDomain below) and
// is unaffected by the down/up resampler choice above -- the resamplers
// bracket the loop from the outside, they are not part of it.
// scanTap is EXCLUDED from the feedback/write path entirely (it only feeds
// wetInt, i.e. the OUTPUT) and scanLevel only ever scales that listening-only
// contribution, never write() -- so SCAN cannot destabilise the loop at any
// setting, including scanLevel == 1.
//
// Latency contract (published formula; tests depend on this EXACT value --
// REVISED by the index-stretch fix above: the down-stage delay now SCALES
// with the decimation ratio instead of being fixed, so the reported latency
// at high host rates is larger than the pre-fix design would have reported):
//   D = VariPolyphaseResampler::kHalfTaps = 31 (ratio-1 half-width, in the
//     INPUT samples of whichever stage -- same numeric value as
//     PolyphaseResampler::kHalfTaps, unchanged by this fix; what changes is
//     how the down stage's EFFECTIVE half-width scales with ratio).
//   M = kFifoMargin = 16 (this header's own safety-cushion constant -- same
//     role and same value as RateBracket<>::kFifoMargin, now owned locally
//     since RateBracket<> is no longer used here -- see above).
//   For fsHost != kInternalRateHz, with r_down = fsHost / kInternalRateHz
//   (> 1 for every one of the 6 standard rates, since 24 kHz is below every
//   supported host rate) and r_up = kInternalRateHz / fsHost (< 1):
//     downDelayHost = VariPolyphaseResampler::groupDelayInputSamples(r_down)
//       -- already in HOST samples (down's own "input" domain IS the
//       host-rate stream) = D * max(1, r_down) = D * r_down -- STRETCHED:
//       this is the fix. VariPolyphaseResampler's index-stretch makes the
//       down stage's effective half-width scale with the decimation ratio
//       (see VariPolyphaseResampler.h's "Kernel index-stretch" design
//       contract), so at 192 kHz the down-stage delay is D * 8 = 248 host
//       samples, not the fixed D = 31 the old (defective) PolyphaseResampler
//       produced -- a longer effective FIR at high ratio is exactly what
//       buys the extra ~50 dB of stopband attenuation the alias-floor fix
//       requires.
//     upDelayHost = VariPolyphaseResampler::groupDelayInputSamples(r_up)
//       * (fsHost / kInternalRateHz) -- the raw call returns INTERNAL (24
//       kHz) samples (up's own "input" domain IS the 24 kHz stream)
//       = D * max(1, r_up) = D * 1 = D -- NOT stretched: interpolation
//       (up-conversion) never needs index-stretch (its own stretch factor
//       clamps to 1 by construction, max(1, r_up) with r_up < 1), so the up
//       stage is numerically IDENTICAL to what the old PolyphaseResampler
//       already did here -- nothing regresses on the up side. The `*
//       fsHost/kInternalRateHz` converts that internal-sample delay to its
//       host-sample equivalent (1 internal sample spans that many host
//       samples) -- algebraically the SAME expression as the down stage's
//       stretched delay.
//   Round-trip group delay in host samples:
//     G(fsHost) = downDelayHost + upDelayHost
//               = D*(fsHost/kInternalRateHz) + D*(fsHost/kInternalRateHz)
//               = 2 * D * (fsHost / kInternalRateHz)
//   L(fsHost) = round(G(fsHost)) + M
//   which is EXACTLY what latencyForRate() below computes (via the actual
//   VariPolyphaseResampler::groupDelayInputSamples() calls, not a hand
//   re-derivation of its max(1,r) clamp) and EXACTLY the round-trip delay
//   realised by the two cascaded VariPolyphaseResampler instances' own
//   causal timing (each's reset() starts its output phase at
//   -D * max(1, its own fixed ratio) -- see VariPolyphaseResampler.h -- so
//   nothing here manually "adds" G; it emerges from actually running the
//   signal through the cascade). When fsHost == kInternalRateHz (bit-exact
//   match, |diff| <= 1e-6) both stages are bypassed entirely and L = 0.
//
//   Per-rate table (the 6 standard rates; supersedes the pre-fix table,
//   whose down stage was NOT ratio-stretched: 44100->104, 48000->109,
//   88200->161, 96000->171, 176400->275, 192000->295):
//     44100 Hz  -> L = round(2*31*44100 /24000) + 16 = 114 + 16 = 130
//     48000 Hz  -> L = round(2*31*48000 /24000) + 16 = 124 + 16 = 140
//     88200 Hz  -> L = round(2*31*88200 /24000) + 16 = 228 + 16 = 244
//     96000 Hz  -> L = round(2*31*96000 /24000) + 16 = 248 + 16 = 264
//     176400 Hz -> L = round(2*31*176400/24000) + 16 = 456 + 16 = 472
//     192000 Hz -> L = round(2*31*192000/24000) + 16 = 496 + 16 = 512
//
//   PROOF that wet transit == L exactly (dry aligned): the output FIFO is
//   pre-filled with EXACTLY kFifoMargin silent zeros at reset()/recovery --
//   NEVER latency + anything -- and every process() call pushes the
//   up-stage's fresh output into the FIFO and immediately pulls exactly h
//   samples back out, so the FIFO's OCCUPANCY (write position minus read
//   position) is a CONSTANT kFifoMargin for the entire run (push count and
//   pull count track each other 1:1 in the long run because
//   r_down * r_up == 1 by construction -- the two ratios are exact
//   reciprocals of one another). A host sample that enters the
//   down-resampler at time t=0 does not reach the up-resampler's OUTPUT
//   stream (i.e. does not become available to push into the FIFO) until
//   G(fsHost) samples later -- that is the cascade's own inherent,
//   unavoidable causal delay, identical in nature to how a single
//   PolyphaseResampler instance delays its own output by its group delay.
//   Once it IS pushed, the constant kFifoMargin-sample FIFO occupancy
//   delivers it exactly kFifoMargin samples after that. Total:
//   G(fsHost) + kFifoMargin = L(fsHost) exactly -- no more, no less. Dry is
//   delayed by the SAME integer L via dryBuf (see process() below), so wet
//   and dry emerge in lockstep at every mix setting.
//
//   Contrast with the bug this deliberately avoids: Madoromi's own
//   VariPolyphaseResampler-based bracket (factory_core/Madoromi.h) pre-fills
//   its FIFO with `fifoFill = latency + kFifoSafetyPad` zeros -- MORE than
//   its own reported `latency` -- while publishing only `latency` (omitting
//   the `kFifoSafetyPad` term) to the host. Because that FIFO's occupancy is
//   therefore pinned at `latency + kFifoSafetyPad`, not `latency`,
//   Madoromi's wet signal actually emerges kFifoSafetyPad samples LATER than
//   its reported latency (a PDC/class-N misalignment bug). This header's
//   bracket instead prefills by kFifoMargin ONLY (never latency-dependent)
//   and folds the ENTIRE remaining delay into L itself via the G(fsHost)
//   term above -- the same "prefill by margin only" pattern RateBracket<>
//   already uses correctly (see RateBracket.h's own header note on the
//   0.1.0 wet/dry mismatch it once had and fixed the same way) -- so there
//   is no unaccounted-for term left outside the reported latency here.
//
//   There is NO additional wet-side constant offset beyond L: history
//   read-before-write (see HistoryBuffer.h) gives readAtAge(age) EXACTLY
//   age internal samples of delay with no off-by-one, and the model-domain
//   callback introduces no other buffering stage, so an impulse's first
//   echo arrives at exactly
//     L(fsHost) + round(delaySamplesTarget * fsHost / kInternalRateHz)
//   host samples later (+/- the bracket's own fractional-resampling phase
//   rounding), and a SCAN marker set to age v arrives at exactly
//     L(fsHost) + round(v * fsHost / kInternalRateHz)
//   host samples after it was recorded. Both are zero-extra-offset (echo/
//   scan onset = L, unchanged from before the fix).
//
// Parameter contracts (every non-bool setter: if (!isfinite(v)) return; --
// the previous target is kept; all internal sample/tau maths below run at
// the FIXED kInternalRateHz, so every alpha is a compile-time-shaped
// constant computed ONCE in prepare(), unlike Madoromi's variable clock):
//   setDelayMs(ms): clamp [kDelayMinMs=100, kDelayMaxMs=10000]. Internal
//     sample TARGET = llround(ms * kInternalRateHz / 1000) (an exact integer
//     re-derived on every call). The actual read age (delaySamplesSm) GLIDES
//     toward that integer target with a one-pole of tau = kDelayGlideTauMs =
//     60 ms in INTERNAL samples: alpha = onePoleAlphaForTauSamples(tau *
//     1e-3 * kInternalRateHz), applied once per internal sample. While
//     gliding, echoTap is read at the continuously-moving fractional age,
//     producing the intended tape-style doppler pitch bend (design intent,
//     not a bug).
//   setRegen01(v): clamp [0,1] -> smoothing TARGET = v * kMaxRegen. Glided
//     with a one-pole of tau = kParamSmoothMs = 20 ms in INTERNAL samples.
//   setToneHz(v): clamp [kToneMinHz=500, kToneMaxHz=11000] -> smoothing
//     TARGET in Hz, glided (tau = 20 ms, internal samples); the feedback-path
//     OnePole's cutoff is re-derived from the glided Hz every internal
//     sample (OnePole::setCutoff only touches its coefficient, not its
//     state, so this is cheap and allocation-free).
//   setScan01(v): clamp [0,1] -> age TARGET (seconds) = v * (kMaxHistorySec
//     - kScanMarginSec), kScanMarginSec = 1.0, converted to internal SAMPLES
//     (* kInternalRateHz). GLIDE is a HEAVY one-pole, tau = kScanGlideTauMs =
//     250 ms (internal samples) -- while scanning, the read age moves slowly
//     enough to sound like a tape transport winding, an intentional pitch
//     glide (design intent).
//   setScanLevel01(v): clamp [0,1], glided (tau = 20 ms, internal samples).
//     At scanLevel = 0 the scan head is silent; combined with regen = 0 and
//     enough elapsed time past the delay, the detector-floor contract holds
//     (residual <= 1e-12; see the safety section below).
//   setMix01(v): clamp [0,1], glided with tau = 20 ms IN HOST samples (this
//     is the one smoother that runs in the outer, host-rate loop, since it
//     blends dryDelayed with wetHost AFTER the bracket, not inside it).
//
// Peak-bound argument (long-hold realistic gate: peak <= kPeakBound = 3.5 --
//   RESOLVED per D4; SUPERSEDES an earlier draft of this argument that
//   claimed a <= 2.0 bound, which independent review found false):
//   The per-write ceiling is unconditional and exact: |write| <=
//   1 + kMaxRegen * (1/1.5) = 1 + 0.95 * 0.6667 ~= 1.6333 for any |inInt| <=
//   1, because shape()'s output magnitude is bounded by 2/3 for EVERY real
//   argument (tanh saturation), regardless of how large echoTap/toneLp ever
//   become -- so history can never contain a value whose magnitude exceeds
//   ~1.6333, and both echoTap and scanTap (being interpolated reads of
//   stored history) inherit that same ceiling individually.
//   TRUE analytic worst case: wetInt = echoTap + scanLevelSm * scanTap, so at
//   scanLevelSm == 1 the two taps CAN simultaneously read history slots each
//   holding the near-ceiling ~1.6333 value with the SAME sign -- this is not
//   merely hypothetical: under the long-hold gate's continuously-sweeping
//   scan (250 ms tau glide, moving across the full history range), the scan
//   age passes through every value, INCLUDING a momentary coincidence with
//   the live echo's own delay age, so the two taps genuinely superpose for
//   an instant. That gives an analytic worst-case peak of 2 * 1.6333 ~=
//   3.267 -- a realistic superposition of two independently-bounded taps,
//   NOT a runaway or a loop-gain violation (the feedback Loop-gain contract
//   above is untouched by this; scanTap is excluded from feedback entirely).
//   On top of that, the down/up VariPolyphaseResampler pair's sinc kernel
//   has the standard negative-lobe overshoot of an ideal-lowpass (Gibbs-
//   style) FIR, which can lift the bracket's output magnitude slightly above
//   the |inInt| <= 1 assumption the per-write ceiling above is stated in
//   terms of -- raising the realistic worst case modestly further, to
//   ~3.4. kPeakBound = 3.5 gates this with a small margin above that ~3.4
//   figure: comfortably wide enough to absorb the analytic worst case, while
//   still catching genuine instability (an unbounded feedback loop would
//   blow far past 3.5, not sit just above it -- kMaxRegen = 0.95 keeps
//   small-signal loop gain < 1 at every in-range setting regardless of this
//   bound, see the Loop-gain contract above). Measured worst-case peak in
//   engineLongHoldPeakTest (8 s, scan sweeping throughout, all 6 rates) is
//   ~2.99, comfortably inside this bound.
//
// Real-time / safety rules (house standard):
//   - prepare() performs ALL allocation: HistoryBuffer's ~23 MB (2 channels x
//     ceil(120 * 24000) + 8 floats), the down/up VariPolyphaseResampler
//     pair's prototype tables + input histories (sized for the ONE fixed
//     ratio each will ever run at -- see the resampler-choice note above),
//     their namBuf/upScratch scratch and output FIFOs (sized for a FIXED
//     internal chunk of kHostChunk = 512 host samples, NOT for the caller's
//     block size -- process() always slices any n into <= kHostChunk
//     pieces, so no buffer here is ever n-dependent), and an L+8-sample dry
//     ring per channel. process() allocates nothing, takes no locks, makes
//     no syscalls, and is noexcept for any n.
//   - Input finite guard: every host sample is sanitised (non-finite -> 0.0)
//     BEFORE it reaches the down-resampler/history.
//   - Wet-node finite guard: a non-finite wetInt (defence-in-depth backstop;
//     HistoryBuffer::write() already guards every stored value, so this
//     should not be reachable via the guarded parameter/input paths, but the
//     regression-policy hard rule requires it regardless) resets history,
//     both channels' tone OnePoles and the down/up resamplers + FIFOs, then
//     emits wet = 0 for that sample; at most ONE such recovery per
//     process() slice (mirrors Madoromi's `recovered` guard). No
//     smoother/target member is touched by recovery -- HistoryBuffer,
//     OnePole and VariPolyphaseResampler/the FIFO carry no independent
//     "mode" state that needs re-asserting after their reset() (unlike e.g.
//     MicroLooper's frozen flag in Madoromi), so recovery is a plain
//     reset-and-continue.
//   - reset() is fully deterministic: two runs from reset() with identical
//     inputs/parameter calls are bit-identical.
//
#include "factory_core/HistoryBuffer.h"
#include "factory_core/OnePole.h"
#include "factory_core/SmoothingCoeff.h"
#include "factory_core/VariPolyphaseResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace factory_core
{
    class OmoideEcho
    {
    public:
        static constexpr int    kMaxChannels     = 2;
        static constexpr double kInternalRateHz  = 24000.0;
        static constexpr double kMaxHistorySec   = HistoryBuffer::kMaxHistorySec; // 120.0
        static constexpr double kScanMarginSec   = 1.0;
        static constexpr double kMaxRegen        = 0.95;
        static constexpr double kDelayMinMs      = 100.0;
        static constexpr double kDelayMaxMs      = 10000.0;
        static constexpr double kToneMinHz       = 500.0;
        static constexpr double kToneMaxHz       = 11000.0;  // < internal Nyquist (12 kHz);
                                                             // OnePole clamps at 0.49*24000
                                                             // = 11760 Hz, so every in-range
                                                             // tone value is effective
        static constexpr double kDelayGlideTauMs = 60.0;
        static constexpr double kScanGlideTauMs  = 250.0;
        static constexpr double kParamSmoothMs   = 20.0;
        static constexpr double kPeakBound       = 3.5;    // see header argument (D4:
                                                            // 2*1.6333~=3.27 analytic
                                                            // worst case + sinc
                                                            // overshoot ~= 3.4, + margin)
        static constexpr int    kHostChunk       = 512;    // fixed internal slice
        static constexpr int    kFifoMargin      = 16;     // output-FIFO safety cushion;
                                                            // same role/value as
                                                            // RateBracket<>::kFifoMargin,
                                                            // now owned locally (see the
                                                            // resampler-choice note above)

        // -- published pure mappings / latency (tests re-derive these) ------

        // Reported round-trip latency in HOST samples (see the header's
        // "Latency contract" section for the full derivation and proof).
        static int latencyForRate (double fsHost) noexcept
        {
            if (fsHost <= 0.0) return 0;
            if (std::abs (fsHost - kInternalRateHz) <= 1.0e-6) return 0; // bypass

            const double rDown = fsHost / kInternalRateHz;   // > 1: decimation
            const double rUp   = kInternalRateHz / fsHost;   // < 1: interpolation

            // Down-stage delay is already in HOST samples (down's own
            // "input" domain IS the host-rate stream) -- STRETCHED by rDown,
            // which is the alias-floor fix. Up-stage delay is returned in
            // INTERNAL (24 kHz) samples (up's own "input" domain), converted
            // to host samples by * fsHost/kInternalRateHz.
            const double downDelayHost = VariPolyphaseResampler::groupDelayInputSamples (rDown);
            const double upDelayHost   = VariPolyphaseResampler::groupDelayInputSamples (rUp)
                                        * (fsHost / kInternalRateHz);

            return (int) std::lround (downDelayHost + upDelayHost) + kFifoMargin;
        }

        // The fixed-shape saturator used on the feedback path: small-signal
        // slope 1, unconditional ceiling 2/3 for any real x.
        static double shaper (double x) noexcept
        {
            return (1.0 / 1.5) * std::tanh (1.5 * x);
        }

        // setDelayMs's exact internal-sample target formula, exposed for
        // test reuse: llround(ms * kInternalRateHz / 1000).
        static long long delaySamplesForMs (double ms) noexcept
        {
            return std::llround (ms * kInternalRateHz / 1000.0);
        }

        // setScan01's exact age-in-seconds target formula, exposed for test
        // reuse: v01 * (kMaxHistorySec - kScanMarginSec).
        static double scanAgeSecondsForScan01 (double v01) noexcept
        {
            return v01 * (kMaxHistorySec - kScanMarginSec);
        }

        // Allocates: HistoryBuffer's ~23 MB store, the down/up
        // VariPolyphaseResampler pair's fixed-ratio tables/histories, their
        // fixed-kHostChunk scratch + FIFOs, and the L+8 dry ring. Not
        // real-time safe -- call from prepareToPlay only.
        void prepare (double sampleRate, int numChannels)
        {
            fs       = (sampleRate > 0.0) ? sampleRate : 44100.0;
            channels = std::clamp (numChannels, 1, kMaxChannels);

            latency = latencyForRate (fs);
            dryCap  = latency + 8;

            resampling = std::abs (fs - kInternalRateHz) > 1.0e-6;
            if (resampling)
            {
                const double downRatio = fs / kInternalRateHz;   // > 1 for every supported
                                                                  // host rate (24 kHz is
                                                                  // below every one of them)
                const double upRatio   = kInternalRateHz / fs;    // < 1 (its own stretch
                                                                   // clamps to 1 -- unchanged
                                                                   // from before the fix)

                // Model-rate scratch sized for the worst-case (largest) host slice,
                // with headroom for the resampler's fractional-phase carry-over --
                // same formula RateBracket<> used for the equivalent PolyphaseResampler
                // pairing.
                namMaxBlk = (int) std::ceil ((double) kHostChunk * kInternalRateHz / fs) + 32;
                const int upCap = (int) std::ceil ((double) namMaxBlk * fs / kInternalRateHz)
                                 + VariPolyphaseResampler::kHalfTaps + 64;

                for (int ch = 0; ch < kMaxChannels; ++ch)
                {
                    // maxRatio == the fixed ratio itself: this instance is NEVER
                    // swept after this point (setTargetRatio is called exactly
                    // once, right below, and never again) -- see the header's
                    // fixed-ratio-use contract.
                    down[(size_t) ch].prepare (fs, kInternalRateHz, downRatio);
                    up[(size_t) ch].prepare   (kInternalRateHz, fs, upRatio);
                    down[(size_t) ch].setTargetRatio (downRatio);   // explicit + redundant
                    up[(size_t) ch].setTargetRatio   (upRatio);     // with prepare()'s own
                                                                     // initial-ratio calc

                    namBuf[ch].assign    ((size_t) namMaxBlk, 0.0f);
                    upScratch[ch].assign ((size_t) upCap, 0.0f);
                    fifo[(size_t) ch].prepare (kHostChunk * 4 + 64);
                }
            }

            history.prepare (kInternalRateHz, kMaxChannels, kMaxHistorySec);

            // Fixed kInternalRateHz-derived coefficients: computed ONCE here
            // because the internal rate never changes (unlike Madoromi's
            // sweepable CLOCK, which must recompute per slice).
            delayGlideAlpha  = onePoleAlphaForTauSamples (kDelayGlideTauMs * 1.0e-3 * kInternalRateHz);
            scanGlideAlpha   = onePoleAlphaForTauSamples (kScanGlideTauMs  * 1.0e-3 * kInternalRateHz);
            modelSmoothAlpha = 1.0 - onePoleCoeffForMs (kParamSmoothMs, kInternalRateHz);
            // mixAlpha runs in the HOST-rate outer loop, so it depends on fs.
            mixAlpha         = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);

            hostInL.assign  ((size_t) kHostChunk, 0.0f);
            hostInR.assign  ((size_t) kHostChunk, 0.0f);
            wetHostL.assign ((size_t) kHostChunk, 0.0f);
            wetHostR.assign ((size_t) kHostChunk, 0.0f);

            for (int ch = 0; ch < kMaxChannels; ++ch)
                dryBuf[ch].assign ((size_t) dryCap, 0.0);

            reset();
        }

        // Deterministic: two runs from reset() with identical inputs and
        // parameter calls are bit-identical. Parameter TARGETS are untouched
        // (a setter call right before reset() takes effect immediately, with
        // no fresh glide-in) -- only the smoothed/runtime state snaps to the
        // current targets and the stateful DSP objects clear to silence.
        void reset() noexcept
        {
            history.reset();
            toneFilterL.reset();
            toneFilterR.reset();

            if (resampling)
            {
                for (int ch = 0; ch < kMaxChannels; ++ch)
                {
                    down[(size_t) ch].reset();
                    up[(size_t) ch].reset();
                    fifo[(size_t) ch].reset();
                    fifo[(size_t) ch].pushZeros (kFifoMargin);   // margin ONLY -- see
                                                                  // the header's latency
                                                                  // proof (never
                                                                  // latency-dependent)
                }
            }

            delaySamplesSm = delayTargetSamples;
            scanAgeSm      = scanAgeTargetSamples;
            regenSm        = regenTarget;
            toneHzSm       = toneHzTarget;
            scanLevelSm    = scanLevelTarget;
            mixSm          = mixTarget;

            for (int ch = 0; ch < kMaxChannels; ++ch)
                std::fill (dryBuf[ch].begin(), dryBuf[ch].end(), 0.0);
            dryPos = 0;

            std::fill (hostInL.begin(),  hostInL.end(),  0.0f);
            std::fill (hostInR.begin(),  hostInR.end(),  0.0f);
            std::fill (wetHostL.begin(), wetHostL.end(), 0.0f);
            std::fill (wetHostR.begin(), wetHostR.end(), 0.0f);
        }

        // D6 (bypass de-click): identical to reset() for every DSP node --
        // history, both tone OnePoles, both VariPolyphaseResampler stages +
        // their output FIFOs, and all six glide/smoothers (which snap to
        // their CURRENT targets exactly like reset() -- see that method's
        // own contract) -- EXCEPT it leaves dryBuf/dryPos completely
        // untouched. dryBuf is a pure passthrough delay line (no feedback,
        // no filter state -- see the "Latency contract" section above), so
        // it carries no "mode" that bypass state-hygiene requires clearing;
        // zeroing it (as a full reset() would) instead silences the
        // latency-compensated dry path for latencySamples() samples
        // (~2.7-3.0 ms) at the exact moment bypass engages/disengages -- an
        // audible dropout on a path that should stay perfectly continuous
        // across the transition.
        // Call this from the wrapper on EVERY bypass edge (both engage and
        // disengage); reserve reset() itself for prepare()/channel-count
        // changes, where the dry ring legitimately needs to start clean.
        // Contract: the caller should push the mix target to 0 (setMix01)
        // immediately BEFORE calling this, so mixSm's target-snap below
        // reads exactly 0 on either transition direction -- pure dry, ==
        // 0 * anything -- which hides the (unavoidable) discontinuity the
        // history/tone/resampler reset introduces into the wet signal,
        // regardless of which way bypass just flipped. The caller then
        // pushes the real post-transition targets right after (mix = 0
        // while bypassed, else the user's mix), and mixSm glides to that
        // target at its normal tau = kParamSmoothMs -- no separate
        // special-case needed here beyond the pre-call setMix01(0).
        void resetForBypass() noexcept
        {
            history.reset();
            toneFilterL.reset();
            toneFilterR.reset();

            if (resampling)
            {
                for (int ch = 0; ch < kMaxChannels; ++ch)
                {
                    down[(size_t) ch].reset();
                    up[(size_t) ch].reset();
                    fifo[(size_t) ch].reset();
                    fifo[(size_t) ch].pushZeros (kFifoMargin);   // margin ONLY -- see
                                                                  // the header's latency
                                                                  // proof (never
                                                                  // latency-dependent)
                }
            }

            delaySamplesSm = delayTargetSamples;
            scanAgeSm      = scanAgeTargetSamples;
            regenSm        = regenTarget;
            toneHzSm       = toneHzTarget;
            scanLevelSm    = scanLevelTarget;
            mixSm          = mixTarget;   // see the pre-call setMix01(0) contract above

            std::fill (hostInL.begin(),  hostInL.end(),  0.0f);
            std::fill (hostInR.begin(),  hostInR.end(),  0.0f);
            std::fill (wetHostL.begin(), wetHostL.end(), 0.0f);
            std::fill (wetHostR.begin(), wetHostR.end(), 0.0f);

            // dryBuf / dryPos deliberately NOT touched -- see contract above.
        }

        // -- parameters (audio thread, between process() calls) -------------
        // Non-finite values are ignored (the previous target is kept).

        // clamp [kDelayMinMs, kDelayMaxMs]; target (internal samples) =
        // llround(ms * kInternalRateHz / 1000); glided, tau = kDelayGlideTauMs.
        void setDelayMs (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            const double clamped = std::clamp (v, kDelayMinMs, kDelayMaxMs);
            delayTargetSamples = (double) delaySamplesForMs (clamped);
        }

        // clamp [0,1]; target = v * kMaxRegen; glided, tau = kParamSmoothMs.
        void setRegen01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            regenTarget = std::clamp (v, 0.0, 1.0) * kMaxRegen;
        }

        // clamp [kToneMinHz, kToneMaxHz]; target in Hz; glided,
        // tau = kParamSmoothMs; re-derives the feedback OnePole cutoff every
        // internal sample from the glided Hz.
        void setToneHz (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            toneHzTarget = std::clamp (v, kToneMinHz, kToneMaxHz);
        }

        // clamp [0,1]; age target (seconds) = scanAgeSecondsForScan01(v),
        // converted to internal samples; glided, tau = kScanGlideTauMs
        // (a HEAVY glide -- see the header's parameter contracts).
        void setScan01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            const double v01 = std::clamp (v, 0.0, 1.0);
            scanAgeTargetSamples = scanAgeSecondsForScan01 (v01) * kInternalRateHz;
        }

        // clamp [0,1]; glided, tau = kParamSmoothMs.
        void setScanLevel01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            scanLevelTarget = std::clamp (v, 0.0, 1.0);
        }

        // clamp [0,1]; glided IN HOST samples, tau = kParamSmoothMs (the
        // only smoother that runs outside the bracket -- see the header).
        void setMix01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            mixTarget = std::clamp (v, 0.0, 1.0);
        }

        int latencySamples() const noexcept { return latency; }

        // In-place, stereo (mono duplicated in / averaged out, see the
        // header). Slices numSamples into <= kHostChunk pieces so no scratch
        // buffer here is ever sized from the caller's block size.
        // Allocation-free, lock-free, noexcept for any numSamples.
        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            int offset = 0;
            while (offset < numSamples)
            {
                const int h = std::min (kHostChunk, numSamples - offset);

                // 1) Host input: finite guard, stereo lanes (mono duplicated).
                for (int i = 0; i < h; ++i)
                {
                    const double rawL = (double) audio[0][offset + i];
                    const double rawR = (double) audio[nCh == 2 ? 1 : 0][offset + i];
                    hostInL[(size_t) i] = (float) (std::isfinite (rawL) ? rawL : 0.0);
                    hostInR[(size_t) i] = (float) (std::isfinite (rawR) ? rawR : 0.0);
                }

                // 2) Down-resample (host -> 24 kHz, fixed ratio) -> model-
                //    domain section (history/echo/scan) -> up-resample
                //    (24 kHz -> host, fixed ratio) -> FIFO (exact h-sample
                //    delivery every call -- see the header's latency proof).
                if (! resampling)
                {
                    for (int i = 0; i < h; ++i)
                    {
                        wetHostL[(size_t) i] = hostInL[(size_t) i];
                        wetHostR[(size_t) i] = hostInR[(size_t) i];
                    }
                    runModelDomain (wetHostL.data(), wetHostR.data(), h);
                }
                else
                {
                    const int namN0 = down[0].process (hostInL.data(), h, namBuf[0].data(), namMaxBlk);
                    const int namN1 = down[1].process (hostInR.data(), h, namBuf[1].data(), namMaxBlk);
                    const int namN  = std::min (namN0, namN1);

                    runModelDomain (namBuf[0].data(), namBuf[1].data(), namN);

                    for (int ch = 0; ch < kMaxChannels; ++ch)
                    {
                        const int hostM = up[(size_t) ch].process (namBuf[ch].data(), namN,
                                                                    upScratch[ch].data(),
                                                                    (int) upScratch[ch].size());
                        fifo[(size_t) ch].push (upScratch[ch].data(), hostM);
                    }
                    fifo[0].pull (wetHostL.data(), h);
                    fifo[1].pull (wetHostR.data(), h);
                }

                // 3) Dry compensation (exact L-sample integer delay) + mix
                //    (glided IN HOST samples -- see the header contract).
                for (int i = 0; i < h; ++i)
                {
                    mixSm += mixAlpha * (mixTarget - mixSm);

                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const double drySrc = (nCh == 2)
                            ? (double) (ch == 0 ? hostInL[(size_t) i] : hostInR[(size_t) i])
                            : (double) hostInL[(size_t) i];
                        dryBuf[ch][(size_t) dryPos] = drySrc;

                        int rd = dryPos - latency;
                        if (rd < 0) rd += dryCap;
                        const double dryD = dryBuf[ch][(size_t) rd];

                        double w = (nCh == 2)
                            ? (double) (ch == 0 ? wetHostL[(size_t) i] : wetHostR[(size_t) i])
                            : 0.5 * ((double) wetHostL[(size_t) i] + (double) wetHostR[(size_t) i]);
                        if (! std::isfinite (w)) w = 0.0;   // final sanitize

                        audio[ch][offset + i] = (float) ((1.0 - mixSm) * dryD + mixSm * w);
                    }
                    if (++dryPos >= dryCap) dryPos = 0;
                }

                offset += h;
            }
        }

    private:
        // Fixed-capacity power-of-two ring delivering exactly h host samples
        // per slice (single audio thread; no locks). Mirrors RateBracket's
        // (and Madoromi's) own private HostFifo -- kept as a local copy here
        // because RateBracket.h may not be modified (see the header's
        // resampler-choice note).
        struct HostFifo
        {
            std::vector<float> buf;
            int mask = 0, rd = 0, wr = 0, count = 0;
            void prepare (int minSize)
            {
                int p = 1; while (p < minSize) p <<= 1;
                buf.assign ((size_t) p, 0.0f); mask = p - 1; rd = wr = count = 0;
            }
            void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); rd = wr = count = 0; }
            void pushZeros (int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = 0.0f; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void push (const float* x, int m) noexcept
            {
                for (int i = 0; i < m; ++i) { buf[(size_t) wr] = x[i]; wr = (wr + 1) & mask; if (count <= mask) ++count; else rd = (rd + 1) & mask; }
            }
            void pull (float* out, int n) noexcept
            {
                for (int i = 0; i < n; ++i) { if (count > 0) { out[i] = buf[(size_t) rd]; rd = (rd + 1) & mask; --count; } else out[i] = 0.0f; }
            }
        };

        // The model-domain section: history record + echo/scan read heads,
        // run once per internal (24 kHz) sample. Called directly from
        // process() above, in place on the down-stage's namBuf output (or
        // directly on the host buffers when the bracket is bypassed at
        // fsHost == kInternalRateHz).
        void runModelDomain (float* l, float* r, int m) noexcept
        {
            bool recovered = false;   // at most one full recovery per slice

            for (int j = 0; j < m; ++j)
            {
                // a) Advance the fixed-alpha (kInternalRateHz-derived)
                //    smoothers/glides once per internal sample.
                delaySamplesSm += delayGlideAlpha  * (delayTargetSamples   - delaySamplesSm);
                scanAgeSm      += scanGlideAlpha   * (scanAgeTargetSamples - scanAgeSm);
                regenSm        += modelSmoothAlpha * (regenTarget          - regenSm);
                toneHzSm       += modelSmoothAlpha * (toneHzTarget         - toneHzSm);
                scanLevelSm    += modelSmoothAlpha * (scanLevelTarget      - scanLevelSm);

                toneFilterL.setCutoff (toneHzSm, kInternalRateHz);
                toneFilterR.setCutoff (toneHzSm, kInternalRateHz);

                const double inL = (double) l[j];
                const double inR = (double) r[j];

                // b) Read BEFORE write (HistoryBuffer's causality contract).
                const double echoTapL = history.readAtAge (0, delaySamplesSm);
                const double echoTapR = history.readAtAge (1, delaySamplesSm);
                const double scanTapL = history.readAtAge (0, scanAgeSm);
                const double scanTapR = history.readAtAge (1, scanAgeSm);

                const double fbL = shaper (toneFilterL.lp (echoTapL));
                const double fbR = shaper (toneFilterR.lp (echoTapR));

                // c) Record (feedback INCLUDED; write()'s own finite guard
                //    is the first backstop against a corrupted feedback node).
                history.write (0, inL + regenSm * fbL);
                history.write (1, inR + regenSm * fbR);
                history.advance();

                // d) Wet = echo + scan (scan excluded from feedback/write).
                double wetL = echoTapL + scanLevelSm * scanTapL;
                double wetR = echoTapR + scanLevelSm * scanTapR;

                // e) Wet-node finite guard (defence-in-depth backstop; see
                //    the header). At most one recovery per slice.
                if (! std::isfinite (wetL) || ! std::isfinite (wetR))
                {
                    if (! recovered) { recoverWetPath(); recovered = true; }
                    wetL = wetR = 0.0;
                }

                l[j] = (float) wetL;
                r[j] = (float) wetR;
            }
        }

        // Wet-path self-recovery (wet-node finite guard). No smoother/target
        // is touched -- see the header's safety-rules note on why no
        // re-assertion step is needed here (unlike Madoromi's frozen flag).
        void recoverWetPath() noexcept
        {
            history.reset();
            toneFilterL.reset();
            toneFilterR.reset();

            if (resampling)
            {
                for (int ch = 0; ch < kMaxChannels; ++ch)
                {
                    down[(size_t) ch].reset();
                    up[(size_t) ch].reset();
                    fifo[(size_t) ch].reset();
                    fifo[(size_t) ch].pushZeros (kFifoMargin);   // margin ONLY (see reset())
                }
            }
        }

        double fs       = 44100.0;
        int    channels = kMaxChannels;
        int    latency  = 0;
        int    dryCap   = 8;
        int    dryPos   = 0;

        bool resampling = false;
        int  namMaxBlk  = 0;   // model-rate scratch capacity (down-stage outCap)

        std::array<VariPolyphaseResampler, kMaxChannels> down, up;
        std::array<HostFifo, kMaxChannels> fifo;
        std::vector<float> namBuf[kMaxChannels], upScratch[kMaxChannels];

        HistoryBuffer history;
        OnePole toneFilterL, toneFilterR;

        // Fixed (kInternalRateHz-derived) glide/smoothing coefficients,
        // computed once in prepare() -- see the header's parameter contracts.
        double delayGlideAlpha  = 1.0;
        double scanGlideAlpha   = 1.0;
        double modelSmoothAlpha = 1.0;
        double mixAlpha         = 1.0;

        // parameter targets (set by the public setters)
        double delayTargetSamples   = 12000.0;  // 500 ms @ 24 kHz (default)
        double regenTarget          = 0.40 * kMaxRegen;
        double toneHzTarget         = 6000.0;
        double scanAgeTargetSamples = 0.0;
        double scanLevelTarget      = 0.0;
        double mixTarget            = 0.35;

        // smoothed / runtime state
        double delaySamplesSm = delayTargetSamples;
        double scanAgeSm      = scanAgeTargetSamples;
        double regenSm        = regenTarget;
        double toneHzSm       = toneHzTarget;
        double scanLevelSm    = scanLevelTarget;
        double mixSm          = mixTarget;

        std::vector<float>  hostInL, hostInR, wetHostL, wetHostR;
        std::vector<double> dryBuf[kMaxChannels];
    };
} // namespace factory_core
