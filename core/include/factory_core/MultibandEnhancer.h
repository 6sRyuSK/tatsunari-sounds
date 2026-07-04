#pragma once
//
// factory_core/MultibandEnhancer.h — the complete 5-band parallel harmonic
// enhancer engine (Waves-Vitamin-style: direct + enhanced buses, per-band width).
// Header-only, JUCE-independent, headless-testable; the plugin is a thin wrapper.
// Nothing here allocates, locks, or makes syscalls in processBlock.
//
// Signal flow (per channel), all inside one oversampling bracket so the direct
// path and the harmonic path share the SAME phase and latency (no comb filtering
// when they are mixed):
//
//   host in -> [sanitize NaN/Inf -> 0] -> Oversampler up (M) ---------------.
//     direct = Crossover5::allpass(x)                                       |
//     x -> Crossover5 -> b1..b5 -> per band: r_i = HarmonicShaper(b_i)      |
//       linWet = sum width_i(b_i)        resWet = DCblock( sum width_i(r_i) )|
//       wet = linWet + resWet            delta  = resWet * gWet             |
//       outOS = direct*gDirect + wet*gWet                                   |
//   Oversampler down (M): outOS -> out,  delta -> deltaHost <---------------'
//   out = (deltaListen ? deltaHost : out) * gOutput
//
// Two full rate configurations are pre-built so Quality can switch with no
// allocation: HQ (M = 4/2/1 by sample rate, raw shaper) and Zero-Latency
// (M = 1, residual-ADAA shaper). M == 1 makes the oversampler a true bypass
// (latency 0). The DC blocker sits on the residual bus ONLY, so the linear
// reconstruction stays bit-flat (no flatness damage from asymmetric curves).
//
// Quality switching is click-free: a change does NOT hard-swap the path. The
// previously-active path keeps its live state and both paths run in parallel for
// a short window (~20 ms), with an equal-power crossfade of the host-rate output
// from old to new; once the fade completes the old path stops. Only processing is
// toggled and the two outputs are mixed — nothing allocates (CPU doubles only for
// the fade window). The two paths have different latency (HQ 51 / ZL 0 samples),
// so brief comb-filtering during the fade is inherent and accepted; the reported
// latency (PDC) still flips to the new quality immediately.
//
#include "Oversampler.h"
#include "Crossover5.h"
#include "HarmonicShaper.h"
#include "EnvelopeFollower.h"
#include "OnePole.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class MultibandEnhancer
    {
        struct Path;   // fwd-decl (fully defined below); runPath() takes Path& by ref.
    public:
        static constexpr int    kBands      = 5;
        using Mode = HarmonicShaper::Mode;
        enum class Quality { HQ = 0, ZeroLatency };

        // HQ oversampling factor: 4x below 50 kHz, 2x below 100 kHz, else 1x
        // (176.4/192 kHz already have the headroom, so run at native rate + ADAA).
        static int hqFactor (double fs) noexcept
        {
            if (fs < 50000.0)  return 4;
            if (fs < 100000.0) return 2;
            return 1;
        }

        // ---- lifecycle ------------------------------------------------------
        void prepare (double sampleRate, int maxBlock)
        {
            fs       = sampleRate;
            maxN     = std::max (1, maxBlock);
            const int mHq = hqFactor (fs);
            maxM     = mHq;

            pathHQ.prepare (fs, mHq, maxN, modeParam);
            pathZL.prepare (fs, 1,   maxN, modeParam);

            const std::size_t osCap = (std::size_t) (maxN * maxM);
            osInL.assign (osCap, 0.0f);   osInR.assign (osCap, 0.0f);
            osOutL.assign (osCap, 0.0f);  osOutR.assign (osCap, 0.0f);
            osDeltaL.assign (osCap, 0.0f); osDeltaR.assign (osCap, 0.0f);

            // Host-rate scratch for the outgoing path during a quality crossfade
            // (both paths write host-rate output; only mixing/processing toggles).
            const std::size_t hostCap = (std::size_t) maxN;
            xfL.assign (hostCap, 0.0f);  xfR.assign (hostCap, 0.0f);
            xfDL.assign (hostCap, 0.0f); xfDR.assign (hostCap, 0.0f);
            xfadeLen = std::max (1, (int) std::lround (kXfadeSeconds * fs)); // ~20 ms
            xfadeRemaining = 0;
            xfadeFrom = nullptr;

            // Host-rate parameter smoothers.
            for (int b = 0; b < kBands; ++b)
            {
                enhSm[b].setRate (fs, 30.0);   enhSm[b].snap (enhTarget[b]);
                widthSm[b].setRate (fs, 30.0); widthSm[b].snap (widthTarget[b]);
            }
            for (int j = 0; j < 4; ++j) { xoverSm[j].setRate (fs, 40.0); xoverSm[j].snap (std::log (xoverTargetHz[j])); }
            gDirectSm.setRate (fs, 30.0); gDirectSm.snap (gDirectTarget);
            gWetSm.setRate (fs, 30.0);    gWetSm.snap (gWetTarget);
            gOutputSm.setRate (fs, 30.0); gOutputSm.snap (gOutputTarget);

            for (int j = 0; j < 4; ++j) lastXoverHz[j] = xoverTargetHz[j];
            pathHQ.setCrossovers (xoverTargetHz);
            pathZL.setCrossovers (xoverTargetHz);
            xoverDecim = 0;

            activeQuality = qualityParam;
            active = (activeQuality == Quality::HQ) ? &pathHQ : &pathZL;
            latencyDirty = true;
        }

        void reset() noexcept
        {
            pathHQ.reset();
            pathZL.reset();
            // Snap the host-rate smoothers to their targets so reset() lands in
            // the same canonical state a freshly prepared engine would (Class E).
            for (int b = 0; b < kBands; ++b) { enhSm[b].snap (enhTarget[b]); widthSm[b].snap (widthTarget[b]); }
            for (int j = 0; j < 4; ++j) { xoverSm[j].snap (std::log (xoverTargetHz[j])); lastXoverHz[j] = xoverTargetHz[j]; }
            gDirectSm.snap (gDirectTarget);
            gWetSm.snap (gWetTarget);
            gOutputSm.snap (gOutputTarget);
            xoverDecim = 0;
            // An explicit reset is a clean slate: drop any in-flight quality
            // crossfade and adopt the currently-selected quality directly (no
            // fade needed — the audio graph is (re)starting). Keeping activeQuality
            // in sync also means the next processBlock does not spuriously start a
            // fade after a reset (Class E: canonical state == freshly prepared).
            Path* prev    = active;
            activeQuality = qualityParam;
            active        = (activeQuality == Quality::HQ) ? &pathHQ : &pathZL;
            xfadeRemaining = 0;
            xfadeFrom      = nullptr;
            if (active != prev) latencyDirty = true;
        }

        // ---- parameters (plain setters; smoothed internally) ----------------
        void setEnhance (int band, double pct) noexcept { enhTarget[band]   = std::clamp (pct, 0.0, 100.0) * 0.01; }
        void setWidth   (int band, double pct) noexcept { widthTarget[band] = std::clamp (pct, 0.0, 200.0) * 0.01; }
        void setCrossovers (double f1, double f2, double f3, double f4) noexcept
        {
            xoverTargetHz[0] = f1; xoverTargetHz[1] = f2; xoverTargetHz[2] = f3; xoverTargetHz[3] = f4;
        }
        void setMode (Mode m) noexcept
        {
            modeParam = m;
            // Propagate immediately so a reset() snaps the shaper coefficients to
            // the correct mode (the per-block push would otherwise leave them stale).
            for (int b = 0; b < kBands; ++b)
                for (int c = 0; c < 2; ++c) { pathHQ.shaper[b][c].setMode (m); pathZL.shaper[b][c].setMode (m); }
        }
        void setDirectDb (double db) noexcept { gDirectTarget = (db <= -59.5) ? 0.0 : dbToLin (db); }
        void setWetDb    (double db) noexcept { gWetTarget    = (db <= -59.5) ? 0.0 : dbToLin (db); }
        void setOutputDb (double db) noexcept { gOutputTarget = dbToLin (db); }
        void setQuality  (Quality q) noexcept { qualityParam = q; }
        void setDeltaListen (bool on) noexcept { deltaListen = on; }

        // ---- info -----------------------------------------------------------
        int    latencySamples() const noexcept { return active ? active->up[0].latencyHostSamples() : 0; }
        double bandResidualRmsDb (int band) const noexcept { return bandRmsDb[(size_t) std::clamp (band, 0, kBands - 1)]; }
        double effectiveCrossoverHz (int i) const noexcept { return active ? active->xover[0].effectiveCrossoverHz (i) : xoverTargetHz[i]; }
        bool   consumeLatencyDirty() noexcept { const bool d = latencyDirty; latencyDirty = false; return d; }

        // ---- audio ----------------------------------------------------------
        // Processes n host samples in place in L/R; writes the added-harmonics
        // (delta) bus to deltaL/deltaR (host rate, for the analyser/meter).
        void processBlock (float* L, float* R, int n, float* deltaL, float* deltaR) noexcept
        {
            // (Class C) Sanitise: one NaN/Inf must not permanently poison the
            // downstream biquads — replace non-finite input with 0 (self-heals).
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (L[i])) L[i] = 0.0f;
                if (! std::isfinite (R[i])) R[i] = 0.0f;
            }

            // Quality switch: begin an equal-power crossfade instead of a hard swap.
            // The previously-active path keeps its live state and fades OUT while the
            // newly-active path is reset (its buildup masked by the fade-in) and fades
            // IN over ~20 ms. PDC still flips to the new quality now (latencyDirty).
            if (qualityParam != activeQuality)
            {
                xfadeFrom      = active;
                activeQuality  = qualityParam;
                active         = (activeQuality == Quality::HQ) ? &pathHQ : &pathZL;
                active->reset();
                xfadeRemaining = xfadeLen;
                latencyDirty   = true;
            }
            const bool fading = (xfadeRemaining > 0 && xfadeFrom != nullptr && xfadeFrom != active);

            // Push current parameter targets into the host-rate smoothers (once per
            // block; during a fade BOTH paths must see the identical trajectory).
            for (int b = 0; b < kBands; ++b) { enhSm[b].set (enhTarget[b]); widthSm[b].set (widthTarget[b]); }
            for (int j = 0; j < 4; ++j) xoverSm[j].set (std::log (xoverTargetHz[j]));
            gDirectSm.set (gDirectTarget);
            gWetSm.set (gWetTarget);
            gOutputSm.set (gOutputTarget);

            // Outgoing (old) path — only during a fade. Snapshot the host-rate
            // smoother + crossover-decimation state, run the old path into scratch,
            // then restore so the new path re-runs the identical parameter path.
            if (fading)
            {
                SmootherState snap; saveSmoothers (snap);
                runPath (*xfadeFrom, L, R, n, xfL.data(), xfR.data(), xfDL.data(), xfDR.data(), false);
                restoreSmoothers (snap);
            }

            // Active (incoming) path — processes in place and drives the meters.
            runPath (*active, L, R, n, L, R, deltaL, deltaR, true);

            // Equal-power crossfade old -> new at host rate. Equal-power (sin/cos)
            // rather than linear: the two paths have different latency (51 vs 0
            // samples) and are partially decorrelated during the window, so a
            // constant-power law keeps the summed level steady with no mid-fade dip.
            if (fading)
            {
                for (int i = 0; i < n; ++i)
                {
                    const double x  = (double) (xfadeLen - xfadeRemaining) / (double) xfadeLen; // 0 -> 1
                    const double gN = std::sin (0.5 * kPi * x);   // incoming weight (0 -> 1)
                    const double gO = std::cos (0.5 * kPi * x);   // outgoing weight (1 -> 0)
                    L[i]      = (float) (gO * (double) xfL[(size_t) i]  + gN * (double) L[i]);
                    R[i]      = (float) (gO * (double) xfR[(size_t) i]  + gN * (double) R[i]);
                    deltaL[i] = (float) (gO * (double) xfDL[(size_t) i] + gN * (double) deltaL[i]);
                    deltaR[i] = (float) (gO * (double) xfDR[(size_t) i] + gN * (double) deltaR[i]);
                    if (xfadeRemaining > 0) --xfadeRemaining;
                }
                if (xfadeRemaining <= 0) xfadeFrom = nullptr;
            }

            // Output gain + delta-listen (once, post-crossfade).
            for (int i = 0; i < n; ++i)
            {
                const double go = gOutputSm.next();
                const float outL = (float) ((deltaListen ? (double) deltaL[i] : (double) L[i]) * go);
                const float outR = (float) ((deltaListen ? (double) deltaR[i] : (double) R[i]) * go);
                L[i] = outL;
                R[i] = outR;
            }
        }

        // Process n host samples through ONE pre-built path (up -> per-sample core
        // -> down), reading from inL/inR and writing host-rate output to outL/outR
        // and the residual delta to outDL/outDR. Advances the shared host-rate
        // smoothers and crossover-decimation counters, so the caller snapshots/
        // restores that state to run a second path over the same block. outL/outR
        // may alias inL/inR (the up-sampler fully consumes the input first). No
        // allocation — the oversample scratch is shared (paths run sequentially).
        void runPath (Path& P, const float* inL, const float* inR, int n,
                      float* outL, float* outR, float* outDL, float* outDR,
                      bool updateMeter) noexcept
        {
            const int M = P.M;

            for (int b = 0; b < kBands; ++b)
                for (int c = 0; c < 2; ++c)
                    P.shaper[b][c].setMode (modeParam);

            P.up[0].processUp (inL, n, osInL.data());
            P.up[1].processUp (inR, n, osInR.data());

            double rmsAcc[kBands] = { 0, 0, 0, 0, 0 };

            for (int i = 0; i < n; ++i)
            {
                double curEnh[kBands], curWidth[kBands];
                for (int b = 0; b < kBands; ++b) { curEnh[b] = enhSm[b].next(); curWidth[b] = widthSm[b].next(); }
                const double gDirect = gDirectSm.next();
                const double gWet    = gWetSm.next();

                // Crossover coefficient update at sub-block granularity (log-smoothed
                // frequencies), only when a value actually moved (avoids zipper).
                double xl[4];
                for (int j = 0; j < 4; ++j) xl[j] = std::exp (xoverSm[j].next());
                if (--xoverDecim <= 0)
                {
                    xoverDecim = kXoverDecim;
                    if (xl[0] != lastXoverHz[0] || xl[1] != lastXoverHz[1]
                        || xl[2] != lastXoverHz[2] || xl[3] != lastXoverHz[3])
                    {
                        P.xover[0].setFrequencies (xl[0], xl[1], xl[2], xl[3]);
                        P.xover[1].setFrequencies (xl[0], xl[1], xl[2], xl[3]);
                        for (int j = 0; j < 4; ++j) lastXoverHz[j] = xl[j];
                    }
                }

                for (int p = 0; p < M; ++p)
                {
                    const int k = i * M + p;
                    const double xL = (double) osInL[(size_t) k];
                    const double xR = (double) osInR[(size_t) k];

                    const double dL = P.xover[0].allpass (xL);
                    const double dR = P.xover[1].allpass (xR);

                    double bL[kBands], bR[kBands];
                    P.xover[0].process (xL, bL);
                    P.xover[1].process (xR, bR);

                    double linWetL = 0.0, linWetR = 0.0, resPreL = 0.0, resPreR = 0.0;
                    for (int b = 0; b < kBands; ++b)
                    {
                        const double det  = std::max (std::abs (bL[b]), std::abs (bR[b]));
                        P.glueEnv[b].process (det);

                        P.shaper[b][0].setEnhance (curEnh[b]);
                        P.shaper[b][1].setEnhance (curEnh[b]);
                        P.shaper[b][0].setEnvGain (P.gSmoothed[b]);
                        P.shaper[b][1].setEnvGain (P.gSmoothed[b]);

                        const double rL = P.shaper[b][0].processResidual (bL[b]);
                        const double rR = P.shaper[b][1].processResidual (bR[b]);

                        const double w = curWidth[b];
                        { const double m = 0.5 * (bL[b] + bR[b]); const double s = 0.5 * (bL[b] - bR[b]) * w; linWetL += m + s; linWetR += m - s; }
                        { const double m = 0.5 * (rL   + rR);     const double s = 0.5 * (rL   - rR)   * w; resPreL += m + s; resPreR += m - s; }

                        rmsAcc[b] += 0.5 * (rL * rL + rR * rR);
                    }

                    const double resWetL = P.dcBlock[0].hp (resPreL);
                    const double resWetR = P.dcBlock[1].hp (resPreR);
                    const double wetL = linWetL + resWetL;
                    const double wetR = linWetR + resWetR;

                    osOutL[(size_t) k]   = (float) (dL * gDirect + wetL * gWet);
                    osOutR[(size_t) k]   = (float) (dR * gDirect + wetR * gWet);
                    osDeltaL[(size_t) k] = (float) (resWetL * gWet);
                    osDeltaR[(size_t) k] = (float) (resWetR * gWet);

                    // Glue envelope-normalised drive: recompute the per-band target
                    // gain at control rate, smooth it (~10 ms) toward that target.
                    if (--P.glueDecim <= 0)
                    {
                        P.glueDecim = kGlueDecim;
                        const bool glue = (modeParam == Mode::Glue);
                        for (int b = 0; b < kBands; ++b)
                        {
                            if (glue)
                            {
                                const double e = std::max (P.glueEnv[b].value(), kEnvFloor);
                                P.gTarget[b] = std::clamp (std::sqrt (kGlueRef / e), kGlueMin, kGlueMax);
                            }
                            else
                                P.gTarget[b] = 1.0;
                        }
                    }
                    for (int b = 0; b < kBands; ++b)
                        P.gSmoothed[b] += (P.gTarget[b] - P.gSmoothed[b]) * P.glueSmooth;
                }
            }

            P.down[0].processDown (osOutL.data(),   n, outL);
            P.down[1].processDown (osOutR.data(),   n, outR);
            P.downD[0].processDown (osDeltaL.data(), n, outDL);
            P.downD[1].processDown (osDeltaR.data(), n, outDR);

            // Only the active (incoming) path publishes the residual meters.
            if (updateMeter)
            {
                const double inv = 1.0 / (double) std::max (1, n * M);
                for (int b = 0; b < kBands; ++b)
                    bandRmsDb[(size_t) b] = 10.0 * std::log10 (std::max (rmsAcc[b] * inv, 1.0e-30));
            }
        }

    private:
        static double dbToLin (double db) noexcept { return std::pow (10.0, db / 20.0); }

        // One-pole parameter smoother (host or path rate).
        struct Smoother
        {
            double cur = 0.0, target = 0.0, coeff = 1.0;
            void setRate (double sr, double ms) noexcept { coeff = 1.0 - std::exp (-1.0 / std::max (1.0, ms * 0.001 * sr)); }
            void snap (double v) noexcept { cur = target = v; }
            void set (double v) noexcept { target = v; }
            double next() noexcept { cur += (target - cur) * coeff; return cur; }
        };

        // Snapshot of the shared host-rate smoother + crossover-decimation state,
        // so a quality crossfade can run BOTH paths over one block with an identical
        // per-sample parameter trajectory (save before the old path, restore before
        // the new). Plain doubles/ints on the stack — no allocation.
        struct SmootherState
        {
            double enh[kBands], width[kBands], xover[4], gDirect, gWet, lastXover[4];
            int    xoverDecim;
        };
        void saveSmoothers (SmootherState& s) const noexcept
        {
            for (int b = 0; b < kBands; ++b) { s.enh[b] = enhSm[b].cur; s.width[b] = widthSm[b].cur; }
            for (int j = 0; j < 4; ++j) { s.xover[j] = xoverSm[j].cur; s.lastXover[j] = lastXoverHz[j]; }
            s.gDirect = gDirectSm.cur; s.gWet = gWetSm.cur; s.xoverDecim = xoverDecim;
        }
        void restoreSmoothers (const SmootherState& s) noexcept
        {
            for (int b = 0; b < kBands; ++b) { enhSm[b].cur = s.enh[b]; widthSm[b].cur = s.width[b]; }
            for (int j = 0; j < 4; ++j) { xoverSm[j].cur = s.xover[j]; lastXoverHz[j] = s.lastXover[j]; }
            gDirectSm.cur = s.gDirect; gWetSm.cur = s.gWet; xoverDecim = s.xoverDecim;
        }

        // One rate configuration (HQ or Zero-Latency). Plain data holder; the
        // engine drives its members. Both are pre-built so Quality switches
        // without allocation.
        struct Path
        {
            int    M = 1;
            double rate = 44100.0;
            Oversampler up[2], down[2], downD[2];
            Crossover5  xover[2];
            HarmonicShaper shaper[kBands][2];
            EnvelopeFollower glueEnv[kBands];
            OnePole dcBlock[2];
            double gTarget[kBands]   { 1, 1, 1, 1, 1 };
            double gSmoothed[kBands] { 1, 1, 1, 1, 1 };
            double glueSmooth = 0.01;
            int    glueDecim = 0;

            void prepare (double hostFs, int factor, int maxBlock, Mode mode)
            {
                M    = factor;
                rate = hostFs * (double) factor;
                for (int c = 0; c < 2; ++c)
                {
                    up[c].prepare   (hostFs, factor, maxBlock);
                    down[c].prepare (hostFs, factor, maxBlock);
                    downD[c].prepare (hostFs, factor, maxBlock);
                    xover[c].prepare (rate);
                    dcBlock[c].setCutoff (kDcBlockHz, rate);
                }
                for (int b = 0; b < kBands; ++b)
                {
                    glueEnv[b].prepare (rate);
                    glueEnv[b].setTimes (5.0, 150.0);
                    for (int c = 0; c < 2; ++c)
                    {
                        shaper[b][c].prepare (rate);
                        shaper[b][c].setMode (mode);
                        shaper[b][c].setAdaa (factor == 1);   // 1x path anti-aliases via ADAA
                    }
                }
                glueSmooth = 1.0 - std::exp (-1.0 / std::max (1.0, 0.010 * rate));
                reset();
            }

            void setCrossovers (const double (&f)[4]) noexcept
            {
                xover[0].setFrequencies (f[0], f[1], f[2], f[3]);
                xover[1].setFrequencies (f[0], f[1], f[2], f[3]);
            }

            void reset() noexcept
            {
                for (int c = 0; c < 2; ++c)
                {
                    up[c].reset(); down[c].reset(); downD[c].reset();
                    xover[c].reset(); dcBlock[c].reset();
                }
                for (int b = 0; b < kBands; ++b)
                {
                    glueEnv[b].reset();
                    gTarget[b] = 1.0; gSmoothed[b] = 1.0;
                    for (int c = 0; c < 2; ++c) shaper[b][c].reset();
                }
                glueDecim = 0;
            }
        };

        static constexpr double kEnvFloor  = 1.0e-4;   // -80 dBFS absolute floor (Class J)
        static constexpr double kGlueRef   = 0.125;    // -18 dBFS target
        static constexpr double kGlueMin   = 0.25;
        static constexpr double kGlueMax   = 4.0;
        static constexpr int    kGlueDecim = 32;
        static constexpr double kDcBlockHz = 5.0;
        static constexpr int    kXoverDecim = 32;
        static constexpr double kXfadeSeconds = 0.020; // ~20 ms click-free quality crossfade
        static constexpr double kPi = 3.14159265358979323846; // equal-power crossfade curve

        double fs   = 44100.0;
        int    maxN = 0;
        int    maxM = 1;

        // Oversample-rate scratch (shared: the two paths run sequentially per block).
        std::vector<float> osInL, osInR, osOutL, osOutR, osDeltaL, osDeltaR;
        // Host-rate scratch for the outgoing path's output during a quality crossfade.
        std::vector<float> xfL, xfR, xfDL, xfDR;

        Path pathHQ, pathZL;
        Path* active = nullptr;
        // Quality crossfade: the path fading out, and how many host samples remain.
        Path* xfadeFrom      = nullptr;
        int   xfadeLen       = 1;
        int   xfadeRemaining = 0;
        Quality qualityParam  = Quality::HQ;
        Quality activeQuality = Quality::HQ;
        Mode    modeParam     = Mode::Tube;
        bool    deltaListen   = false;
        bool    latencyDirty  = false;

        // parameter targets (set by the host, snapped/smoothed in prepare/process)
        double enhTarget[kBands]   { 0, 0, 0, 0, 0 };
        double widthTarget[kBands] { 1, 1, 1, 1, 1 };
        double xoverTargetHz[4]    { 130.0, 700.0, 2200.0, 7500.0 };
        double gDirectTarget = 1.0;
        double gWetTarget    = 0.251188643150958; // -12 dB default (Enhanced)
        double gOutputTarget = 1.0;

        // smoothers
        Smoother enhSm[kBands], widthSm[kBands], xoverSm[4];
        Smoother gDirectSm, gWetSm, gOutputSm;
        double   lastXoverHz[4] { 130.0, 700.0, 2200.0, 7500.0 };
        int      xoverDecim = 0;

        double bandRmsDb[kBands] { -120, -120, -120, -120, -120 };
    };
} // namespace factory_core
