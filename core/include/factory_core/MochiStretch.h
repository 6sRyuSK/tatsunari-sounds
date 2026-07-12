#pragma once
//
// factory_core/MochiStretch.h -- a Red Panda Tensor-style "time machine":
// HistoryBuffer is a continuously recording tape (host rate, no internal
// rate bracket -- this engine runs entirely at the host rate; the transport's
// linear-interpolated read IS the intended lo-fi character, not a bug to
// anti-alias away). A variable-speed TRANSPORT reads the tape at a
// time-varying age A samples behind "now" (supporting stop and reverse); a
// PitchShifter (one instance per channel) then undoes the transport's own
// speed-induced pitch/tempo coupling and applies the user's PITCH control on
// top, so SPEED (tempo) and PITCH are fully decoupled (true time-stretch).
// HOLD freezes the tape (write/advance stop) while the transport keeps
// moving per its own dynamics, looping the frozen window. There is NO
// feedback path anywhere in this engine (see "Loop-gain" below). Header-only,
// JUCE-independent, headless-testable; process() allocates nothing, takes no
// locks, makes no syscalls, and is noexcept for any numSamples.
//
// API verification (composed from two existing, UNMODIFIED primitives):
//   - HistoryBuffer is the "recording ring, variable-age read" primitive
//     this engine's tape needs as-is: write(ch,x) + advance() once per host
//     sample (skipped entirely while HOLD is engaged -- freezing the ring's
//     content is then FREE, since HistoryBuffer's storage only ever changes
//     via write()); readAtAge(ch, A) for the transport tap, clamped to its
//     own [1, capacity-2] causality/safety bound (A is kept in [kAgeMin,
//     Wlen] by this engine's own wrap rule, always comfortably inside that
//     bound -- see "Worst-case sizing" below for the one place a fade
//     transient can briefly exceed the logical window edge).
//   - PitchShifter is the mono per-channel rotating-head shifter this engine
//     needs for the "undo transport pitch, apply user pitch" step; its
//     setRatio() is deliberately unclamped/unsmoothed (caller-owned), which
//     is exactly right here since this engine already glides/clamps
//     everything that feeds it (see "Parameter contracts" below). Its
//     ratio=1 degeneracy (phase pinned at window/2, i.e. a PURE
//     window/2-sample delay -- see PitchShifter.h) is exactly what test #1
//     gates on at speed=1/pitch=0 default, and this engine preserves it
//     unconditionally: at s=1 (default), dA/dt = 1-s = 0 so A never moves
//     (no wrap, no crossfade ever engages) and shifterRatio(1,ratio=1) = 1
//     exactly (comp(1) = 1/clamp(1, kCompMin, kCompMax) = 1) -- so the
//     signal path degenerates to HistoryBuffer.readAtAge(kAgeMin) feeding a
//     ratio=1 PitchShifter, i.e. exactly the D0-sample pure delay published
//     by passthroughDelaySamples() below. Both primitives fit the spec
//     without modification; no contract conflict was found.
//
// Transport dynamics (Design contract -- tests depend on these exact
// formulas; s = the SMOOTHED speed value below, i.e. sSm, used identically
// for the transport AND for the pitch compensation -- see "Pitch law"):
//   Recording (HOLD off):  dA/dt = 1 - s   (per host sample)
//   HOLD:                  dA/dt = -s      (per host sample)
//   Domain: A in [kAgeMin = 1, Wlen], Wlen = llround(windowMs * fs / 1000).
//   Wrap (checked every sample, against the CURRENTLY ACTIVE Wlen):
//     A > Wlen  ->  A -= (Wlen - 1)
//     A < kAgeMin -> A += (Wlen - 1)
//   A pending setWindowMs() change (WlenPending) is adopted into Wlen
//   exactly AT the moment a wrap triggers (i.e. "applied from the next wrap
//   boundary" -- see setWindowMs below); between wraps Wlen stays fixed, so
//   a window change at speed=1 (A never wraps) has no audible effect until
//   motion causes the next wrap (spec'd behaviour, not a bug).
//   Wrap crossfade (click prevention): kFadeMs = 10 ms, equal-power, TWO
//   read heads. On a wrap (only if no fade is already in progress -- a
//   second, single-shadow-head design; see "Resolved ambiguities" in the
//   accompanying report), the OLD head (Aold) is seeded with the RAW,
//   not-yet-folded age value (so it keeps reading as if the wrap had not
//   happened -- this is exactly why the ring is sized kMaxWindowSec+1, not
//   kMaxWindowSec: the extra second of headroom lets Aold run past the
//   logical window edge for the ~10 ms fade without saturating
//   HistoryBuffer's own defensive clamp). BOTH heads keep advancing by the
//   SAME dA/dt every sample while fading (per spec). With fadeCounter
//   counting 1..fadeLen (fadeLen = max(1, llround(kFadeMs*1e-3*fs))) and
//   g = fadeCounter / fadeLen:
//     wOld = cos(pi/2 * g),  wNew = sin(pi/2 * g)   (equal power: wOld^2 + wNew^2 = 1)
//     tapped = wOld * HistoryBuffer.readAtAge(ch, Aold)
//            + wNew * HistoryBuffer.readAtAge(ch, A)
//   and this ALREADY-BLENDED tapped signal is what feeds the PitchShifter
//   (one shifter instance per channel, not two -- the crossfade happens
//   upstream of it, per spec). At g=1 (fadeCounter==fadeLen) the fade ends
//   and A alone drives the next sample's tap.
//   Degenerate case (s=1, p=0, default): dA/dt=0 so A stays at kAgeMin
//   forever, no wrap ever triggers, and D0 = passthroughDelaySamples(fs)
//   below is the exact, constant, total passthrough delay.
//
// Pitch law (Design contract -- published pure functions, primary test
// oracle):
//   comp(s)           = 1 / clamp(|s|, kCompMin = 0.125, kCompMax = 2.0)
//   pitchFactor(s, p) = |s| * comp(s) * 2^(p/12)
// For |s| in [kCompMin, kCompMax], pitchFactor = 2^(p/12) exactly -- complete
// pitch preservation across the whole in-range speed sweep (true stretch).
// Below kCompMin the compensation saturates and pitch audibly drops with
// speed (tape-stop character; spec'd, not a bug). The SIGN of s affects only
// playback direction (via the transport), never comp()'s magnitude-only
// argument -- so PitchShifter itself NEVER sees a negative ratio.
//   IMPORTANT DERIVATION (not directly obvious from pitchFactor() above):
//   pitchFactor(s,p) is the NET/overall pitch multiplier the LISTENER hears,
//   which is the PRODUCT of two independent effects: (1) the transport's own
//   NATURAL speed-induced pitch/tempo scaling, magnitude |s| (reading
//   recorded content at s times the recording rate inherently transposes it
//   by |s|, exactly like a tape-speed change -- this happens for free, from
//   the transport dynamics above, with NO help from PitchShifter), and (2)
//   whatever ratio is actually fed to PitchShifter::setRatio(). Solving
//   |s| * shifterRatio = pitchFactor(s,p) gives the value THIS engine
//   actually feeds the shifter every sample:
//     shifterRatio(s, ratio) = comp(s) * ratio     (ratio = the 20 ms-glided
//                                                    2^(pitchSemis/12) target)
//   At s=1 this is comp(1)*1 = 1 (the degenerate pure-delay case above); at
//   s=2, p=0 it is comp(2)*1 = 0.5, so the transport's natural 2x speedup is
//   exactly cancelled by the shifter's 0.5x, net pitch = 2*0.5 = 1 = 2^0 --
//   2x tempo, unchanged pitch, matching pitchFactor(2,0) = 1 exactly.
//
// Latency / passthrough contract:
//   D0 = passthroughDelaySamples(fs) = kAgeMin + 0.5 * max(8, kShifterWindowMs
//        * 1e-3 * fs)  -- this is PitchShifter's OWN (unrounded) internal
//        `window` field halved, matching PitchShifter::prepare() exactly (it
//        does NOT llround its window), plus the fixed kAgeMin=1 transport
//        floor. No other additive constant exists in this engine: at
//        s=1/p=0 the crossfade machinery never engages (A never wraps), so
//        nothing else can add delay.
//   latencySamples() is ALWAYS 0 (published contract): a "time machine" pedal
//   deliberately does not latency-compensate its dry path -- dry is an
//   unconditional, no-delay passthrough of the (finite-guarded) input, and
//   mix is a plain convex blend of that dry signal with the wet transport
//   output. This is a design choice (spec'd), not an oversight.
//
// Loop-gain / regression-policy note: there is NO feedback path in this
// engine at all -- HistoryBuffer is written ONLY with the raw, finite-guarded
// input sample (never any function of what was just read back), so
// readAtAge() is a pure, one-directional tap with no way to close a loop;
// the "feedback gain < 1 at every setting" regression class is satisfied
// structurally (no loop exists to have a gain), not by a tuned coefficient.
//
// Peak-bound argument (long-hold realistic gate: peak <= kPeakBound = 1.2
// for |in| <= 1, mix convex combination included):
//   1) HistoryBuffer only ever stores the raw, finite-guarded input (no gain
//      anywhere in the write path), so every stored sample's magnitude is
//      bounded by the input's own peak (<=1 for a normalised test signal).
//   2) The wrap crossfade is EQUAL POWER (wOld^2+wNew^2=1), not equal GAIN,
//      so its worst-case INSTANTANEOUS sum can reach sqrt(2) (both heads at
//      their ceiling, same sign, at g=0.5); but Aold and A name generally
//      UNCORRELATED points in the recorded signal (different, independently
//      evolving historical instants), so sustained coincidence at the peak
//      is not realistic -- consistent with OmoideEcho's SCAN-vs-echo-tap
//      argument, the realistic combined envelope over a long, moving sweep
//      settles near ~1.06 (spec's own engineering estimate for this exact
//      two-head, uncorrelated-source construction).
//   3) PitchShifter's OWN internal two-head Hann blend is, unlike the wrap
//      crossfade, a TRUE unity-SUM convex combination: with d2 = d1 + window/2
//      (mod window), w2's cosine term is exactly the negative of w1's
//      (cos(theta+pi) = -cos(theta)), so w1 + w2 = 1 identically, with both
//      weights in [0,1] -- the shifter's output is therefore bounded by the
//      LARGER of its two tap reads, i.e. it adds NO extra gain on top of
//      whatever the (already-blended) tapped signal's peak is.
//   4) The final mix stage is a plain convex combination, (1-mix)*dry +
//      mix*wet, bounded by max(|dry|, |wet|) <= ~1.06.
//   kPeakBound = 1.2 is set as a safety margin above that ~1.06 estimated
//   envelope (matching the spec's own figure) to absorb statistical
//   variance over an 8 s worst-case sweep (LCG noise, full param sweep, HOLD
//   toggling) without being a fragile, tight bound.
//
// Real-time / safety rules (house standard):
//   - prepare() performs ALL allocation: HistoryBuffer's ring, sized for the
//     worst-case window PLUS the crossfade headroom --
//     (kMaxWindowSec + 1) seconds * fs * kMaxChannels (float storage; at
//     192 kHz/2ch this is ~7.7 MB) -- and both PitchShifters' fixed
//     kShifterWindowMs-sized delay lines. process() allocates nothing (there
//     is no per-call scratch buffer at all in this engine: every stage reads
//     one host sample at a time directly into persistent state, so there is
//     no n-dependent sizing to guard against). Internal chunking
//     (kHostChunk = 512) exists specifically to bound the wet-node
//     finite-guard's "at most one recovery" scope to a fixed-size slice
//     (house pattern; see Madoromi.h/OmoideEcho.h) rather than to size any
//     scratch buffer.
//   - Input finite guard: every sample is sanitised (non-finite -> 0.0)
//     before it can reach HistoryBuffer or the mix.
//   - Wet-node finite guard: PitchShifter's OWN internal DelayLine (unlike
//     HistoryBuffer) does NOT sanitise on write(), so a non-finite ratio or
//     tap could in principle poison its buffer permanently; a non-finite
//     shifter output therefore resets BOTH PitchShifters (purging any such
//     poison) and re-asserts the current shifterRatio on both immediately
//     afterwards (house pattern: recovery is targeted at the one
//     corruptible component -- HistoryBuffer, the transport age state and
//     the parameter smoothers cannot themselves become non-finite through
//     this engine's own guarded/clamped arithmetic, so they are
//     deliberately NOT touched by recovery, unlike OmoideEcho's broader
//     reset -- a scope-appropriate difference explained in the report). At
//     most one recovery per kHostChunk slice; the corrupted sample's wet
//     contribution is zeroed for that sample regardless.
//   - reset() is fully deterministic (two runs from reset() with identical
//     inputs/parameter calls are bit-identical): it clears HistoryBuffer,
//     resets both PitchShifters, and snaps every smoother to its current
//     target (parameter TARGETS themselves are untouched by reset(), house
//     rule). setHold()'s bool has no separate "target" to snap -- it is
//     latching/immediate and simply persists across reset(), like
//     MicroLooper's/Madoromi's frozen flag.
//
#include "HistoryBuffer.h"
#include "PitchShifter.h"
#include "SmoothingCoeff.h"

