#pragma once
//
// plugins/pitch-fix/PfCore.h — pf_core::PfCore, the framework-free DSP core of
// Pitch TatFixer (real-time monophonic pitch correction). The CLAP shell's
// Policy (shell/ClapEntry.cpp) is a thin wrapper over this class; the headless
// dsp_test drives it directly. No JUCE, no CLAP, no allocation in process().
//
// SIGNAL PATH (per block)
//   in L/R ──┬─ mid sum → detector ring ──(every hop)── PitchDetector (MPM)
//            │                                   │ median filter (mode depth)
//            │                                   │ scale quantiser + hysteresis
//            │                                   │ tolerance deadzone → amount
//            │                                   │ glide + retune one-poles
//            │                                   ▼
//            ├─ PsolaShifter (pitch-synchronous OLA, stereo phase-locked) ─ wet
//            └─ dry delay (== lookahead) ──────────────────────────────── dry
//   out = (wet*mix + dry*(1-mix)) * outputGain          (LinearRamp smoothed)
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
#include "factory_core/PitchDetector.h"
#include "factory_core/PsolaShifter.h"
#include "factory_core/LinearRamp.h"
#include "factory_core/SmoothingCoeff.h"

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
        float toleranceCt   = 12.0f;   // cents deadzone (0..75)
        float hysteresisCt  = 18.0f;   // cents note-switch margin (0..75)
        float minPitchHz    = 75.0f;   // Hz  (25..500)
        float maxPitchHz    = 1300.0f; // Hz  (200..4000)
        float thresholdPct  = 86.0f;   // %   detector clarity threshold (50..99)
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

        void prepare (double sampleRate, int maxBlockIn)
        {
            fs       = sampleRate;
            maxBlock = std::max (16, maxBlockIn);

            const int maxPeriod = (int) std::ceil (fs / kMinPitchFloorHz) + 4;
            const int maxLook   = (int) std::ceil (kLookaheadPeriods[3] * fs / kMinPitchFloorHz) + 8;
            maxWin = (int) std::ceil ((kWindowPeriods[3] + 0.3) * fs / kMinPitchFloorHz) + 2;

            detector.prepare (fs, kMinPitchFloorHz, kWindowPeriods[3] + 0.3);
            shifter.prepare (fs, maxBlock, maxLook, maxPeriod);

            detSize = nextPow2 (maxWin + maxBlock + 8);
            detMask = detSize - 1;
            detRing.assign ((size_t) detSize, 0.0f);
            scratch.assign ((size_t) maxWin, 0.0f);

            drySize = nextPow2 (maxLook + maxBlock + 8);
            dryMask = drySize - 1;
            dryL.assign ((size_t) drySize, 0.0f);
            dryR.assign ((size_t) drySize, 0.0f);

            written = 0;
            mixRamp.reset (fs, 0.02);
            gainRamp.reset (fs, 0.02);
            mixRamp.setCurrentAndTargetValue (1.0);
            gainRamp.setCurrentAndTargetValue (1.0);

            lookahead   = 0;   // sentinel: first process() applies the snapshot
            hopCounter  = 0;
            resetTracking();
            uiSampleRateHz.store ((float) fs, std::memory_order_relaxed);
        }

        int latencySamples() const noexcept { return lookahead; }

        // In-place stereo processing (R may be null). Snapshot applied at block
        // granularity (last write per block wins), matching the shell contract.
        void process (float* L, float* R, int n, const PfParamSnapshot& snap) noexcept
        {
            if (L == nullptr || n <= 0 || fs <= 0.0)
                return;

            applySnapshot (snap);

            int done = 0;
            while (done < n)
            {
                const int m = std::min (n - done, maxBlock);
                processChunk (L + done, R != nullptr ? R + done : nullptr, m);
                done += m;
            }
        }

        // --- editor / shell feed (lock-free, any thread) -----------------------
        std::atomic<float> uiSampleRateHz { 48000.0f };
        std::atomic<float> uiDetectedHz   { 0.0f };
        std::atomic<float> uiTargetHz     { 0.0f };
        std::atomic<float> uiShiftCents   { 0.0f };
        std::atomic<int>   uiLatencySamples { 0 };

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
            uiDetectedHz.store (0.0f, std::memory_order_relaxed);
            uiTargetHz.store (0.0f, std::memory_order_relaxed);
            uiShiftCents.store (0.0f, std::memory_order_relaxed);
        }

        void applySnapshot (const PfParamSnapshot& s) noexcept
        {
            amount   = std::clamp ((double) s.amount, 0.0, 150.0) * 0.01;
            retuneMs = std::clamp ((double) s.retuneMs, 0.0, 600.0);
            glideMs  = std::clamp ((double) s.glideMs, 0.0, 750.0);
            tolCt    = std::clamp ((double) s.toleranceCt, 0.0, 75.0);
            hystCt   = std::clamp ((double) s.hysteresisCt, 0.0, 75.0);
            minHz    = std::clamp ((double) s.minPitchHz, kMinPitchFloorHz, 500.0);
            maxHz    = std::clamp (std::max ((double) s.maxPitchHz, minHz * 2.0), 200.0, 4000.0);
            thresh   = std::clamp ((double) s.thresholdPct, 50.0, 99.0) * 0.01;
            mode     = std::clamp (s.buffer, 0, 3);
            key      = std::clamp (s.key, 0, 11);
            scale    = std::clamp (s.scale, 0, 2);
            a4       = std::clamp ((double) s.a4Hz, 400.0, 480.0);

            mixRamp.setTargetValue (std::clamp ((double) s.mixPct, 0.0, 100.0) * 0.01);
            gainRamp.setTargetValue (std::pow (10.0, std::clamp ((double) s.outDb, -24.0, 24.0) / 20.0));

            winLen = std::min ((int) std::lround (kWindowPeriods[mode] * fs / minHz), maxWin);
            hopLen = std::max (32, (int) std::lround (kHopSeconds[mode] * fs));
            medLen = kMedianDepth[mode];

            const int wantLook = (int) std::lround (kLookaheadPeriods[mode] * fs / minHz);
            if (wantLook != lookahead)
            {
                lookahead = wantLook;
                shifter.setLookahead (lookahead);
                resetTracking();
                hopCounter = 0;
                uiLatencySamples.store (shifter.latencySamples(), std::memory_order_relaxed);
                lookahead = shifter.latencySamples();   // adopt the shifter's clamp, if any
            }
        }

        void processChunk (float* L, float* R, int m) noexcept
        {
            // 1) Feed the analysis + dry rings; run the detector on hop marks.
            for (int i = 0; i < m; ++i)
            {
                const float l = L[i];
                const float r = R != nullptr ? R[i] : l;
                const size_t w = (size_t) (written & detMask);
                detRing[w] = 0.5f * (l + r);
                const size_t dw = (size_t) (written & dryMask);
                dryL[dw] = l;
                dryR[dw] = r;
                ++written;
                if (++hopCounter >= hopLen)
                {
                    hopCounter = 0;
                    runHop();
                }
            }

            // 2) Wet: pitch-synchronous resynthesis (in place).
            shifter.process (L, R, m);

            // 3) Dry mix + output gain (both ramped — continuous params).
            for (int i = 0; i < m; ++i)
            {
                const std::int64_t t = written - m + i;
                const std::int64_t d = t - (std::int64_t) lookahead;
                const float dl = d >= 0 ? dryL[(size_t) (d & dryMask)] : 0.0f;
                const float dr = d >= 0 ? dryR[(size_t) (d & dryMask)] : 0.0f;
                const double mix = mixRamp.getNextValue();
                const double g   = gainRamp.getNextValue();
                L[i] = (float) ((L[i] * mix + dl * (1.0 - mix)) * g);
                if (R != nullptr)
                    R[i] = (float) ((R[i] * mix + dr * (1.0 - mix)) * g);
            }
        }

        // One detection/decision step (every hopLen samples). Real-time safe:
        // the detector runs on preallocated buffers, everything here is O(win).
        void runHop() noexcept
        {
            // Assemble the last winLen samples (ring → contiguous scratch).
            const int W = winLen;
            const std::int64_t start = written - (std::int64_t) W;
            for (int i = 0; i < W; ++i)
            {
                const std::int64_t t = start + i;
                scratch[(size_t) i] = t >= 0 ? detRing[(size_t) (t & detMask)] : 0.0f;
            }

            const auto est = detector.estimate (scratch.data(), W, minHz, maxHz, thresh);

            // Median over the last medLen hops (octave-glitch suppression; the
            // depth is the Buffer mode's quality lever).
            medBuf[(size_t) medPos] = est.voiced ? est.f0Hz : 0.0;
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

            if (voiced)
            {
                unvoicedHops = 0;
                const double detCents = 1200.0 * std::log2 (f0 / a4);

                // -- scale quantiser with note hysteresis --
                const int cand = nearestAllowedNote (detCents);
                if (targetNote < 0)
                {
                    targetNote  = cand;
                    glidedCents = noteCents (targetNote);   // fresh note: no stale glide
                }
                else if (cand != targetNote)
                {
                    const double dCand = std::abs (detCents - noteCents (cand));
                    const double dCur  = std::abs (detCents - noteCents (targetNote));
                    if (dCand + hystCt < dCur)
                        targetNote = cand;
                }

                // -- note glide (portamento between targets) --
                const double cg = factory_core::onePoleCoeffForMs (glideMs, hopRate);
                glidedCents += (1.0 - cg) * (noteCents (targetNote) - glidedCents);

                // -- tolerance deadzone + amount + retune smoothing --
                const double err  = glidedCents - detCents;
                double dead = 0.0;
                if (err >  tolCt) dead = err - tolCt;
                if (err < -tolCt) dead = err + tolCt;
                const double corrTarget =
                    std::clamp (dead * amount, -kMaxShiftCents, kMaxShiftCents);
                const double cr = factory_core::onePoleCoeffForMs (retuneMs, hopRate);
                corrCents += (1.0 - cr) * (corrTarget - corrCents);

                shifter.setTrack (fs / f0, true);
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
            uiShiftCents.store ((float) corrCents, std::memory_order_relaxed);
        }

        // Cents of a MIDI note relative to A4 (note 69).
        static double noteCents (int note) noexcept { return (note - 69) * 100.0; }

        // Nearest MIDI note whose pitch class is allowed by the key/scale mask.
        int nearestAllowedNote (double detCents) const noexcept
        {
            static constexpr int kMajor[12] = { 1,0,1,0,1,1,0,1,0,1,0,1 };
            static constexpr int kMinor[12] = { 1,0,1,1,0,1,0,1,1,0,1,0 };

            const double noteF = detCents / 100.0 + 69.0;
            const int nn = (int) std::lround (noteF);
            int    best  = nn;
            double bestD = 1.0e9;
            for (int cand = nn - 12; cand <= nn + 12; ++cand)
            {
                const int pc  = ((cand % 12) + 12) % 12;
                const int deg = ((pc - key) % 12 + 12) % 12;
                bool ok = true;
                if (scale == 1) ok = kMajor[deg] != 0;
                if (scale == 2) ok = kMinor[deg] != 0;
                if (! ok) continue;
                const double d = std::abs (noteF - (double) cand);
                if (d < bestD)
                {
                    bestD = d;
                    best  = cand;
                }
            }
            return best;
        }

        static constexpr int kMaxMedian = 7;

        // --- composition ---
        factory_core::PitchDetector detector;
        factory_core::PsolaShifter  shifter;
        factory_core::LinearRamp<double> mixRamp { 1.0 }, gainRamp { 1.0 };

        // --- config ---
        double fs = 0.0;
        int    maxBlock = 512;
        int    maxWin = 0;

        // --- rings ---
        std::vector<float> detRing, scratch, dryL, dryR;
        int detSize = 0, drySize = 0;
        std::int64_t detMask = 0, dryMask = 0;
        std::int64_t written = 0;

        // --- live params (block-latched) ---
        double amount = 1.0, retuneMs = 80.0, glideMs = 60.0;
        double tolCt = 12.0, hystCt = 18.0;
        double minHz = 75.0, maxHz = 1300.0, thresh = 0.86, a4 = 440.0;
        int    mode = 2, key = 0, scale = 0;
        int    winLen = 0, hopLen = 512, medLen = 1;
        int    lookahead = 0;

        // --- tracking state ---
        int    hopCounter = 0;
        double medBuf[kMaxMedian] = {};
        int    medPos = 0, medCount = 0;
        double corrCents = 0.0, glidedCents = 0.0;
        int    targetNote = -1;
        std::int64_t unvoicedHops = 0;
    };
} // namespace pf_core
