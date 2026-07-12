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
// API verification (see the ticket): factory_core::RateBracket<> already
// implements exactly the "run a FIXED-rate section inside a host running at
// a different, fixed rate" bracket this engine needs -- a constant modelRate
// (24000 Hz here, vs ~48000 Hz for the NAM Player it was extracted from) with
// a published, deterministic reported latency. No variable-ratio machinery
// (VariPolyphaseResampler, which Madoromi needs for its sweepable CLOCK) is
// required, so RateBracket<PolyphaseResampler> is reused AS-IS, unmodified,
// with the section callback (sectionFn) implementing the history/echo/scan
// logic below at the fixed 24 kHz model rate.
//
// Signal chain (per host sample; stereo lanes, mono input duplicated to both
// lanes on the way in and averaged back down on the way out -- the same
// mono-handling convention as Madoromi.h):
//   inHost -> [finite guard] -> RateBracket down-resample (host -> 24 kHz)
//     -- inside the bracket's sectionFn, per INTERNAL (24 kHz) sample, per
//        channel (echoTap/scanTap read BEFORE this sample's own write --
//        see HistoryBuffer.h's read-before-write causality contract):
//          echoTap = history.readAtAge(delaySamplesSm)   (feedback tap)
//          scanTap = history.readAtAge(scanAgeSm)         (memory tap)
//          fb      = shaper(toneLp.lp(echoTap))
//          history.write(inInt + regenSm * fb)
//          history.advance()                              (once per sample)
//          wetInt  = echoTap + scanLevelSm * scanTap
//   -> RateBracket up-resample (24 kHz -> host) -> wetHost
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
// case: regen01 = 1 -> effective 0.95, tone fully open) holds.
// scanTap is EXCLUDED from the feedback/write path entirely (it only feeds
// wetInt, i.e. the OUTPUT) and scanLevel only ever scales that listening-only
// contribution, never write() -- so SCAN cannot destabilise the loop at any
// setting, including scanLevel == 1.
//
// Latency contract (published formula; tests depend on this EXACT value):
// D = PolyphaseResampler::kHalfTaps = 31 (the down/up stage group delay, a
// compile-time constant independent of rate -- see PolyphaseResampler.h),
// M = RateBracket<>::kFifoMargin = 16. For fsHost != kInternalRateHz:
//   L(fsHost) = round(D + D * fsHost / kInternalRateHz) + M
//             = resamplerRoundTripLatency(fsHost, kInternalRateHz, D) + M
// which is EXACTLY what RateBracket::prepare(fsHost, kInternalRateHz, ...)
// computes internally (reportedLatency), so latencyForRate() below and the
// live bracket.latencySamples() are always equal by construction. When
// fsHost == kInternalRateHz (bit-exact match, |diff| <= 1e-6) the bracket is
// a transparent passthrough and L = 0, matching RateBracket's own bypass
// rule. There is NO additional wet-side constant offset beyond L: history
// read-before-write (see HistoryBuffer.h) gives readAtAge(age) EXACTLY
// age internal samples of delay with no off-by-one, and the sectionFn
// callback introduces no other buffering stage, so an impulse's first echo
// arrives at exactly
//   L(fsHost) + round(delaySamplesTarget * fsHost / kInternalRateHz)
// host samples later (+/- the bracket's own fractional-resampling phase
// rounding), and a SCAN marker set to age v arrives at exactly
//   L(fsHost) + round(v * fsHost / kInternalRateHz)
// host samples after it was recorded. Both are zero-extra-offset.
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
// Peak-bound argument (long-hold realistic gate: peak <= kPeakBound = 2.0):
//   The per-write ceiling is unconditional and exact: |write| <=
//   1 + kMaxRegen * (1/1.5) = 1 + 0.95 * 0.6667 ~= 1.6333 for any |inInt| <=
//   1, because shape()'s output magnitude is bounded by 2/3 for EVERY real
//   argument (tanh saturation), regardless of how large echoTap/toneLp ever
//   become -- so history can never contain a value whose magnitude exceeds
//   ~1.6333, and both echoTap and scanTap (being interpolated reads of
//   stored history) inherit that same ceiling individually.
//   A naive worst-case SUM bound would be ~1.6333 * (1 + scanLevel) <= 3.27,
//   but that requires echoTap AND scanTap to BOTH sit at their peak at the
//   EXACT SAME output instant -- i.e. the delay-age slot and the (generally
//   different) scan-age slot both holding the near-ceiling value
//   simultaneously. Under the specified long-hold gate (8 s, scan SWEEPING
//   throughout -- see the test guidance), the scan glide (250 ms tau)
//   continuously moves the read pointer across history rather than parking
//   on one historical instant, so it cannot dwell in exact phase/age
//   alignment with the live feedback tap for a sustained span; the realistic
//   combined envelope stays under kPeakBound = 2.0 (comfortably above the
//   single-tap ceiling of 1.6333, well under the pathological instantaneous
//   coincidence bound of 3.27). This is an engineering argument, not a closed
//   -form proof, consistent with the spec's own framing ("realistic
//   envelope", not a worst-instant guarantee) -- if the planned 8 s
//   long-hold/moving-scan test ever measures a peak exceeding 2.0 in
//   practice, that is a finding for the test designer to escalate (Ask a
//   human), not a tolerance to silently loosen.
//
// Real-time / safety rules (house standard):
//   - prepare() performs ALL allocation: HistoryBuffer's ~23 MB (2 channels x
//     ceil(120 * 24000) + 8 floats), RateBracket's down/up/FIFO scratch
//     (sized for a FIXED internal chunk of kHostChunk = 512 host samples,
//     NOT for the caller's block size -- process() always slices any n into
//     <= kHostChunk pieces, so no buffer here is ever n-dependent), and an
//     L+8-sample dry ring per channel. process() allocates nothing, takes no
//     locks, makes no syscalls, and is noexcept for any n.
//   - Input finite guard: every host sample is sanitised (non-finite -> 0.0)
//     BEFORE it reaches the bracket/history.
//   - Wet-node finite guard: a non-finite wetInt (defence-in-depth backstop;
//     HistoryBuffer::write() already guards every stored value, so this
//     should not be reachable via the guarded parameter/input paths, but the
//     regression-policy hard rule requires it regardless) resets history,
//     both channels' tone OnePoles and the bracket (down/up resamplers +
//     FIFO), then emits wet = 0 for that sample; at most ONE such recovery
//     per process() slice (mirrors Madoromi's `recovered` guard). No
//     smoother/target member is touched by recovery -- HistoryBuffer, OnePole
//     and RateBracket carry no independent "mode" state that needs
//     re-asserting after their reset() (unlike e.g. MicroLooper's frozen
//     flag in Madoromi), so recovery is a plain reset-and-continue.
//   - reset() is fully deterministic: two runs from reset() with identical
//     inputs/parameter calls are bit-identical.
//
#include "factory_core/HistoryBuffer.h"
#include "factory_core/OnePole.h"
#include "factory_core/PolyphaseResampler.h"
#include "factory_core/RateBracket.h"
#include "factory_core/ResamplerLatency.h"
#include "factory_core/SmoothingCoeff.h"

