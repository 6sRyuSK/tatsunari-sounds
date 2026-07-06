#pragma once
//
// factory_core/TumbleDelay.h — a physics-driven granular delay engine. Input
// transients spawn balls inside a rotating 2D box; every time a ball hits a wall
// it fires one Hann-windowed grain that reads the input's stereo ring buffer at a
// physics-derived offset, panned by the impact position. There is NO audio
// feedback loop: the "delay tail" is the trigger sequence (§4.1) and the Refeed
// generation spawn (§4.8), both purely feed-forward and structurally unable to
// diverge. Header-only, JUCE-independent, headless-testable.
//
// Real-time safe: ball / grain / event / pending pools are fixed std::arrays;
// the only allocation is the ring buffer in prepare(). Randomness comes from a
// single inlined xorshift generator (same lexicon as GranularDelay.h), reseeded
// deterministically in reset() so a full offline render is bit-reproducible.
//
// Physics runs on a fixed 1 kHz tick (a double sample accumulator, so timing is
// independent of sample rate and block size); wall collisions are refined to
// sub-sample precision by bisection and queued as grain-start events. See
// docs/plans/physics-granular-delay.md (§4/§6/§9/§11) for the full spec.
//
#include "EnvelopeFollower.h"
#include "OnePole.h"
#include "SmoothingCoeff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace factory_core
{
    class TumbleDelay
    {
    public:
        // --- public contract (constants) ---
        static constexpr int    kNumSlots        = 4;
        static constexpr int    kMaxBallsPerSlot = 8;
        static constexpr int    kMaxBalls        = 32;
        static constexpr int    kMaxGrains       = 64;
        static constexpr double kRingSeconds     = 34.0;
        static constexpr double kHorizonSeconds  = 33.0;

        enum class Shape   { Triangle, Square, Pentagon, Hexagon, Octagon, Circle };
        enum class PanMode { Physics, Center, Random };

        struct SlotParams
        {
            bool    enabled        = false;
            int     count          = 1;
            double  ballSize       = 0.08;
            double  speed          = 1.0;
            double  directionDeg   = 90.0;
            double  dirRandom      = 1.0;
            double  preDelaySeconds = 0.0;
            double  timeSeconds    = 0.35;
            double  bounce         = 0.70;
            double  drag           = 0.10;
            double  decayCurve     = 0.0;
            bool    lifeIsBounces  = false;
            double  lifeTimeSeconds = 3.0;
            int     lifeBounces    = 12;
            double  pitchSemis     = 0.0;
            double  pitchRandSemis = 0.0;
            double  grainMs        = 90.0;
            double  reverseProb    = 0.0;
            double  motion         = 0.0;
            double  stepSeconds    = 0.0;
            double  sprayMs        = 0.0;
            PanMode panMode        = PanMode::Physics;
            double  gainLinear     = 1.0;
        };

        struct BallView { float x, y, radius; int slot; float energy; int generation; };
        struct HitEvent { float x, y; int slot; float intensity; };

        // --- lifecycle ---
        void prepare (double sampleRate)
        {
            fs = (sampleRate > 0.0) ? sampleRate : 44100.0;

            // Ring holds the worst-case memory horizon (34 s) + 0.5 s slack + guard.
            // float storage keeps 192 kHz stereo under ~52 MB (double would exceed
            // 100 MB — regression class D). Allocated here only.
            const int ringN = (int) (kRingSeconds * fs + 0.5 * fs) + 8;
            ring.prepare (ringN);

            onset.prepare (fs);
            onset.setTimes (1.0, 80.0);                 // fast attack, slow-ish release

            tickSamples    = fs / 1000.0;               // 1 kHz physics tick
            minFireSamples = (long long) (0.015 * fs);  // per-ball min grain interval 15 ms
            cAudio = onePoleCoeffForMs (20.0, fs);      // tone / mix per-sample smoothing
            cPhys  = onePoleCoeffForMs (50.0, 1000.0);  // physics params, tick-rate smoothing

            reset();
        }

        void reset() noexcept
        {
            ring.reset();
            for (auto& b : balls)   b.active = false;
            for (auto& g : grains)  g.active = false;
            for (auto& p : pending) p.active = false;
            for (auto& e : events)  e.active = false;

            onset.reset();
            armed = true;
            lastOnsetSample = -(1LL << 50);             // allow the first onset to fire

            rngState    = 0x9e3779b9u;                  // deterministic reseed (bit-repro)
            sampleClock = 0;
            tickAccum   = 0.0;
            boxTheta    = 0.0;

            // Snap smoothed params to their targets so no start-up ramp poisons repro.
            boxSizeS = boxSizeTarget; spinS = spinTarget;
            pivotXS  = pivotXTarget;  pivotYS = pivotYTarget;
            grav01S  = grav01Target;
            mixS     = mixTarget;     toneHzS = toneHzTarget;

            toneL.reset(); toneR.reset();
            toneL.setCutoff (toneHzS, fs);
            toneR.setCutoff (toneHzS, fs);

            recomputeDerived();

            aliveBallsAtomic.store (0, std::memory_order_relaxed);
            activeGrainsAtomic.store (0, std::memory_order_relaxed);
            boxAngleAtomic.store (0.0, std::memory_order_relaxed);
            snapIndex.store (0, std::memory_order_relaxed);
            snapCount[0].store (0, std::memory_order_relaxed);
            snapCount[1].store (0, std::memory_order_relaxed);
            hitWrite.store (0, std::memory_order_relaxed);
            hitRead.store (0, std::memory_order_relaxed);
        }

        // --- setters (all noexcept; store targets, clamp defensively) ---
        void setBoxShape (Shape s) noexcept
        {
            switch (s)
            {
                case Shape::Triangle: shapeN = 3; isCircle = false; break;
                case Shape::Square:   shapeN = 4; isCircle = false; break;
                case Shape::Pentagon: shapeN = 5; isCircle = false; break;
                case Shape::Hexagon:  shapeN = 6; isCircle = false; break;
                case Shape::Octagon:  shapeN = 8; isCircle = false; break;
                case Shape::Circle:   shapeN = 0; isCircle = true;  break;
            }
            rebuildVertices();
        }
        void setBoxSizeSeconds (double s) noexcept { boxSizeTarget = std::clamp (s, 0.02, 8.0); }
        void setSpinRevPerSec  (double s) noexcept { spinTarget    = std::clamp (s, -8.0, 8.0); }
        void setPivot (double x, double y) noexcept
        {
            pivotXTarget = std::clamp (x, -1.0, 1.0);
            pivotYTarget = std::clamp (y, -1.0, 1.0);
        }
        void setGravity      (double g01) noexcept { grav01Target = std::clamp (g01, 0.0, 1.0); }
        void setBallCollide  (bool b)     noexcept { ballCollide  = b; }
        void setSenseDb      (double db)  noexcept { senseDb      = db; }
        void setRetrigMs     (double ms)  noexcept { retrigMs     = std::max (0.0, ms); }
        void setSpawnSpread  (double s)   noexcept { spawnSpread  = std::clamp (s, 0.0, 1.0); }
        void setRefeed       (double r)   noexcept { refeed       = std::clamp (r, 0.0, 0.95); }
        void setToneHz       (double hz)  noexcept { toneHzTarget = std::max (20.0, hz); }
        void setMix          (double m)   noexcept { mixTarget    = std::clamp (m, 0.0, 1.0); }
        void setSlotParams (int slot, const SlotParams& p) noexcept
        {
            if (slot >= 0 && slot < kNumSlots) this->slot[slot] = p;
        }

        // --- audio (per-sample, in place) ---
        void processStereo (double& l, double& r) noexcept
        {
            const double dryL = l, dryR = r;
            const double mono = 0.5 * (dryL + dryR);

            ring.write (dryL, dryR);                    // dry only — no audio feedback

            // Onset detection -> schedule (silent) trigger sequences.
            const double env = onset.process (mono);
            handleOnset (env);

            // Per-sample smoothing of the audio-path scalars.
            mixS    = cAudio * mixS    + (1.0 - cAudio) * mixTarget;
            toneHzS = cAudio * toneHzS + (1.0 - cAudio) * toneHzTarget;
            toneL.setCutoff (toneHzS, fs);
            toneR.setCutoff (toneHzS, fs);

            // Spawn balls whose scheduled time has arrived, then start any grains
            // whose collision sample has arrived.
            processPending();
            activateDueGrains();

            // Render the active grains.
            double wetL = 0.0, wetR = 0.0;
            int grainCount = 0;
            const double maxRead = (double) ring.getSize() - 2.0;
            for (auto& g : grains)
            {
                if (! g.active) continue;
                ++grainCount;

                const double phase = g.n / g.duration;
                const double win   = 0.5 - 0.5 * std::cos (2.0 * kPi * phase);
                double d = g.d0 + g.n * (g.reverse ? (1.0 + g.rate) : (1.0 - g.rate));
                d = std::clamp (d, 1.0, maxRead);

                const double src = 0.5 * (ring.readL (d) + ring.readR (d));
                const double s   = g.gain * win * src;
                wetL += s * g.gL;
                wetR += s * g.gR;

                g.n += 1.0;
                if (g.n >= g.duration) g.active = false;
            }

            wetL = toneL.lp (wetL);
            wetR = toneR.lp (wetR);

            // Finite guard (regression class C): the grain sum is the only place a
            // NaN/Inf can surface. Kill all voices, flush the ring and tone state,
            // and emit silence for this sample so the engine self-recovers without
            // an external reset().
            if (! std::isfinite (wetL) || ! std::isfinite (wetR))
            {
                for (auto& g : grains) g.active = false;
                ring.reset();
                toneL.reset(); toneR.reset();
                wetL = wetR = 0.0;
                grainCount = 0;
            }

            l = (1.0 - mixS) * dryL + mixS * wetL;
            r = (1.0 - mixS) * dryR + mixS * wetR;

            activeGrainsAtomic.store (grainCount, std::memory_order_relaxed);

            // Advance the physics clock. Ticks look ahead the coming 1 ms window,
            // scheduling collisions into future samples (window starts next sample).
            tickAccum += 1.0;
            while (tickAccum >= tickSamples)
            {
                tickAccum -= tickSamples;
                runTick (sampleClock + 1);
            }
            ++sampleClock;
        }

        // --- UI mirror / introspection (lock-free) ---
        int snapshotBalls (BallView* dst, int maxCount) const noexcept
        {
            if (dst == nullptr || maxCount <= 0) return 0;
            const int idx = snapIndex.load (std::memory_order_acquire);
            const int c   = snapCount[idx].load (std::memory_order_relaxed);
            const int n   = std::min (c, maxCount);
            for (int i = 0; i < n; ++i) dst[i] = snap[idx][i];
            return n;
        }

        int drainHits (HitEvent* dst, int maxCount) noexcept
        {
            if (dst == nullptr || maxCount <= 0) return 0;
            const std::uint64_t w = hitWrite.load (std::memory_order_acquire);
            std::uint64_t rd = hitRead.load (std::memory_order_relaxed);
            if (w - rd > kHitCap) rd = w - kHitCap;     // overflow: drop oldest
            int cnt = 0;
            while (rd < w && cnt < maxCount)
            {
                dst[cnt++] = hits[(std::size_t) (rd % kHitCap)];
                ++rd;
            }
            hitRead.store (rd, std::memory_order_relaxed);
            return cnt;
        }

        double boxAngle()     const noexcept { return boxAngleAtomic.load (std::memory_order_relaxed); }
        int    activeGrains() const noexcept { return activeGrainsAtomic.load (std::memory_order_relaxed); }
        int    aliveBalls()   const noexcept { return aliveBallsAtomic.load (std::memory_order_relaxed); }

        double tailSeconds() const noexcept
        {
            double t = 0.0;
            for (int i = 0; i < kNumSlots; ++i)
            {
                const SlotParams& sp = slot[i];
                if (! sp.enabled) continue;
                const double life = std::clamp (sp.lifeTimeSeconds, 0.1, 16.0);
                const double pd   = std::clamp (sp.preDelaySeconds, 0.0, 1.0);
                const double tm   = std::clamp (sp.timeSeconds, 0.01, 2.0);
                const int    c    = std::clamp (sp.count, 1, kMaxBallsPerSlot);
                const double cand = pd + (double) (c - 1) * tm + life + sp.grainMs * 1.0e-3;
                t = std::max (t, cand);
            }
            return t;
        }

    private:
        static constexpr double kPi     = 3.14159265358979323846;
        static constexpr int    kHitCap = 256;

        struct Vec2 { double x = 0.0, y = 0.0; };
        static Vec2 add  (Vec2 a, Vec2 b) noexcept { return { a.x + b.x, a.y + b.y }; }
        static Vec2 sub  (Vec2 a, Vec2 b) noexcept { return { a.x - b.x, a.y - b.y }; }
        static Vec2 scale (Vec2 a, double s) noexcept { return { a.x * s, a.y * s }; }
        static double dot (Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
        static double len (Vec2 a) noexcept { return std::sqrt (a.x * a.x + a.y * a.y); }

        // ------------------------------------------------------------------ ring
        // Private nested stereo ring (DelayLine.h is deliberately not reused, per
        // §11): float storage, written every sample, linearly-interpolated read
        // returning double. Non-finite writes are coerced to 0.
        struct StereoRing
        {
            std::vector<float> bl, br;
            int size = 0, wpos = 0;

            void prepare (int n) { size = std::max (8, n); bl.assign ((size_t) size, 0.0f); br.assign ((size_t) size, 0.0f); wpos = 0; }
            void reset() noexcept { std::fill (bl.begin(), bl.end(), 0.0f); std::fill (br.begin(), br.end(), 0.0f); wpos = 0; }
            int  getSize() const noexcept { return size; }

            void write (double l, double r) noexcept
            {
                if (! std::isfinite (l)) l = 0.0;
                if (! std::isfinite (r)) r = 0.0;
                bl[(size_t) wpos] = (float) l;
                br[(size_t) wpos] = (float) r;
                if (++wpos >= size) wpos = 0;
            }
            double readL (double d) const noexcept { return readCh (bl, d); }
            double readR (double d) const noexcept { return readCh (br, d); }
            double readCh (const std::vector<float>& b, double d) const noexcept
            {
                double rp = (double) (wpos - 1) - d;
                while (rp < 0.0)            rp += (double) size;
                while (rp >= (double) size) rp -= (double) size;
                const int i0 = (int) rp;
                const double f = rp - (double) i0;
                const int i1 = (i0 + 1 >= size) ? 0 : i0 + 1;
                return (double) b[(size_t) i0] + f * ((double) b[(size_t) i1] - (double) b[(size_t) i0]);
            }
        };

        // ------------------------------------------------------------------ pools
        struct Ball
        {
            bool active = false;
            int  slotIndex = 0;
            int  generation = 0;
            double gainScale = 1.0;
            Vec2 pos, v;
            long long anchorSample = 0, spawnSample = 0, lastFireSample = 0;
            double ageSeconds = 0.0, lowSpeedSeconds = 0.0;
            int  lifeBounceCount = 0, firedCount = 0;
            bool hadCollision = false, horizonRetire = false;
            Vec2 lastCollisionPos;
        };
        struct Grain
        {
            bool active = false, reverse = false;
            double n = 0.0, duration = 1.0, d0 = 0.0, rate = 1.0;
            double gain = 0.0, gL = 0.0, gR = 0.0;
        };
        struct Pending { bool active = false; long long due = 0, anchor = 0; int slot = 0, k = 1; };
        struct GEvent  { bool active = false, reverse = false; long long startSample = 0; double d0 = 0, rate = 1, duration = 1, gain = 0, gL = 0, gR = 0; };

        // ------------------------------------------------------------------ rng
        // xorshift32 — identical lexicon to GranularDelay.h. Bipolar in [-1, 1).
        double nextRandBipolar() noexcept
        {
            rngState ^= rngState << 13;
            rngState ^= rngState >> 17;
            rngState ^= rngState << 5;
            return (double) rngState / 2147483648.0 - 1.0;
        }
        double nextRand01() noexcept { return 0.5 * (nextRandBipolar() + 1.0); }

        // ------------------------------------------------------------------ geometry
        void rebuildVertices() noexcept
        {
            if (isCircle) return;
            for (int i = 0; i < shapeN; ++i)
            {
                // Flat-bottom convention: at theta=0 one edge lies horizontal at the
                // bottom (a resting Square reads as an upright box, and gravity gets
                // a level floor). Vertex i sits at -pi/2 + pi/N + 2*pi*i/N.
                const double a = 2.0 * kPi * (double) i / (double) shapeN
                                 - 0.5 * kPi + kPi / (double) shapeN;
                baseV[i] = { std::cos (a), std::sin (a) };
            }
        }
        Vec2 rot (Vec2 v, double ct, double st) const noexcept { return { v.x * ct - v.y * st, v.x * st + v.y * ct }; }
        Vec2 boxCenter (double ct, double st) const noexcept
        {
            const Vec2 rp = rot (pivot, ct, st);        // box center = p + Rot(-p) = p - Rot(p)
            return { pivot.x - rp.x, pivot.y - rp.y };
        }
        Vec2 vertex (int i, double ct, double st) const noexcept
        {
            return add (pivot, rot (sub (baseV[i], pivot), ct, st));
        }
        static Vec2 closestOnSeg (Vec2 c, Vec2 a, Vec2 b) noexcept
        {
            const Vec2 ab = sub (b, a);
            const double denom = dot (ab, ab);
            double t = (denom > 1e-18) ? dot (sub (c, a), ab) / denom : 0.0;
            t = std::clamp (t, 0.0, 1.0);
            return add (a, scale (ab, t));
        }

        // Max wall penetration for a ball of radius r at box angle theta. >= 0 means
        // the disk touches/crosses a wall. Fills the winning wall's outward normal
        // and contact point (segment-clamped, per §11 "center × edge nearest point").
        double boxPenetration (Vec2 c, double theta, double r, Vec2& nOut, Vec2& contact) const noexcept
        {
            const double ct = std::cos (theta), st = std::sin (theta);
            const Vec2 bc = boxCenter (ct, st);

            if (isCircle)
            {
                const Vec2 d = sub (c, bc);
                const double dist = len (d);
                const Vec2 dir = (dist > 1e-9) ? scale (d, 1.0 / dist) : Vec2 { 0.0, 1.0 };
                nOut = dir;
                contact = add (bc, dir);                // inner wall radius 1
                return dist + r - 1.0;
            }

            double best = -1e30;
            for (int i = 0; i < shapeN; ++i)
            {
                const Vec2 A = vertex (i, ct, st);
                const Vec2 B = vertex ((i + 1) % shapeN, ct, st);
                const Vec2 e = sub (B, A);
                Vec2 nrm = { e.y, -e.x };
                const double nl = len (nrm);
                if (nl > 1e-12) nrm = scale (nrm, 1.0 / nl);
                const Vec2 mid = scale (add (A, B), 0.5);
                if (dot (nrm, sub (mid, bc)) < 0.0) nrm = { -nrm.x, -nrm.y };   // force outward
                const double pen = dot (sub (c, A), nrm) + r;
                if (pen > best) { best = pen; nOut = nrm; contact = closestOnSeg (c, A, B); }
            }
            return best;
        }

        double ballRadius (const SlotParams& sp) const noexcept
        {
            const double inradius = isCircle ? 1.0 : std::cos (kPi / (double) shapeN);
            return std::clamp (sp.ballSize, 0.002, 0.4 * inradius);
        }

        // ------------------------------------------------------------------ onset
        void handleOnset (double env) noexcept
        {
            const double thresh = std::max (std::pow (10.0, senseDb / 20.0), 1.0e-4);   // absolute -80 dBFS floor
            const long long retrig = (long long) (retrigMs * 1.0e-3 * fs);

            if (env >= thresh && armed && (sampleClock - lastOnsetSample) >= retrig)
            {
                lastOnsetSample = sampleClock;
                armed = false;
                const long long anchor = sampleClock;
                for (int s = 0; s < kNumSlots; ++s)
                {
                    const SlotParams& sp = slot[s];
                    if (! sp.enabled) continue;
                    const int cnt = std::clamp (sp.count, 1, kMaxBallsPerSlot);
                    const long long pd = (long long) (std::clamp (sp.preDelaySeconds, 0.0, 1.0) * fs);
                    const long long tm = (long long) (std::clamp (sp.timeSeconds, 0.01, 2.0) * fs);
                    for (int k = 1; k <= cnt; ++k)
                        schedulePending (sampleClock + pd + (long long) (k - 1) * tm, s, k, anchor);
                }
            }
            if (env < 0.5 * thresh) armed = true;
        }

        void schedulePending (long long due, int s, int k, long long anchor) noexcept
        {
            int idx = -1;
            for (int i = 0; i < kPendingCap; ++i) if (! pending[i].active) { idx = i; break; }
            if (idx < 0)   // full: drop oldest (smallest due)
            {
                long long md = std::numeric_limits<long long>::max();
                for (int i = 0; i < kPendingCap; ++i) if (pending[i].due < md) { md = pending[i].due; idx = i; }
            }
            pending[idx] = { true, due, anchor, s, k };
        }

        void processPending() noexcept
        {
            for (int i = 0; i < kPendingCap; ++i)
                if (pending[i].active && pending[i].due <= sampleClock)
                {
                    spawnBall (pending[i]);
                    pending[i].active = false;
                }
        }

        // ------------------------------------------------------------------ spawning
        int acquireBallSlot() noexcept
        {
            for (int i = 0; i < kMaxBalls; ++i) if (! balls[i].active) return i;
            // Pool full: steal the oldest (earliest spawnSample). No Refeed on steal.
            int idx = 0; long long ms = std::numeric_limits<long long>::max();
            for (int i = 0; i < kMaxBalls; ++i) if (balls[i].spawnSample < ms) { ms = balls[i].spawnSample; idx = i; }
            return idx;
        }

        void spawnBall (const Pending& p) noexcept
        {
            const SlotParams& sp = slot[p.slot];
            Ball& b = balls[acquireBallSlot()];

            b.active = true;
            b.slotIndex = p.slot;
            b.generation = 0;
            b.gainScale = 1.0;
            b.anchorSample = p.anchor;
            b.spawnSample = sampleClock;
            b.ageSeconds = 0.0;
            b.lowSpeedSeconds = 0.0;
            b.lifeBounceCount = 0;
            b.firedCount = 0;
            b.hadCollision = false;
            b.horizonRetire = false;
            b.lastFireSample = sampleClock - minFireSamples - 1;

            const int cnt = std::clamp (sp.count, 1, kMaxBallsPerSlot);
            const int kk  = std::clamp (p.k, 1, cnt);
            const double e = std::clamp (sp.bounce, 0.0, 1.0);
            // D11: ball k is "born already bounced (k-1) times" -> initial speed decay.
            const double spd = std::clamp (sp.speed, 0.01, 8.0) * std::pow (e, (double) (kk - 1)) * vRef;
            const double dirDeg = sp.directionDeg + std::clamp (sp.dirRandom, 0.0, 1.0) * (nextRandBipolar() * 180.0);
            const double dr = dirDeg * kPi / 180.0;
            b.v = { std::cos (dr) * spd, std::sin (dr) * spd };

            const double ct = std::cos (boxTheta), st = std::sin (boxTheta);
            const Vec2 bc = boxCenter (ct, st);
            const double u = nextRand01();
            const double rr = spawnSpread * std::sqrt (u);          // uniform disk (radius * sqrt(u))
            const double ang = nextRand01() * 2.0 * kPi;
            b.pos = { bc.x + rr * std::cos (ang), bc.y + rr * std::sin (ang) };
            b.lastCollisionPos = b.pos;
        }

        // ------------------------------------------------------------------ physics tick
        void recomputeDerived() noexcept
        {
            vRef = 2.0 / std::max (boxSizeS, 0.02);
            omega = spinS * 2.0 * kPi;
            pivot = { pivotXS, pivotYS };
            gravity01 = grav01S;
        }

        void runTick (long long windowStart) noexcept
        {
            // Physics params smooth at the tick rate (jitter only affects the event
            // stream, so this is zipper-free; boxSize jumps are safe via push-out).
            boxSizeS = cPhys * boxSizeS + (1.0 - cPhys) * boxSizeTarget;
            spinS    = cPhys * spinS    + (1.0 - cPhys) * spinTarget;
            pivotXS  = cPhys * pivotXS  + (1.0 - cPhys) * pivotXTarget;
            pivotYS  = cPhys * pivotYS  + (1.0 - cPhys) * pivotYTarget;
            grav01S  = cPhys * grav01S  + (1.0 - cPhys) * grav01Target;
            recomputeDerived();

            for (auto& b : balls) if (b.active) tickBall (b, windowStart);
            if (ballCollide) resolveBallBall();

            boxTheta += omega * 0.001;
            if (boxTheta > 2.0 * kPi || boxTheta < -2.0 * kPi) boxTheta = std::fmod (boxTheta, 2.0 * kPi);

            writeSnapshot();
            boxAngleAtomic.store (boxTheta, std::memory_order_relaxed);
        }

        void tickBall (Ball& b, long long windowStart) noexcept
        {
            const double dt = 0.001;
            const SlotParams& sp = slot[b.slotIndex];
            const double r = ballRadius (sp);

            // Semi-implicit Euler: v += g*dt ; v *= exp(-k_eff*dt) ; pos += v*dt.
            b.v.y += (-8.0 * gravity01) * dt;           // world gravity points down
            const double x = std::clamp (b.ageSeconds / std::max (sp.lifeTimeSeconds, 0.1), 0.0, 1.0);
            const double kEff = std::clamp (sp.drag, 0.0, 1.0) * 4.0 * dragProfile (x, std::clamp (sp.decayCurve, -1.0, 1.0));
            const double df = std::exp (-kEff * dt);
            b.v.x *= df; b.v.y *= df;

            // Move, resolving up to 4 wall collisions this tick.
            double tRemain = dt;
            double thetaSeg = boxTheta;
            int resolves = 0;
            while (tRemain > 1e-12 && resolves < 4)
            {
                const Vec2 posEnd = add (b.pos, scale (b.v, tRemain));
                const double dAngle = omega * tRemain;
                double frac;
                if (! findCollision (b.pos, posEnd, thetaSeg, dAngle, r, frac))
                {
                    b.pos = posEnd;
                    break;
                }
                const double tAdv = frac * tRemain;
                b.pos = add (b.pos, scale (b.v, tAdv));
                const double thetaCol = thetaSeg + omega * tAdv;
                const double tElapsed = dt - (tRemain - tAdv);
                resolveWall (b, sp, r, thetaCol, windowStart, tElapsed);
                thetaSeg = thetaCol;
                tRemain -= tAdv;
                ++resolves;
            }

            b.ageSeconds += dt;

            // Death: life mode, energy death, and horizon (τ) death.
            bool die = b.horizonRetire;
            if (! die)
            {
                if (sp.lifeIsBounces) die = (b.lifeBounceCount >= std::clamp (sp.lifeBounces, 1, 99));
                else                  die = (b.ageSeconds >= std::clamp (sp.lifeTimeSeconds, 0.1, 16.0));
            }
            const double vmag2 = dot (b.v, b.v);
            const double lowThr = 0.01 * vRef;
            if (vmag2 < lowThr * lowThr) b.lowSpeedSeconds += dt; else b.lowSpeedSeconds = 0.0;
            if (b.lowSpeedSeconds >= 0.25) die = true;

            const double tauEst = ((double) windowStart - (double) b.anchorSample
                                   - sp.motion * ((double) windowStart - (double) b.spawnSample)) / fs;
            if (tauEst > kHorizonSeconds) die = true;

            if (die) killOrRefeed (b, sp);
        }

        static double dragProfile (double x, double c) noexcept
        {
            // c>=0 -> lerp(1, 2x, c) ; c<0 -> lerp(1, 2(1-x), -c). Always >= 0.
            return (c >= 0.0) ? (1.0 + c * (2.0 * x - 1.0))
                              : (1.0 + (-c) * (1.0 - 2.0 * x));
        }

        bool findCollision (Vec2 pos, Vec2 posEnd, double thetaStart, double dAngle, double r, double& frac) const noexcept
        {
            Vec2 nrm, con;
            auto penAt = [&] (double f) -> double
            {
                const Vec2 c = add (pos, scale (sub (posEnd, pos), f));
                return boxPenetration (c, thetaStart + dAngle * f, r, nrm, con);
            };
            if (penAt (1.0) < 0.0) return false;        // no penetration by end of step
            if (penAt (0.0) >= 0.0) { frac = 0.0; return true; }   // already inside a wall -> push-out
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 8; ++i)
            {
                const double m = 0.5 * (lo + hi);
                if (penAt (m) < 0.0) lo = m; else hi = m;
            }
            frac = hi;
            return true;
        }

        void resolveWall (Ball& b, const SlotParams& sp, double r, double theta,
                          long long windowStart, double tElapsed) noexcept
        {
            Vec2 nOut, contact;
            const double depth = boxPenetration (b.pos, theta, r, nOut, contact);
            const Vec2 nIn = { -nOut.x, -nOut.y };      // inward normal

            // Moving-wall velocity at the contact point: v_wall = ω · perp(q - p).
            const Vec2 rq = sub (contact, pivot);
            const Vec2 vWall = { -omega * rq.y, omega * rq.x };
            const Vec2 rel = sub (b.v, vWall);
            const double vn = dot (rel, nIn);           // (v - v_wall) · n_in

            if (depth > 0.0) b.pos = add (b.pos, scale (nIn, depth));   // push-out only

            const bool reflect = (vn < 0.0);
            if (reflect)
            {
                const double e = std::clamp (sp.bounce, 0.0, 1.0);
                b.v = add (b.v, scale (nIn, -(1.0 + e) * vn));
                ++b.lifeBounceCount;                    // wall reflections count for Life=Bounces
                const double vmax = 8.0 * vRef;
                const double s2 = dot (b.v, b.v);
                if (s2 > vmax * vmax) b.v = scale (b.v, vmax / std::sqrt (s2));
            }
            b.hadCollision = true;
            b.lastCollisionPos = b.pos;

            // Grain firing: reflecting hits only, above the transient floor, and no
            // more often than every 15 ms per ball.
            const double impact = std::abs (vn);
            const long long nowSample = (long long) std::floor ((double) windowStart + tElapsed * fs);
            const bool fireGuard = (impact >= 0.02 * vRef) && ((nowSample - b.lastFireSample) >= minFireSamples);
            if (reflect && fireGuard) fireGrain (b, sp, contact, impact, nowSample);
        }

        void fireGrain (Ball& b, const SlotParams& sp, Vec2 contact, double impact, long long nowSample) noexcept
        {
            const int n = b.firedCount;                 // 0-based: first fired grain uses n = 0
            const double now = (double) nowSample;
            const double spray = (std::max (0.0, sp.sprayMs) * 1.0e-3 * fs) * nextRandBipolar();
            double d0 = (now - (double) b.anchorSample)
                      - std::clamp (sp.motion, -1.0, 1.0) * (now - (double) b.spawnSample)
                      - std::clamp (sp.stepSeconds, -0.5, 0.5) * fs * (double) n
                      - spray;

            if (d0 / fs > kHorizonSeconds) { b.horizonRetire = true; return; }   // read past horizon -> retire
            d0 = std::clamp (d0, 1.0, (double) ring.getSize() - 2.0);

            const double impactGain = std::min (1.0, impact / vRef);
            const double amp = impactGain * b.gainScale * std::max (0.0, sp.gainLinear);

            const double pitchTotal = sp.pitchSemis * (double) (b.generation + 1) + sp.pitchRandSemis * nextRandBipolar();
            const double rate = std::pow (2.0, pitchTotal / 12.0);
            const bool reverse = (nextRand01() < std::clamp (sp.reverseProb, 0.0, 1.0));
            const double dur = std::max (4.0, std::clamp (sp.grainMs, 4.0, 500.0) * 1.0e-3 * fs);

            double pan;
            switch (sp.panMode)
            {
                case PanMode::Physics: pan = std::clamp (contact.x, -1.0, 1.0); break;
                case PanMode::Center:  pan = 0.0; break;
                default:               pan = nextRandBipolar(); break;
            }
            const double th = (pan + 1.0) * 0.25 * kPi;
            queueGrainEvent (nowSample, d0, rate, reverse, dur, amp, std::cos (th), std::sin (th));
            pushHit (contact, b.slotIndex, (float) impactGain);

            b.lastFireSample = nowSample;
            ++b.firedCount;
        }

        void queueGrainEvent (long long startSample, double d0, double rate, bool reverse,
                              double duration, double gain, double gL, double gR) noexcept
        {
            int idx = -1;
            for (int i = 0; i < kEventCap; ++i) if (! events[i].active) { idx = i; break; }
            if (idx < 0) return;                        // queue full -> drop
            GEvent& e = events[idx];
            e.active = true; e.reverse = reverse; e.startSample = startSample;
            e.d0 = d0; e.rate = rate; e.duration = duration; e.gain = gain; e.gL = gL; e.gR = gR;
        }

        void activateDueGrains() noexcept
        {
            for (int i = 0; i < kEventCap; ++i)
            {
                if (! events[i].active || events[i].startSample > sampleClock) continue;
                int g = -1;
                for (int j = 0; j < kMaxGrains; ++j) if (! grains[j].active) { g = j; break; }
                if (g >= 0)
                {
                    Grain& gr = grains[g];
                    gr.active = true; gr.n = 0.0; gr.reverse = events[i].reverse;
                    gr.duration = events[i].duration; gr.d0 = events[i].d0; gr.rate = events[i].rate;
                    gr.gain = events[i].gain; gr.gL = events[i].gL; gr.gR = events[i].gR;
                }
                events[i].active = false;               // consume (grain dropped if pool full)
            }
        }

        void killOrRefeed (Ball& b, const SlotParams& sp) noexcept
        {
            // Natural death (not steal): deterministic Refeed. Reuse this slot as the
            // child so the ball count stays bounded and generations decay to silence.
            if (refeed > 0.0 && b.generation < 8 && b.gainScale * refeed >= 1.0e-3)
            {
                const int gen = b.generation + 1;
                const double gs = b.gainScale * refeed;
                const long long anchor = b.anchorSample;
                const Vec2 spawnPos = b.hadCollision ? b.lastCollisionPos : b.pos;

                b.generation = gen;
                b.gainScale = gs;
                b.anchorSample = anchor;
                b.spawnSample = sampleClock;
                b.ageSeconds = 0.0;
                b.lowSpeedSeconds = 0.0;
                b.lifeBounceCount = 0;
                b.firedCount = 0;
                b.hadCollision = false;
                b.horizonRetire = false;
                b.lastFireSample = sampleClock - minFireSamples - 1;
                b.pos = spawnPos;

                const double spd = std::clamp (sp.speed, 0.01, 8.0) * vRef;   // fresh speed, no bounce^k
                const double dirDeg = sp.directionDeg + std::clamp (sp.dirRandom, 0.0, 1.0) * (nextRandBipolar() * 180.0);
                const double dr = dirDeg * kPi / 180.0;
                b.v = { std::cos (dr) * spd, std::sin (dr) * spd };
                return;
            }
            b.active = false;                           // in-flight grains keep playing to their end
        }

        void resolveBallBall() noexcept
        {
            for (int i = 0; i < kMaxBalls; ++i)
            {
                if (! balls[i].active) continue;
                const double ri = ballRadius (slot[balls[i].slotIndex]);
                const double mi = ri * ri;
                for (int j = i + 1; j < kMaxBalls; ++j)
                {
                    if (! balls[j].active) continue;
                    const double rj = ballRadius (slot[balls[j].slotIndex]);
                    const Vec2 d = sub (balls[j].pos, balls[i].pos);
                    double dist = len (d);
                    const double sumr = ri + rj;
                    if (dist >= sumr || dist < 1e-9) continue;

                    const double mj = rj * rj;
                    const Vec2 nrm = scale (d, 1.0 / dist);
                    const double overlap = sumr - dist;
                    // Positional push-apart, mass-weighted.
                    balls[i].pos = sub (balls[i].pos, scale (nrm, overlap * (mj / (mi + mj))));
                    balls[j].pos = add (balls[j].pos, scale (nrm, overlap * (mi / (mi + mj))));

                    const double rel = dot (sub (balls[j].v, balls[i].v), nrm);
                    if (rel < 0.0)                      // approaching -> elastic exchange (e=1)
                    {
                        const double imp = -(2.0) * rel / (1.0 / mi + 1.0 / mj);
                        balls[i].v = sub (balls[i].v, scale (nrm, imp / mi));
                        balls[j].v = add (balls[j].v, scale (nrm, imp / mj));
                        const double vmax = 8.0 * vRef;
                        double s2 = dot (balls[i].v, balls[i].v);
                        if (s2 > vmax * vmax) balls[i].v = scale (balls[i].v, vmax / std::sqrt (s2));
                        s2 = dot (balls[j].v, balls[j].v);
                        if (s2 > vmax * vmax) balls[j].v = scale (balls[j].v, vmax / std::sqrt (s2));
                    }
                }
            }
        }

        // ------------------------------------------------------------------ UI mirror
        void writeSnapshot() noexcept
        {
            const int inactive = 1 - snapIndex.load (std::memory_order_relaxed);
            int c = 0, alive = 0;
            for (int i = 0; i < kMaxBalls; ++i)
            {
                if (! balls[i].active) continue;
                ++alive;
                if (c >= kMaxBalls) continue;
                const Ball& b = balls[i];
                const double vmag = len (b.v);
                BallView v;
                v.x = (float) b.pos.x; v.y = (float) b.pos.y;
                v.radius = (float) ballRadius (slot[b.slotIndex]);
                v.slot = b.slotIndex;
                v.energy = (float) std::clamp (vmag / (8.0 * vRef), 0.0, 1.0);
                v.generation = b.generation;
                snap[inactive][c++] = v;
            }
            snapCount[inactive].store (c, std::memory_order_relaxed);
            snapIndex.store (inactive, std::memory_order_release);
            aliveBallsAtomic.store (alive, std::memory_order_relaxed);
        }

        void pushHit (Vec2 q, int s, float intensity) noexcept
        {
            const std::uint64_t w = hitWrite.load (std::memory_order_relaxed);
            hits[(std::size_t) (w % kHitCap)] = { (float) q.x, (float) q.y, s, intensity };
            hitWrite.store (w + 1, std::memory_order_release);
        }

        // ------------------------------------------------------------------ state
        static constexpr int kPendingCap = 64;
        static constexpr int kEventCap   = 128;

        double fs = 44100.0;
        double tickSamples = 44.1;
        double tickAccum = 0.0;
        long long sampleClock = 0;
        long long minFireSamples = 662;
        double cAudio = 0.0, cPhys = 0.0;

        // Box shape / rotation.
        int  shapeN = 4;
        bool isCircle = false;
        std::array<Vec2, 8> baseV {};
        double boxTheta = 0.0;

        // Smoothing targets and smoothed values.
        double boxSizeTarget = 0.40, spinTarget = 0.0, pivotXTarget = 0.0, pivotYTarget = 0.0, grav01Target = 0.0;
        double boxSizeS = 0.40, spinS = 0.0, pivotXS = 0.0, pivotYS = 0.0, grav01S = 0.0;
        double mixTarget = 0.35, toneHzTarget = 12000.0;
        double mixS = 0.35, toneHzS = 12000.0;

        // Derived per tick.
        double vRef = 5.0, omega = 0.0, gravity01 = 0.0;
        Vec2 pivot {};

        // Global scalars.
        bool   ballCollide = false;
        double senseDb = -30.0, retrigMs = 150.0, spawnSpread = 0.10, refeed = 0.0;

        // Onset.
        EnvelopeFollower onset;
        bool armed = true;
        long long lastOnsetSample = 0;

        // Tone.
        OnePole toneL, toneR;

        std::uint32_t rngState = 0x9e3779b9u;

        StereoRing ring;
        std::array<Ball, kMaxBalls>      balls {};
        std::array<Grain, kMaxGrains>    grains {};
        std::array<Pending, kPendingCap> pending {};
        std::array<GEvent, kEventCap>    events {};
        std::array<SlotParams, kNumSlots> slot {};

        // UI mirror (lock-free).
        std::array<std::array<BallView, kMaxBalls>, 2> snap {};
        std::array<std::atomic<int>, 2> snapCount {};
        std::atomic<int> snapIndex { 0 };
        std::array<HitEvent, kHitCap> hits {};
        std::atomic<std::uint64_t> hitWrite { 0 }, hitRead { 0 };
        std::atomic<int> aliveBallsAtomic { 0 }, activeGrainsAtomic { 0 };
        std::atomic<double> boxAngleAtomic { 0.0 };
    };
} // namespace factory_core
