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

            // Quality switch: swap the pre-built path, reset it (clean, click on a
            // deliberate latency change is acceptable), flag latency for the host.
            if (qualityParam != activeQuality)
            {
                activeQuality = qualityParam;
                active = (activeQuality == Quality::HQ) ? &pathHQ : &pathZL;
                active->reset();
                latencyDirty = true;
            }
            Path& P = *active;
            const int M = P.M;

            // Push current parameter targets into the host-rate smoothers.
            for (int b = 0; b < kBands; ++b) { enhSm[b].set (enhTarget[b]); widthSm[b].set (widthTarget[b]); }
            for (int j = 0; j < 4; ++j) xoverSm[j].set (std::log (xoverTargetHz[j]));
            gDirectSm.set (gDirectTarget);
            gWetSm.set (gWetTarget);
            gOutputSm.set (gOutputTarget);

            for (int b = 0; b < kBands; ++b)
                for (int c = 0; c < 2; ++c)
                    P.shaper[b][c].setMode (modeParam);

            P.up[0].processUp (L, n, osInL.data());
            P.up[1].processUp (R, n, osInR.data());

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

            P.down[0].processDown (osOutL.data(),   n, L);
            P.down[1].processDown (osOutR.data(),   n, R);
            P.downD[0].processDown (osDeltaL.data(), n, deltaL);
            P.downD[1].processDown (osDeltaR.data(), n, deltaR);

            for (int i = 0; i < n; ++i)
            {
                const double go = gOutputSm.next();
                const float outL = (float) ((deltaListen ? (double) deltaL[i] : (double) L[i]) * go);
                const float outR = (float) ((deltaListen ? (double) deltaR[i] : (double) R[i]) * go);
                L[i] = outL;
                R[i] = outR;
            }

            const double inv = 1.0 / (double) std::max (1, n * M);
            for (int b = 0; b < kBands; ++b)
                bandRmsDb[(size_t) b] = 10.0 * std::log10 (std::max (rmsAcc[b] * inv, 1.0e-30));
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

        double fs   = 44100.0;
        int    maxN = 0;
        int    maxM = 1;

        // Oversample-rate scratch (shared: only one path is active at a time).
        std::vector<float> osInL, osInR, osOutL, osOutR, osDeltaL, osDeltaR;

        Path pathHQ, pathZL;
        Path* active = nullptr;
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