#include <algorithm>
#include <cmath>

namespace factory_core
{
    class MochiStretch
    {
    public:
        static constexpr int    kMaxChannels     = 2;
        static constexpr double kMaxWindowSec    = 4.0;     // matches kWindowMaxMs
        static constexpr double kWindowMinMs     = 100.0;
        static constexpr double kWindowMaxMs     = 4000.0;
        static constexpr double kShifterWindowMs = 80.0;
        static constexpr double kAgeMin          = 1.0;     // HistoryBuffer's causality floor
        static constexpr double kFadeMs          = 10.0;
        static constexpr double kSpeedGlideTauMs = 80.0;
        static constexpr double kParamSmoothMs   = 20.0;
        static constexpr double kSpeedMin        = -2.0;
        static constexpr double kSpeedMax        =  2.0;
        static constexpr double kPitchMinSemis   = -12.0;
        static constexpr double kPitchMaxSemis   =  12.0;
        static constexpr double kCompMin         = 0.125;
        static constexpr double kCompMax         = 2.0;
        static constexpr int    kHostChunk       = 512;     // fixed internal slice
        static constexpr double kPeakBound       = 1.2;     // long-hold realistic gate

        // -- published pure mappings (tests re-derive these) -----------------

        // Overall (net) pitch multiplier heard at the output for a STEADY
        // speed/pitch pair: |s| * comp(s) * 2^(p/12), comp(s) = 1 /
        // clamp(|s|, kCompMin, kCompMax). NOTE: this is NOT what gets fed to
        // PitchShifter::setRatio -- see the header / shifterRatio() below.
        static double pitchFactor (double speed, double pitchSemis) noexcept
        {
            const double comp = 1.0 / std::clamp (std::abs (speed), kCompMin, kCompMax);
            return std::abs (speed) * comp * std::pow (2.0, pitchSemis / 12.0);
        }