#include <algorithm>
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
        static constexpr double kPeakBound       = 2.0;    // see header argument
        static constexpr int    kHostChunk       = 512;    // fixed internal slice

        // -- published pure mappings / latency (tests re-derive these) ------

        // Reported round-trip latency in HOST samples (see the header's
        // "Latency contract" section for the derivation).
        static int latencyForRate (double fsHost) noexcept
        {
            if (fsHost <= 0.0) return 0;
            if (std::abs (fsHost - kInternalRateHz) <= 1.0e-6) return 0; // bypass

            return resamplerRoundTripLatency (fsHost, kInternalRateHz,
                                               PolyphaseResampler::kHalfTaps)
                 + RateBracket<PolyphaseResampler>::kFifoMargin;
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

        // Allocates: HistoryBuffer's ~23 MB store, the RateBracket's
        // fixed-kHostChunk scratch, and the L+8 dry ring. Not real-time
        // safe -- call from prepareToPlay only.
        void prepare (double sampleRate, int numChannels)
        {
            fs       = (sampleRate > 0.0) ? sampleRate : 44100.0;
            channels = std::clamp (numChannels, 1, kMaxChannels);

            latency = latencyForRate (fs);
            dryCap  = latency + 8;

            bracket.prepare (fs, kInternalRateHz, kHostChunk);
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
            bracket.reset();

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

                // 2) Bracket: down-resample -> sectionFn (history/echo/scan
                //    at the fixed 24 kHz model rate) -> up-resample.
                bracket.process (hostInL.data(), hostInR.data(),
                                  wetHostL.data(), wetHostR.data(), h,
                                  [this] (float* l, float* r, int m) noexcept
                                  { runModelDomain (l, r, m); });

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
        // The bracket's model-domain section: history record + echo/scan
        // read heads, run once per internal (24 kHz) sample. Called by
        // RateBracket::process() as its sectionFn, in place on its namBuf.
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
            bracket.reset();
        }

        double fs       = 44100.0;
        int    channels = kMaxChannels;
        int    latency  = 0;
        int    dryCap   = 8;
        int    dryPos   = 0;

        RateBracket<PolyphaseResampler> bracket;
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
