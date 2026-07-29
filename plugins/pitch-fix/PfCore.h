#pragma once
//
// plugins/pitch-fix/PfCore.h — pf_core::PfCore, the framework-free DSP core of
// Pitch TatFixer (real-time monophonic pitch correction). The CLAP shell's
// Policy (shell/ClapEntry.cpp) is a thin wrapper over this class; the headless
// dsp_test drives it directly. No JUCE, no CLAP, no allocation in process().
//
// SIGNAL PATH (per block)
//   in L/R ──┬─ mid sum → HP(minPitch) → detector ring ──(hop)── PitchDetector
//            │                                   │ acquisition / tracking window
//            │                                   │ median filter (mode depth)
//            │                                   │ scale quantiser + hysteresis
//            │                                   │ Stability deadzone + Accuracy
//            │                                   │ glide + retune one-poles
//            │                                   ▼
//            ├─ PsolaShifter (pitch-synchronous OLA, stereo phase-locked) ─ wet
//            └─ dry delay (== lookahead) ──────────────────────────────── dry
//   out = (wet*mix + dry*(1-mix)) * outputGain          (LinearRamp smoothed)
//
// DETECTION WINDOWS (docs/plans/pitch-fix-detection-accuracy.md §3 P1)
//   Acquisition uses kWindowPeriods[mode] periods of Min Pitch (the long window
//   that can hear the floor). Tracking shrinks to >= 6 periods of the tracked
//   f0 so lagMax = n/2 still reaches one octave below the track (anti lock-in).
//   Tracking periodically re-runs acquisition (~50 ms); a single disagreeing
//   candidate never switches the track — 2 consecutive confirms are required.
//   Unvoiced runs / clarity drops / large pitch jumps fall back to acquisition.
//
// ANALYSIS TIMEBASE (docs/plans/pitch-fix-detection-accuracy.md §3 P2)
//   The analysis window is centred on the grain-read time (written - L) plus
//   the median filter's group delay, so the f0 estimate applied to a grain is
//   the estimate of the same source time the grain is reading. No intentional
//   anticipation is baked into the analysis position.
//
// LOOKAHEAD / LATENCY — the Buffer parameter (Realtime/Fast/Normal/Quality)
// scales the lookahead in PERIODS OF THE MIN-PITCH PARAMETER, so the latency
// is always exactly what the correction structurally needs — no hidden clamp:
//   latency = round(kLookaheadPeriods[mode] * fs / minPitch)
// Higher modes buy a longer analysis window, deeper median filtering (octave-
// glitch suppression) and more anticipation of the output cursor. Latency is
// reported to the host (CLAP latency ext) and mirrored in uiLatencySamples for
// the editor. Changing Buffer or Min Pitch changes the reported latency; the
// shell then requests a host restart (same contract as RS's Quality switch).
//
// The correction shift is hard-clamped to ±1200 cents (PsolaShifter clamps the
// ratio to [0.5, 2] as well) — the worst-case buffer sizing is derived from
// exactly these bounds plus the parameter floors (min pitch >= 25 Hz).
//
#include "factory_core/Biquad.h"
#include "factory_core/Filters.h"
#include "factory_core/PitchDetector.h"
#include "factory_core/PsolaShifter.h"
#include "factory_core/LinearRamp.h"
#include "factory_core/SmoothingCoeff.h"
#include "PfNote.h"
#include "PfCorrection.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pf_core
{
    // Per-block parameter snapshot, in REAL units (the shell fills it from the
    // ParamStore; the tests construct it directly).
    struct PfParamSnapshot
    {
        float amount        = 100.0f;  // %   (0..150)
        float retuneMs      = 80.0f;   // ms  (0..600)
        float glideMs       = 60.0f;   // ms  (0..750)
        float stabilityCt   = 12.0f;   // cents deadzone / Stability (0..75); wire id "tolerance"
        float accuracyCt    = 0.0f;    // cents residual Accuracy (0..75); new instances default 0
        float hysteresisCt  = 18.0f;   // cents note-switch margin (0..75)
        float minPitchHz    = 75.0f;   // Hz  (25..500)
        float maxPitchHz    = 1300.0f; // Hz  (200..4000)
        float thresholdPct  = 80.0f;   // %   detector clarity threshold (50..99)
        int   buffer        = 2;       // 0 Realtime / 1 Fast / 2 Normal / 3 Quality
        int   key           = 0;       // 0 = C .. 11 = B
        int   scale         = 0;       // 0 Chromatic / 1 Major / 2 Minor
        float a4Hz          = 440.0f;  // Hz  (400..480)
        float mixPct        = 100.0f;  // %
        float outDb         = 0.0f;    // dB  (-24..+24)
    };

    class PfCore
    {
    public:
        // --- buffer-mode tables (THE latency spec; dsp_test re-derives these) ---
        static constexpr double kLookaheadPeriods[4] = { 2.35, 2.75, 3.5, 5.0 };
        static constexpr double kWindowPeriods[4]    = { 2.0,  2.4,  2.8, 3.2 };
        static constexpr int    kMedianDepth[4]      = { 1,    3,    5,   7   };
        static constexpr double kHopSeconds[4]       = { 0.008, 0.006, 0.005, 0.0035 };

        static constexpr double kMinPitchFloorHz = 25.0;   // parameter floor
        static constexpr double kUnvoicedReleaseMs = 60.0; // correction release
        static constexpr double kTargetHoldSec     = 0.4;  // note memory across gaps
        static constexpr double kMaxShiftCents     = 1200.0;

        // Tracking window: >= 6 periods of tracked f0 so lagMax = n/2 reaches one
        // octave below the track (4 periods sat exactly on the octave boundary and
        // locked in — see pitch-fix-detection-accuracy.md §2.6-1).
        static constexpr double kTrackingMinPeriods   = 6.0;
        static constexpr double kAcqIntervalSec        = 0.050; // periodic full-band reacquire
        static constexpr int    kReacqConfirmNeeded    = 2;     // consecutive agreeing candidates
        static constexpr int    kTrackEnterStableHops  = 3;     // median+clarity settle before track
        static constexpr double kPitchJumpRatio        = 1.414; // ~half octave → fall back to acq
        static constexpr double kReacqMatchCents       = 50.0;  // within this = same candidate

        // P3 trajectory (causal, within existing latency — no extra lookahead):
        // keep a short history of NSDF candidates and pick the path that maximises
        // clarity while paying continuity + near-exact-octave jump costs.
        // Harmonic-support scoring is intentionally absent (§2.1).
        static constexpr int    kTrajCand              = 8;
        static constexpr int    kTrajHist              = 5;
        static constexpr double kTrajContPerOctave     = 1.5;   // |log2| cost weight
        static constexpr double kTrajOctaveBump        = 0.75;  // extra near ±1/±2 octaves
        static constexpr double kTrajOctaveWindowCt    = 80.0;  // "near" = within this of k*1200
        static constexpr double kTrajEmitClarity       = 1.0;   // emission = 1 - clarity

        // The PSOLA budget: PsolaShifter demands 2*P + P/4 + 4 <= lookahead for a
        // VOICED grain and degrades to its identity path otherwise — silently, so
        // the pitch would still read out in the UI while nothing gets corrected.
        // The lookahead table is written in periods of Min Pitch, so every mode's
        // entry clears 2.25 only while the tracked period stays <= fs/minPitch.
        // Today PitchDetector's lag search is itself capped at fs/minHz, so that
        // already holds; runHop bounds the tracked pitch at minHz anyway so the
        // guarantee is PfCore's own rather than a side effect of a shared header's
        // internal clamp (PitchDetector's documented acceptance is 0.9*minHz, which
        // would NOT clear the budget at Realtime). dsp_test asserts the invariant.
        static constexpr double kPsolaBudgetPeriods = 2.25;

        void prepare (double sampleRate, int maxBlockIn)
        {
            fs       = sampleRate;
            maxBlock = std::max (16, maxBlockIn);

            const int maxPeriod = (int) std::ceil (fs / kMinPitchFloorHz) + 4;
            const int maxLook   = (int) std::ceil (kLookaheadPeriods[3] * fs / kMinPitchFloorHz) + 8;
            // Tracking windows need >= 6 periods of the lowest trackable f0, which
            // can EXCEED the acquisition window (acquisition is only ~3.2 periods of
            // Min Pitch). Size the detector / scratch / ring for both.
            const double maxWinPeriods = std::max (kWindowPeriods[3] + 0.3, kTrackingMinPeriods);
            maxWin = (int) std::ceil (maxWinPeriods * fs / kMinPitchFloorHz) + 2;
            // Median group-delay compensation can push the analysis window start
            // back by up to ((7-1)/2) * hop samples past the grain-read cursor.
            const int maxMedDelay = 3 * (int) std::ceil (kHopSeconds[0] * fs) + 8;

            detector.prepare (fs, kMinPitchFloorHz, maxWinPeriods);
            shifter.prepare (fs, maxBlock, maxLook, maxPeriod);

            // P2: window may be centred near written - L, so the ring must hold
            // lookahead + window + median delay behind the write cursor.
            detSize = nextPow2 (maxLook + maxWin + maxMedDelay + maxBlock + 8);
            detMask = detSize - 1;
            detRing.assign ((size_t) detSize, 0.0f);
            scratch.assign ((size_t) maxWin, 0.0f);
            scratchAcq.assign ((size_t) maxWin, 0.0f);

            drySize = nextPow2 (maxLook + maxBlock + 8);
            dryMask = drySize - 1;
            dryL.assign ((size_t) drySize, 0.0f);
            dryR.assign ((size_t) drySize, 0.0f);

            written = 0;
            detHp[0].reset();
            detHp[1].reset();
            hpDesignedHz = 0.0;          // force a design on the first snapshot
            mixRamp.reset (fs, 0.02);
            gainRamp.reset (fs, 0.02);
            mixRamp.setCurrentAndTargetValue (1.0);
            gainRamp.setCurrentAndTargetValue (1.0);

            // Latency latching: prepare() defers the FIRST lookahead latch to the
            // first process() (activate() primes silently, so the settled value is
            // reported before the host queries it). committedLookahead is what the
            // DSP path actually delays by; pendingLookahead is what we REPORT — the
            // two split so a mid-stream Buffer/Min-Pitch change never desyncs the
            // host's delay compensation from the real DSP delay (see applySnapshot).
            committedLookahead = 0;
            pendingLookahead   = 0;
            needsCommit        = true;
            hopCounter  = 0;
            prevKey = -1; prevScale = -1; prevMedLen = -1;
            resetTracking();
            uiSampleRateHz.store ((float) fs, std::memory_order_relaxed);
        }

        // The latency REPORTED to the host. It reflects the live parameters, so a
        // mid-stream change moves it and the shell requests a restart; the actual
        // DSP delay only follows on the next activate() (see committedLookahead).
        int latencySamples() const noexcept { return pendingLookahead; }

        // Clear all audio + tracking state WITHOUT reallocating or changing the
        // latency — for the CLAP shell to call on a transport discontinuity (seek,
        // loop wrap) so stale audio / detector history / correction state do not
        // bleed across the jump. Real-time safe (std::fill + scalar resets only).
        void reset() noexcept
        {
            std::fill (detRing.begin(), detRing.end(), 0.0f);
            std::fill (dryL.begin(),    dryL.end(),    0.0f);
            std::fill (dryR.begin(),    dryR.end(),    0.0f);
            std::fill (scratch.begin(), scratch.end(), 0.0f);
            std::fill (scratchAcq.begin(), scratchAcq.end(), 0.0f);
            shifter.reset();
            detHp[0].reset();            // state only — the design follows minHz
            detHp[1].reset();
            written    = 0;
            hopCounter = 0;
            resetTracking();
            // Latency (committed/pending), winLen/hopLen/medLen and the live params
            // are preserved: a reset re-zeroes buffers, it does not re-latch latency.
            mixRamp.setCurrentAndTargetValue (mixRamp.getTargetValue());
            gainRamp.setCurrentAndTargetValue (gainRamp.getTargetValue());
        }

        // In-place stereo processing (R may be null). Snapshot applied at block
        // granularity (last write per block wins), matching the shell contract.
        //
        // BLOCK-SIZE INVARIANCE: the block is cut at internal HOP boundaries, and
        // the pitch-synchronous shifter runs per span with the track state set at
        // the hop that opens it — so the correction timeline is independent of the
        // host block boundaries, driven by the fixed hop grid rather than by where
        // the host's blocks happen to fall. Output stays numerically equivalent
        // within the regression tolerance across any block size (not bit-identical:
        // a grain straddling a host-block boundary can reorder a few FP adds — see
        // the block-invariance test's tolerance).
        void process (float* L, float* R, int n, const PfParamSnapshot& snap) noexcept
        {
            if (L == nullptr || n <= 0 || fs <= 0.0)
                return;

            applySnapshot (snap);

            int done = 0;
            while (done < n)
            {
                // Span = up to the next hop mark, the block end, or maxBlock.
                const int untilHop = hopLen - hopCounter;      // > 0 (hopCounter < hopLen)
                int m = std::min (n - done, untilHop);
                m = std::min (m, maxBlock);
                if (m <= 0) m = std::min (n - done, maxBlock); // defensive; untilHop is > 0

                processSpan (L + done, R != nullptr ? R + done : nullptr, m);

                done       += m;
                hopCounter += m;
                if (hopCounter >= hopLen)                       // reached a hop boundary
                {
                    hopCounter -= hopLen;                      // == 0 (m <= untilHop)
                    runHop();
                }
            }
        }

        // --- editor / shell feed (lock-free, any thread) -----------------------
        std::atomic<float> uiSampleRateHz { 48000.0f };
        std::atomic<float> uiDetectedHz   { 0.0f };
        std::atomic<float> uiTargetHz     { 0.0f };
        std::atomic<float> uiShiftCents   { 0.0f };
        std::atomic<int>   uiLatencySamples { 0 };

        // Contract-test / diagnostics readouts (same lock-free contract as ui*).
        // detMode: 0 = acquisition, 1 = tracking.
        std::atomic<int>   uiDetMode       { 0 };
        std::atomic<int>   uiWinLen        { 0 };
        std::atomic<int>   uiAcqRunCount   { 0 };
        std::atomic<int>   uiReacqStreak   { 0 };
        std::atomic<float> uiTrackHz       { 0.0f };
        std::atomic<float> uiLastClarity   { 0.0f };

    private:
        static int nextPow2 (int v) noexcept
        {
            int p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        void resetTracking() noexcept
        {
            medCount = 0;
            medPos   = 0;
            std::fill (std::begin (medBuf), std::end (medBuf), 0.0);
            corrCents      = 0.0;
            glidedCents    = 0.0;
            targetNote     = -1;
            unvoicedHops   = 0;
            acqUnvoicedHops = 0;
            targetHeldHops = 0;
            detMode        = DetMode::Acquisition;
            trackF0Hz      = 0.0;
            stableTrackHops = 0;
            samplesSinceAcq = 0;
            reacqStreak    = 0;
            reacqCandidateHz = 0.0;
            lastClarity    = 0.0;
            pendingReacqF0 = 0.0;
            winLen         = 0;
            trajHistCount  = 0;
            trajHistPos    = 0;
            for (int h = 0; h < kTrajHist; ++h)
                trajN[h] = 0;
            publishDetDiag();
            uiDetectedHz.store (0.0f, std::memory_order_relaxed);
            uiTargetHz.store (0.0f, std::memory_order_relaxed);
            uiShiftCents.store (0.0f, std::memory_order_relaxed);
        }

        void publishDetDiag() noexcept
        {
            uiDetMode.store (detMode == DetMode::Tracking ? 1 : 0, std::memory_order_relaxed);
            uiWinLen.store (winLen, std::memory_order_relaxed);
            uiReacqStreak.store (reacqStreak, std::memory_order_relaxed);
            uiTrackHz.store ((float) trackF0Hz, std::memory_order_relaxed);
            uiLastClarity.store ((float) lastClarity, std::memory_order_relaxed);
        }

        void applySnapshot (const PfParamSnapshot& s) noexcept
        {
            amount   = std::clamp ((double) s.amount, 0.0, 150.0) * 0.01;
            retuneMs = std::clamp ((double) s.retuneMs, 0.0, 600.0);
            glideMs  = std::clamp ((double) s.glideMs, 0.0, 750.0);
            stabilityCt = std::clamp ((double) s.stabilityCt, 0.0, 75.0);
            accuracyCt  = std::clamp ((double) s.accuracyCt, 0.0, 75.0);
            hystCt   = std::clamp ((double) s.hysteresisCt, 0.0, 75.0);
            minHz    = std::clamp ((double) s.minPitchHz, kMinPitchFloorHz, 500.0);
            // Respect the user's Max Pitch; only guarantee it sits a little above
            // Min Pitch so the detector has a non-empty lag range. (Previously this
            // forced max >= 2*min, silently widening the detection band well past
            // the UI value — Min 300 / Max 400 actually searched to 600 Hz.)
            maxHz    = std::clamp (std::max ((double) s.maxPitchHz, minHz * 1.1), 200.0, 4000.0);
            thresh   = std::clamp ((double) s.thresholdPct, 50.0, 99.0) * 0.01;
            mode     = std::clamp (s.buffer, 0, 3);
            key      = std::clamp (s.key, 0, 11);
            scale    = std::clamp (s.scale, 0, 2);
            a4       = std::clamp ((double) s.a4Hz, 400.0, 480.0);

            mixRamp.setTargetValue (std::clamp ((double) s.mixPct, 0.0, 100.0) * 0.01);
            gainRamp.setTargetValue (std::pow (10.0, std::clamp ((double) s.outDb, -24.0, 24.0) / 20.0));

            acqWinLen = std::min ((int) std::lround (kWindowPeriods[mode] * fs / minHz), maxWin);
            hopLen = std::max (32, (int) std::lround (kHopSeconds[mode] * fs));
            medLen = kMedianDepth[mode];
            acqIntervalSamples = std::max (hopLen, (int) std::lround (kAcqIntervalSec * fs));
            if (hopCounter >= hopLen) hopCounter = 0;   // keep hopCounter < hopLen if hopLen shrank

            // The median window is a ring of medLen entries; a Buffer change that
            // SHRINKS the depth would otherwise leave medCount > medLen, so the
            // majority vote below would keep counting entries that are no longer
            // being refreshed (a deep-mode history voting on a shallow-mode window).
            if (medLen != prevMedLen)
            {
                std::fill (std::begin (medBuf), std::end (medBuf), 0.0);
                medCount   = 0;
                medPos     = 0;
                prevMedLen = medLen;
                // Depth change invalidates the "stable enough to track" count.
                stableTrackHops = 0;
                if (detMode == DetMode::Tracking)
                    enterAcquisition();
            }

            // Analysis-path high-pass, DETECTOR FEED ONLY (the audio that reaches
            // the output comes from the dry ring and the shifter's own rings, so
            // this cannot colour the signal).
            //
            // A male fundamental sits at 80-160 Hz, so the source is never
            // high-passed hard and proximity build-up, plosive thumps and stage
            // rumble ride along underneath it. That sub-fundamental energy does
            // not merely bias the NSDF — a component whose period exceeds the
            // whole lag range makes the NSDF decay monotonically across
            // [lagMin, lagMax], leaving MPM with NO local maximum to pick, so the
            // frame comes back UNVOICED. Measured on a 110 Hz vowel with 40 Hz
            // rumble: 12 dB below the voice already loses 7.6% of frames, 6 dB
            // below loses 71%. That is the "it doesn't hear my voice" failure.
            //
            // The detector never searches below Min Pitch, so everything under it
            // is noise by definition — hence the corner sits exactly at minHz
            // (4th-order Butterworth). Measured at 6 dB rumble, 110 Hz: 71% of
            // frames rejected unfiltered vs 0% at this corner.
            if (minHz != hpDesignedHz)
            {
                // Transcendentals only — no allocation, lock or syscall — and only
                // when Min Pitch actually moves.
                for (int i = 0; i < 2; ++i)
                    detHp[i].setCoeffs (factory_core::designHpLpStage (
                        factory_core::BandType::HighPass, minHz, kButterQ, i, 2, fs));
                hpDesignedHz = minHz;
            }

            // Key/Scale change: drop a target note that the NEW mask forbids, so a
            // held note in the old scale is never kept as a correction target once
            // it becomes an out-of-scale pitch (targetNote < 0 -> fresh pick, no
            // hysteresis, at the next voiced hop).
            if ((key != prevKey || scale != prevScale) && targetNote >= 0
                && ! noteAllowed (targetNote, key, scale))
                targetNote = -1;
            prevKey = key; prevScale = scale;

            const int wantLook = (int) std::lround (kLookaheadPeriods[mode] * fs / minHz);
            if (needsCommit)
            {
                // First snapshot after prepare(): latch the ACTUAL DSP latency. The
                // shell primes silently before latching what it reports, so committed
                // == pending == the settled value here (host compensation matches).
                shifter.setLookahead (wantLook);
                committedLookahead = shifter.latencySamples();  // adopt the shifter's clamp, if any
                pendingLookahead   = committedLookahead;
                needsCommit        = false;
                resetTracking();
                hopCounter = 0;
            }
            else
            {
                // Live change: move ONLY the reported latency. The shell sees it
                // differ from what it announced and asks the host to restart; the
                // real DSP delay stays at committedLookahead until that restart
                // re-primes, so host delay-compensation and DSP delay never diverge.
                pendingLookahead = wantLook;
            }
            uiLatencySamples.store (pendingLookahead, std::memory_order_relaxed);
        }

        void enterAcquisition() noexcept
        {
            detMode = DetMode::Acquisition;
            stableTrackHops = 0;
            reacqStreak = 0;
            reacqCandidateHz = 0.0;
            // Keep trackF0Hz as a hint for diagnostics; the next voiced estimate
            // re-seeds it. samplesSinceAcq resets so the next hop runs a full acq.
            samplesSinceAcq = acqIntervalSamples;
            // Drop trajectory history so a harmonic lock cannot keep voting
            // through the acquisition restart.
            trajHistCount = 0;
            trajHistPos = 0;
            for (int h = 0; h < kTrajHist; ++h)
                trajN[h] = 0;
        }

        int trackingWindowLen (double f0) const noexcept
        {
            if (! (f0 > 0.0) || fs <= 0.0)
                return acqWinLen;
            // ceil so rounding never undershoots the 6-period contract.
            const int want = (int) std::ceil (kTrackingMinPeriods * fs / f0 - 1.0e-12);
            // Do NOT clamp to acqWinLen: at low tracked f0 (e.g. 110 Hz with
            // Min Pitch 75 / Quality) 6 periods exceeds the acquisition window
            // (~3.2 periods of Min Pitch). Clamping there was the lock-in margin
            // we just added — only the prepare() budget (maxWin) is the ceiling.
            return std::clamp (want, 16, maxWin);
        }

        // Assemble W samples centred on the grain-read time, advanced by the
        // median group delay so the post-median f0 lines up with that grain.
        // When medDelay > lookahead (high Min Pitch / shallow L), the ideal
        // centre would sit PAST the write cursor — clamp so the window never
        // reads unwritten / wrap-around-stale samples.
        void fillAnalysisWindow (float* dst, int W) noexcept
        {
            const int medDelay = ((medLen - 1) / 2) * hopLen;
            std::int64_t center = written
                                - (std::int64_t) committedLookahead
                                + (std::int64_t) medDelay;
            // Window end must be <= written (exclusive of the next write slot).
            const std::int64_t maxCenter = written - (std::int64_t) (W / 2);
            if (center > maxCenter)
                center = maxCenter;
            const std::int64_t start = center - (std::int64_t) (W / 2);
            for (int i = 0; i < W; ++i)
            {
                const std::int64_t t = start + i;
                dst[(size_t) i] = t >= 0 ? detRing[(size_t) (t & detMask)] : 0.0f;
            }
        }

        // Process one span [start, start+m) whose track state was fixed by the hop
        // that opened it (the caller fires the closing hop). No hop firing here, so
        // a host-block boundary that lands inside a hop interval leaves the track
        // state unchanged across the split — the correction timeline is unaffected,
        // and the shifter output stays numerically equivalent within tolerance.
        void processSpan (float* L, float* R, int m) noexcept
        {
            // 1) Feed the analysis + dry rings.
            for (int i = 0; i < m; ++i)
            {
                const float l = L[i];
                const float r = R != nullptr ? R[i] : l;
                const double mid = detHp[1].processSample (
                                     detHp[0].processSample (0.5 * ((double) l + (double) r)));
                detRing[(size_t) (written & detMask)] = (float) mid;
                dryL[(size_t) (written & dryMask)] = l;
                dryR[(size_t) (written & dryMask)] = r;
                ++written;
            }

            // 2) Wet: pitch-synchronous resynthesis (in place).
            shifter.process (L, R, m);

            // 3) Dry mix + output gain (both ramped). The dry delay uses the
            //    COMMITTED lookahead so dry and wet stay time-aligned at the latency
            //    the host is compensating for.
            for (int i = 0; i < m; ++i)
            {
                const std::int64_t t = written - m + i;
                const std::int64_t d = t - (std::int64_t) committedLookahead;
                const float dl = d >= 0 ? dryL[(size_t) (d & dryMask)] : 0.0f;
                const float dr = d >= 0 ? dryR[(size_t) (d & dryMask)] : 0.0f;
                const double mix = mixRamp.getNextValue();
                const double g   = gainRamp.getNextValue();
                L[i] = (float) ((L[i] * mix + dl * (1.0 - mix)) * g);
                if (R != nullptr)
                    R[i] = (float) ((R[i] * mix + dr * (1.0 - mix)) * g);
            }
        }

        // Continuity cost between two f0s: |octaves| plus a bump when the jump
        // lands near an exact octave (the harmonic-lock signature). Does NOT
        // reward harmonic relatedness — that has no discriminatory power (§2.1).
        static double trajTransitionCost (double fromHz, double toHz) noexcept
        {
            if (! (fromHz > 0.0) || ! (toHz > 0.0))
                return 4.0; // heavy: connecting through an unvoiced gap
            const double cents = std::abs (1200.0 * std::log2 (toHz / fromHz));
            double cost = kTrajContPerOctave * (cents / 1200.0);
            for (int k = 1; k <= 2; ++k)
            {
                const double oct = 1200.0 * (double) k;
                if (std::abs (cents - oct) <= kTrajOctaveWindowCt)
                    cost += kTrajOctaveBump;
            }
            return cost;
        }

        // Push this hop's candidates into the history ring and pick the f0 that
        // ends the minimum-cost path over the retained hops (causal Viterbi).
        // Returns false when no usable candidate exists (treat as unvoiced).
        //
        // Candidate gate (load-bearing): a pure tone has perfect NSDF peaks at
        // every sub-multiple (f0/2, f0/3, …) with clarity ≈ 1. Flooding the
        // trajectory with those locks onto a random subharmonic. We therefore
        // keep ONLY (a) the MPM pick (first peak ≥ kPeakRatio * best) and
        // (b) peaks within a continuity band of the current track — never the
        // whole subharmonic comb. Harmonic-support scoring stays forbidden.
        bool selectTrajectory (const factory_core::PitchCandidate* cands, int nCand,
                               double clarityGate,
                               double& outF0, double& outClarity) noexcept
        {
            outF0 = 0.0;
            outClarity = 0.0;
            if (cands == nullptr || nCand <= 0)
            {
                trajN[trajHistPos] = 0;
                trajHistPos = (trajHistPos + 1) % kTrajHist;
                if (trajHistCount < kTrajHist) ++trajHistCount;
                return false;
            }

            double bestVal = 0.0;
            for (int i = 0; i < nCand; ++i)
                bestVal = std::max (bestVal, cands[i].clarity);

            // MPM pick: first (shortest-lag) candidate reaching kPeakRatio * best.
            int mpm = -1;
            for (int i = 0; i < nCand; ++i)
                if (cands[i].clarity >= factory_core::PitchDetector::kPeakRatio * bestVal
                    && cands[i].f0Hz >= minHz)
                { mpm = i; break; }

            int kept = 0;
            auto push = [&] (int i) noexcept
            {
                if (i < 0 || kept >= kTrajCand) return;
                for (int k = 0; k < kept; ++k)
                    if (std::abs (1200.0 * std::log2 (trajF0[trajHistPos][k] / cands[i].f0Hz)) < 5.0)
                        return; // already have this peak
                trajF0[trajHistPos][kept] = cands[i].f0Hz;
                trajCl[trajHistPos][kept] = cands[i].clarity;
                ++kept;
            };

            if (mpm >= 0)
                push (mpm);

            // Near-track alternatives (the residual harmonic-error case): allow a
            // lower-clarity peak that continues the track when MPM jumped away.
            if (trackF0Hz > 0.0)
            {
                for (int i = 0; i < nCand; ++i)
                {
                    if (cands[i].f0Hz < minHz || cands[i].clarity < clarityGate * 0.85)
                        continue;
                    const double ct = std::abs (1200.0 * std::log2 (cands[i].f0Hz / trackF0Hz));
                    if (ct <= 250.0) // ~quarter-octave continuity band
                        push (i);
                }
            }

            trajN[trajHistPos] = kept;
            const int curSlot = trajHistPos;
            trajHistPos = (trajHistPos + 1) % kTrajHist;
            if (trajHistCount < kTrajHist) ++trajHistCount;
            if (kept == 0)
                return false;

            // Single candidate → trivial path (cold-start / MPM-only frames).
            if (kept == 1)
            {
                outF0 = trajF0[curSlot][0];
                outClarity = trajCl[curSlot][0];
                return outF0 >= minHz && outClarity >= clarityGate * 0.85;
            }

            // Forward DP over the occupied history (oldest → current).
            const int H = trajHistCount;
            double dp[kTrajHist][kTrajCand];

            auto slotAt = [&] (int chron) noexcept -> int
            {
                return (curSlot - (H - 1 - chron) + kTrajHist * 4) % kTrajHist;
            };

            int start = 0;
            while (start < H && trajN[slotAt (start)] == 0)
                ++start;
            if (start >= H)
                return false;

            {
                const int s0 = slotAt (start);
                for (int j = 0; j < trajN[s0]; ++j)
                    dp[start][j] = kTrajEmitClarity * (1.0 - trajCl[s0][j]);
            }

            for (int h = start + 1; h < H; ++h)
            {
                const int s = slotAt (h);
                const int n = trajN[s];
                if (n == 0)
                {
                    // Hole in history: restart the chain after it.
                    start = h + 1;
                    while (start < H && trajN[slotAt (start)] == 0)
                        ++start;
                    if (start >= H)
                        return false;
                    const int s1 = slotAt (start);
                    for (int j = 0; j < trajN[s1]; ++j)
                        dp[start][j] = kTrajEmitClarity * (1.0 - trajCl[s1][j]);
                    h = start;
                    continue;
                }

                int prev = h - 1;
                while (prev >= start && trajN[slotAt (prev)] == 0)
                    --prev;
                if (prev < start)
                {
                    for (int j = 0; j < n; ++j)
                        dp[h][j] = kTrajEmitClarity * (1.0 - trajCl[s][j]);
                    continue;
                }

                const int sp = slotAt (prev);
                const int np = trajN[sp];
                for (int j = 0; j < n; ++j)
                {
                    double best = 1.0e300;
                    for (int i = 0; i < np; ++i)
                    {
                        const double cost = dp[prev][i]
                                          + trajTransitionCost (trajF0[sp][i], trajF0[s][j]);
                        if (cost < best) best = cost;
                    }
                    dp[h][j] = best + kTrajEmitClarity * (1.0 - trajCl[s][j]);
                }
            }

            const int sc = slotAt (H - 1);
            const int nc = trajN[sc];
            if (nc <= 0)
                return false;
            int bestJ = 0;
            for (int j = 1; j < nc; ++j)
                if (dp[H - 1][j] < dp[H - 1][bestJ])
                    bestJ = j;

            outF0 = trajF0[sc][bestJ];
            outClarity = trajCl[sc][bestJ];
            return outF0 >= minHz && outClarity >= clarityGate * 0.85;
        }

        // One detection/decision step (every hopLen samples). Real-time safe:
        // the detector runs on preallocated buffers, everything here is O(win).
        void runHop() noexcept
        {
            samplesSinceAcq += hopLen;

            // Primary analysis window: acquisition (Min-Pitch periods) or tracking
            // (>= 6 periods of tracked f0).
            const bool tracking = (detMode == DetMode::Tracking && trackF0Hz > 0.0);
            winLen = tracking ? trackingWindowLen (trackF0Hz) : acqWinLen;
            const int W = winLen;
            fillAnalysisWindow (scratch.data(), W);

            // P3: multi-candidate + causal trajectory (not the single-frame MPM pick).
            factory_core::PitchCandidate cands[kTrajCand];
            const int nCand = detector.estimateCandidates (
                scratch.data(), W, minHz, maxHz, cands, kTrajCand);

            double estF0 = 0.0, estClarity = 0.0;
            const bool trajOk = selectTrajectory (cands, nCand, thresh, estF0, estClarity);
            lastClarity = estClarity;

            // Keep the tracked pitch inside the band the PSOLA budget is sized
            // for (see kPsolaBudgetPeriods): PitchDetector's documented acceptance
            // reaches 0.9*minHz, which at Realtime would need 2.5 periods against
            // a 2.35-period lookahead — tracked and displayed, never corrected.
            const bool inRange = trajOk && estF0 >= minHz;

            // --- acquisition / tracking state machine -------------------------
            pendingReacqF0 = 0.0;
            if (! inRange)
            {
                // Unvoiced: fall back to acquisition after a short hold so
                // consonants do not thrash the mode every hop. (Separate from
                // unvoicedHops, which gates note-memory across longer gaps.)
                if (detMode == DetMode::Tracking)
                {
                    if (++acqUnvoicedHops * (double) hopLen > 0.030 * fs)
                        enterAcquisition();
                }
                else
                {
                    stableTrackHops = 0;
                    ++acqUnvoicedHops;
                }
            }
            else
            {
                acqUnvoicedHops = 0;

                // Large pitch jump while tracking → re-acquire (protects against
                // harmonic lock-in AND lets genuine leaps restart cleanly).
                if (detMode == DetMode::Tracking && trackF0Hz > 0.0)
                {
                    const double ratio = estF0 / trackF0Hz;
                    if (ratio > kPitchJumpRatio || ratio < 1.0 / kPitchJumpRatio)
                        enterAcquisition();
                }

                // Clarity collapse relative to the threshold → re-acquire.
                if (detMode == DetMode::Tracking && lastClarity < thresh * 0.85)
                    enterAcquisition();

                if (detMode == DetMode::Acquisition)
                {
                    ++stableTrackHops;
                    trackF0Hz = estF0; // seed / refresh the prospective track
                    if (stableTrackHops >= kTrackEnterStableHops)
                    {
                        detMode = DetMode::Tracking;
                        reacqStreak = 0;
                        reacqCandidateHz = 0.0;
                        samplesSinceAcq = 0; // start the periodic-acq clock fresh
                        // Adopt the tracking window on the same hop we enter, so
                        // diagnostics / the next hop's sizing aren't stuck on acq.
                        if (trackF0Hz > 0.0)
                            winLen = trackingWindowLen (trackF0Hz);
                    }
                }
                else if (detMode == DetMode::Tracking)
                {
                    trackF0Hz = estF0;
                }
            }

            // Periodic full-band acquisition while tracking (never switch on a
            // single disagreeing frame — need kReacqConfirmNeeded consecutive
            // agreeing candidates). Use trajectory over the acquisition-window
            // candidates so a lone harmonic MPM pick cannot force a switch.
            if (detMode == DetMode::Tracking && samplesSinceAcq >= acqIntervalSamples)
            {
                samplesSinceAcq = 0;
                uiAcqRunCount.fetch_add (1, std::memory_order_relaxed);

                fillAnalysisWindow (scratchAcq.data(), acqWinLen);
                factory_core::PitchCandidate acqCands[kTrajCand];
                const int nAcq = detector.estimateCandidates (
                    scratchAcq.data(), acqWinLen, minHz, maxHz, acqCands, kTrajCand);

                // Prefer the MPM pick of the acquisition window; also consider a
                // near-track alternative. Do not score the full subharmonic comb.
                double bestVal = 0.0;
                for (int i = 0; i < nAcq; ++i)
                    bestVal = std::max (bestVal, acqCands[i].clarity);
                double acqF0 = 0.0;
                for (int i = 0; i < nAcq; ++i)
                {
                    if (acqCands[i].clarity >= factory_core::PitchDetector::kPeakRatio * bestVal
                        && acqCands[i].f0Hz >= minHz
                        && acqCands[i].clarity >= thresh * 0.85)
                    { acqF0 = acqCands[i].f0Hz; break; }
                }
                // If MPM jumped an octave away from the track but a near-track
                // peak is still present, prefer that (continuity over MPM).
                if (trackF0Hz > 0.0 && acqF0 > 0.0)
                {
                    const double mpmCt = std::abs (1200.0 * std::log2 (acqF0 / trackF0Hz));
                    if (mpmCt > kReacqMatchCents)
                    {
                        for (int i = 0; i < nAcq; ++i)
                        {
                            if (acqCands[i].f0Hz < minHz
                                || acqCands[i].clarity < thresh * 0.85)
                                continue;
                            const double ct = std::abs (
                                1200.0 * std::log2 (acqCands[i].f0Hz / trackF0Hz));
                            if (ct <= kReacqMatchCents)
                            {
                                acqF0 = acqCands[i].f0Hz;
                                break;
                            }
                        }
                    }
                }

                if (acqF0 >= minHz && trackF0Hz > 0.0)
                {
                    const double cents = 1200.0 * std::log2 (acqF0 / trackF0Hz);
                    if (std::abs (cents) <= kReacqMatchCents)
                    {
                        // Agrees with the track — clear any pending reacquire.
                        reacqStreak = 0;
                        reacqCandidateHz = 0.0;
                    }
                    else
                    {
                        if (reacqCandidateHz > 0.0)
                        {
                            const double dc = 1200.0 * std::log2 (acqF0 / reacqCandidateHz);
                            if (std::abs (dc) <= kReacqMatchCents)
                                ++reacqStreak;
                            else
                            {
                                reacqStreak = 1;
                                reacqCandidateHz = acqF0;
                            }
                        }
                        else
                        {
                            reacqStreak = 1;
                            reacqCandidateHz = acqF0;
                        }

                        if (reacqStreak >= kReacqConfirmNeeded)
                        {
                            // Confirmed: adopt the acquisition candidate as the
                            // new track (true octave leaps included).
                            trackF0Hz = reacqCandidateHz;
                            pendingReacqF0 = reacqCandidateHz;
                            for (int i = 0; i < medLen; ++i)
                                medBuf[(size_t) i] = reacqCandidateHz;
                            medCount = medLen;
                            reacqStreak = 0;
                            reacqCandidateHz = 0.0;
                        }
                    }
                }
            }

            publishDetDiag();

            // Median over the last medLen hops (octave-glitch suppression; the
            // depth is the Buffer mode's quality lever).
            double medSample = inRange ? estF0 : 0.0;
            // After a confirmed reacquire, prefer the confirmed f0 so a stale
            // tracking-window harmonic estimate cannot vote the median back.
            if (pendingReacqF0 > 0.0)
                medSample = pendingReacqF0;
            medBuf[(size_t) medPos] = medSample;
            medPos = (medPos + 1) % medLen;
            if (medCount < medLen) ++medCount;

            double sorted[kMaxMedian];
            int nv = 0;
            for (int i = 0; i < medCount; ++i)
                if (medBuf[(size_t) i] > 0.0)
                    sorted[nv++] = medBuf[(size_t) i];
            const bool voiced = nv * 2 > medLen;   // majority of the window voiced
            double f0 = 0.0;
            if (voiced)
            {
                for (int i = 1; i < nv; ++i)      // insertion sort (nv <= 7)
                {
                    const double v = sorted[i];
                    int j = i - 1;
                    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
                    sorted[j + 1] = v;
                }
                f0 = sorted[nv / 2];
            }

            const double hopRate = fs / (double) hopLen;

            // Correction is DISABLED at amount 0: the plugin must then be a
            // transparent pure delay for ANY input, voiced included. We force the
            // shifter's unvoiced identity path (a near-exact pure delay within the
            // regression tolerance, ratio ignored) rather than running a ratio-1
            // voiced PSOLA, whose correlation re-alignment would perturb voiced
            // material. Detection still runs (for the UI read-out).
            const bool correcting = amount > 0.0;

            if (voiced)
            {
                unvoicedHops = 0;
                const double detCents = 1200.0 * std::log2 (f0 / a4);

                // -- scale quantiser with note hysteresis --
                const int cand = nearestAllowedNote (detCents, key, scale);
                if (targetNote < 0)
                {
                    targetNote     = cand;
                    glidedCents    = noteCents (targetNote); // fresh note: no stale glide
                    targetHeldHops = 0;                      // open the settling window
                }
                else
                {
                    if (cand != targetNote)
                    {
                        const double dCand = std::abs (detCents - noteCents (cand));
                        const double dCur  = std::abs (detCents - noteCents (targetNote));
                        if (targetHeldHops < settleHops() || dCand + hystCt < dCur)
                            targetNote = cand;
                    }
                    // Hops since the note was picked FRESH (not since the last
                    // switch), so an on-the-boundary pitch cannot hold the window
                    // open by flip-flopping.
                    ++targetHeldHops;
                }

                // -- note glide (portamento between targets) --
                const double cg = factory_core::onePoleCoeffForMs (glideMs, hopRate);
                glidedCents += (1.0 - cg) * (noteCents (targetNote) - glidedCents);

                // -- Stability deadzone + Accuracy residual + amount + retune --
                // Continuous form lives in PfCorrection.h (P4 correction stage).
                const double err = glidedCents - detCents;
                const double corrTarget = correctionTargetCents (
                    err, stabilityCt, accuracyCt, amount, kMaxShiftCents);
                const double cr = factory_core::onePoleCoeffForMs (retuneMs, hopRate);
                corrCents += (1.0 - cr) * (corrTarget - corrCents);

                shifter.setTrack (fs / f0, correcting);   // amount 0 -> unvoiced identity
                uiDetectedHz.store ((float) f0, std::memory_order_relaxed);
                uiTargetHz.store ((float) (a4 * std::exp2 (noteCents (targetNote) / 1200.0)),
                                  std::memory_order_relaxed);
            }
            else
            {
                // Correction releases toward unity; the note memory survives
                // short unvoiced gaps (consonants) but expires after the hold.
                const double cu = factory_core::onePoleCoeffForMs (kUnvoicedReleaseMs, hopRate);
                corrCents += (1.0 - cu) * (0.0 - corrCents);
                if (++unvoicedHops * (double) hopLen > kTargetHoldSec * fs)
                    targetNote = -1;
                shifter.setTrack (0.0, false);
                uiDetectedHz.store (0.0f, std::memory_order_relaxed);
            }

            shifter.setRatio (std::exp2 (corrCents / 1200.0));
            uiShiftCents.store ((float) (correcting ? corrCents : 0.0), std::memory_order_relaxed);
        }

        // Hops a freshly picked target may keep following the nearest-note
        // candidate before hysteresis takes over.
        //
        // Hysteresis defends the CURRENT target, but the first pick of a note is
        // made from the least reliable estimate there is: at the onset the
        // analysis window still straddles whatever came before, and the median is
        // still refilling. That pick is latched with NO hysteresis and then
        // defended by it, so one bad onset estimate owns the whole note — a tone
        // 49 cents sharp of A4 (so 51 flat of A#4) latched onto A#4 can never
        // leave, because defending it only costs 49 + 18 < 51.
        //
        // So the window must outlast the detector's OWN settling, not a magic
        // constant: the analysis window has to flush the transition (winLen) and
        // the median has to refill (medLen hops). Derived rather than tuned, it
        // scales with Buffer mode and sample rate for free. Past it, hysteresis
        // resumes, so vibrato crossing a semitone boundary is still held.
        int settleHops() const noexcept
        {
            const int W = winLen > 0 ? winLen : acqWinLen;
            return (hopLen > 0 ? (W + hopLen - 1) / hopLen : 0) + medLen;
        }

        static constexpr int    kMaxMedian = 7;
        static constexpr double kButterQ   = 0.70710678118654752440; // 1/sqrt(2)

        enum class DetMode : int { Acquisition = 0, Tracking = 1 };

        // --- composition ---
        factory_core::PitchDetector detector;
        factory_core::PsolaShifter  shifter;
        factory_core::Biquad        detHp[2];   // 4th-order Butterworth HP, analysis only
        factory_core::LinearRamp<double> mixRamp { 1.0 }, gainRamp { 1.0 };

        // --- config ---
        double fs = 0.0;
        int    maxBlock = 512;
        int    maxWin = 0;

        // --- rings ---
        std::vector<float> detRing, scratch, scratchAcq, dryL, dryR;
        int detSize = 0, drySize = 0;
        std::int64_t detMask = 0, dryMask = 0;
        std::int64_t written = 0;

        // --- live params (block-latched) ---
        double amount = 1.0, retuneMs = 80.0, glideMs = 60.0;
        double stabilityCt = 12.0, accuracyCt = 0.0, hystCt = 18.0;
        double minHz = 75.0, maxHz = 1300.0, thresh = 0.80, a4 = 440.0;
        int    mode = 2, key = 0, scale = 0;
        int    winLen = 0, acqWinLen = 0, hopLen = 512, medLen = 1;
        int    acqIntervalSamples = 1;
        double hpDesignedHz = 0.0;   // minHz the detHp cascade is currently designed for

        // --- latency latching (see applySnapshot / latencySamples / prepare) ---
        int    committedLookahead = 0;  // actual DSP delay (drives shifter + dry mix)
        int    pendingLookahead   = 0;  // REPORTED delay (follows live params)
        bool   needsCommit        = true;
        int    prevKey = -1, prevScale = -1;

        // --- tracking state ---
        int    hopCounter = 0;
        double medBuf[kMaxMedian] = {};
        int    medPos = 0, medCount = 0, prevMedLen = -1;
        double corrCents = 0.0, glidedCents = 0.0;
        int    targetNote = -1;
        std::int64_t unvoicedHops = 0;
        int    acqUnvoicedHops = 0;  // short hold before falling back to acquisition
        int    targetHeldHops = 0;   // voiced hops since targetNote was picked fresh

        // --- acquisition / tracking mode (§3 P1) ---
        DetMode detMode = DetMode::Acquisition;
        double  trackF0Hz = 0.0;
        int     stableTrackHops = 0;
        int     samplesSinceAcq = 0;
        int     reacqStreak = 0;
        double  reacqCandidateHz = 0.0;
        double  lastClarity = 0.0;
        double  pendingReacqF0 = 0.0; // confirmed reacquire f0 for this hop's median

        // --- P3 causal trajectory history ---
        double trajF0[kTrajHist][kTrajCand] = {};
        double trajCl[kTrajHist][kTrajCand] = {};
        int    trajN[kTrajHist] = {};
        int    trajHistPos = 0;
        int    trajHistCount = 0;
    };
} // namespace pf_core