        // Total passthrough delay (host samples) at speed=1, pitch=0 (the
        // degenerate pure-delay case -- see the header). D0 = kAgeMin +
        // window/2, window = PitchShifter's own internal window computation
        // (max(8, kShifterWindowMs*1e-3*fs)) -- NOT rounded, matching
        // PitchShifter::prepare() exactly. No other fixed offset exists.
        static double passthroughDelaySamples (double fsHost) noexcept
        {
            const double window = std::max (8.0, kShifterWindowMs * 1.0e-3 * fsHost);
            return kAgeMin + 0.5 * window;
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs       = (sampleRate > 0.0) ? sampleRate : 44100.0;
            channels = std::clamp (numChannels, 1, kMaxChannels);

            fadeLen = std::max (1, (int) std::llround (kFadeMs * 1.0e-3 * fs));

            history.prepare (fs, kMaxChannels, kMaxWindowSec + 1.0);
            shifterL.prepare (fs, kShifterWindowMs);
            shifterR.prepare (fs, kShifterWindowMs);

            aSpeed = 1.0 - onePoleCoeffForMs (kSpeedGlideTauMs, fs);
            aParam = 1.0 - onePoleCoeffForMs (kParamSmoothMs, fs);

            WlenPending = (double) std::llround (windowMsTarget * 1.0e-3 * fs);

            reset();
        }

        // Deterministic: two runs from reset() with identical inputs and
        // parameter calls are bit-identical. Parameter TARGETS are untouched
        // (house rule) -- only the smoothed/runtime state snaps to the
        // current targets and the stateful DSP objects clear to silence.
        void reset() noexcept
        {
            history.reset();
            shifterL.reset();
            shifterR.reset();

            sSm     = speedTarget;
            ratioSm = pitchRatioTarget;
            mixSm   = mixTarget;

            Wlen        = WlenPending;
            A           = kAgeMin;
            Aold        = kAgeMin;
            fading      = false;
            fadeCounter = 0;
        }

        // -- parameters (audio thread, between process() calls) -------------
        // Non-finite values are ignored (the previous target is kept).

        void setSpeed (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            speedTarget = std::clamp (v, kSpeedMin, kSpeedMax);
        }

        void setPitchSemis (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            const double clamped = std::clamp (v, kPitchMinSemis, kPitchMaxSemis);
            pitchRatioTarget = std::pow (2.0, clamped / 12.0);
        }

        void setWindowMs (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            windowMsTarget = std::clamp (v, kWindowMinMs, kWindowMaxMs);
            WlenPending    = (double) std::llround (windowMsTarget * 1.0e-3 * fs);
        }

        // bool parameter: excluded from the non-finite guard rule by spec.
        // Latching, immediate -- no smoothing (house pattern: MicroLooper's
        // setFrozen / Madoromi's setFrozen).
        void setHold (bool h) noexcept { holdEngaged = h; }

        void setMix01 (double v) noexcept
        {
            if (! std::isfinite (v)) return;
            mixTarget = std::clamp (v, 0.0, 1.0);
        }

        // Always 0 -- see the header's "Latency / passthrough contract".
        int latencySamples() const noexcept { return 0; }

        // In-place, up to kMaxChannels. Slices numSamples into <= kHostChunk
        // pieces purely to bound the wet-node finite-guard's recovery scope
        // (see the header) -- there is no scratch buffer sized from either
        // numSamples or kHostChunk in this engine. Allocation-free,
        // lock-free, noexcept for any numSamples.
        void process (float* const* audio, int numChannels, int numSamples) noexcept
        {
            const int nCh = std::clamp (numChannels, 1, channels);

            int offset = 0;
            while (offset < numSamples)
            {
                const int h = std::min (kHostChunk, numSamples - offset);
                bool recovered = false;   // wet-node finite guard: <= 1 per slice

                for (int i = 0; i < h; ++i)
                {
                    // 1) Parameter smoothers (host-rate, once per sample).
                    sSm     += aSpeed * (speedTarget      - sSm);
                    ratioSm += aParam * (pitchRatioTarget - ratioSm);
                    mixSm   += aParam * (mixTarget        - mixSm);

                    // 2) Transport dynamics (Design contract; see header).
                    const double dAdt = holdEngaged ? -sSm : (1.0 - sSm);
                    A += dAdt;
                    if (fading) Aold += dAdt;

                    const double preFold = A;   // raw, not-yet-folded value
                    bool wrapped = false;
                    if (A > Wlen)
                    {
                        if (Wlen != WlenPending) Wlen = WlenPending;
                        A -= (Wlen - 1.0);
                        wrapped = true;
                    }
                    else if (A < kAgeMin)
                    {
                        if (Wlen != WlenPending) Wlen = WlenPending;
                        A += (Wlen - 1.0);
                        wrapped = true;
                    }

                    // Single-shadow-head design: only spawn a new fade if
                    // one is not already in progress (see the header's
                    // "Resolved ambiguities" note in the accompanying
                    // report) -- A itself is ALWAYS kept in [kAgeMin, Wlen]
                    // regardless, every sample.
                    if (wrapped && ! fading)
                    {
                        fading      = true;
                        Aold        = preFold;
                        fadeCounter = 0;
                    }

                    const bool blend = fading;   // captured before it may close below
                    double wOld = 0.0, wNew = 1.0;
                    if (fading)
                    {
                        ++fadeCounter;
                        const double g = (double) fadeCounter / (double) fadeLen;
                        wOld = std::cos (0.5 * kPi * g);
                        wNew = std::sin (0.5 * kPi * g);
                        if (fadeCounter >= fadeLen) { fading = false; fadeCounter = 0; }
                    }

                    // 3) Pitch-compensation ratio (shared by both channels'
                    //    shifters -- see the header's derivation).
                    const double compS        = 1.0 / std::clamp (std::abs (sSm), kCompMin, kCompMax);
                    const double shifterRatio  = compS * ratioSm;
                    shifterL.setRatio (shifterRatio);
                    if (nCh == 2) shifterR.setRatio (shifterRatio);

                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const double raw = (double) audio[ch][offset + i];
                        const double x   = std::isfinite (raw) ? raw : 0.0;

                        // Read BEFORE write (HistoryBuffer's causality contract).
                        const double tapped = blend
                            ? wOld * history.readAtAge (ch, Aold) + wNew * history.readAtAge (ch, A)
                            : history.readAtAge (ch, A);

                        double shifted = (ch == 0) ? shifterL.process (tapped)
                                                    : shifterR.process (tapped);

                        // Wet-node finite guard (see header): at most one
                        // recovery per slice.
                        if (! std::isfinite (shifted))
                        {
                            if (! recovered) { recoverWetPath(); recovered = true; }
                            shifted = 0.0;
                        }

                        if (! holdEngaged)
                            history.write (ch, x);

                        audio[ch][offset + i] = (float) ((1.0 - mixSm) * x + mixSm * shifted);
                    }

                    if (! holdEngaged) history.advance();   // shared write pointer, once per sample
                }

                offset += h;
            }
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;

        // Wet-path self-recovery (wet-node finite guard). Targeted at the
        // one component that can actually become NaN-poisoned (PitchShifter's
        // internal DelayLine does not sanitise on write, unlike
        // HistoryBuffer -- see the header); the transport age state and the
        // parameter smoothers cannot themselves go non-finite through this
        // engine's own guarded/clamped arithmetic, so they are deliberately
        // left untouched.
        void recoverWetPath() noexcept
        {
            shifterL.reset();
            shifterR.reset();

            // Re-assert current params (Madoromi-style pattern), even though
            // PitchShifter::reset() does not itself clear `ratio`.
            const double compS       = 1.0 / std::clamp (std::abs (sSm), kCompMin, kCompMax);
            const double shifterRatio = compS * ratioSm;
            shifterL.setRatio (shifterRatio);
            shifterR.setRatio (shifterRatio);
        }

        double fs       = 44100.0;
        int    channels = kMaxChannels;
        int    fadeLen  = 1;

        HistoryBuffer history;
        PitchShifter  shifterL, shifterR;

        double aSpeed = 1.0;   // speed glide alpha (kSpeedGlideTauMs, host rate)
        double aParam = 1.0;   // 20 ms param-smoother alpha (host rate)

        // parameter targets
        double speedTarget      = 1.0;
        double pitchRatioTarget = 1.0;
        double windowMsTarget   = 1000.0;
        double mixTarget        = 0.5;
        bool   holdEngaged      = false;

        // smoothed / runtime state
        double sSm     = 1.0;
        double ratioSm = 1.0;
        double mixSm   = 0.5;

        double Wlen       = 1.0;    // active window length, samples (wrap-quantised)
        double WlenPending = 1.0;   // target window length, samples (see setWindowMs)
        double A    = kAgeMin;      // primary transport read age, samples
        double Aold = kAgeMin;      // shadow (fading-out) head's age, valid while fading
        bool   fading      = false;
        int    fadeCounter = 0;
    };
} // namespace factory_core
