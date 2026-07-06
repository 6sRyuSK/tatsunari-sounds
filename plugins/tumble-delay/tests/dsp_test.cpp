//
// dsp_test.cpp — headless verification of the tumble-delay DSP core.
//
// Spec-based tests T1–T12 (docs/plans/physics-granular-delay.md §10 and
// docs/plans/tumble-delay-handoff.md §3). Every quantitative check uses an
// INDEPENDENT closed-form oracle (billiard unfolding, projectile motion,
// geometric decay, equal-power pan law) — never a value derived from the engine
// under test. All signal lengths / times / frequencies are derived from Fs at
// runtime; the rate matrix is factory_core::testing::sampleRatesFromArgs().
//
// The engine (factory_core::TumbleDelay) has no audio feedback loop; the "tail"
// is a purely feed-forward trigger sequence + Refeed generation spawn. Physics
// runs on a fixed 1 kHz tick refined to sub-sample precision, so collision
// timing is sample-rate independent (measured ±0.55 samples on intervals).
//
#include "factory_core/TumbleDelay.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace fct = factory_core::testing;
    using factory_core::TumbleDelay;
    using Slot  = TumbleDelay::SlotParams;
    using Shape = TumbleDelay::Shape;
    using Pan   = TumbleDelay::PanMode;

    int  g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }
    void note (const std::string& m) { std::printf ("    %s\n", m.c_str()); }

    constexpr double kPi = 3.14159265358979323846;

    // ------------------------------------------------------------------ geometry
    // Square baseline, flat-bottom convention: apothem a = cos(pi/N) with N = 4.
    // The billiard domain for the ball CENTER is the inset square [-L, L]^2 with
    // L = a - ballSize. Reference speed vRef = 2 / boxSize.
    double apothemSquare()            { return std::cos (kPi / 4.0); }
    double insetL (double ballSize)   { return apothemSquare() - ballSize; }
    double vRefFor (double boxSize)   { return 2.0 / boxSize; }

    // ------------------------------------------------------------------ baseline
    // NOTE: SlotParams defaults are NOT the deterministic test baseline
    // (drag defaults 0.10, dirRandom defaults 1.0). Zero the stochastic /
    // dissipative fields explicitly.
    Slot makeBaselineSlot()
    {
        Slot s;
        s.enabled         = true;
        s.count           = 1;
        s.ballSize        = 0.08;
        s.speed           = 1.0;
        s.directionDeg    = 90.0;   // straight up
        s.dirRandom       = 0.0;    // deterministic (default is 1.0!)
        s.preDelaySeconds = 0.0;
        s.timeSeconds     = 0.35;
        s.bounce          = 0.70;
        s.drag            = 0.0;    // no dissipation (default is 0.10!)
        s.decayCurve      = 0.0;
        s.lifeIsBounces   = false;
        s.lifeTimeSeconds = 3.0;
        s.lifeBounces     = 12;
        s.pitchSemis      = 0.0;
        s.pitchRandSemis  = 0.0;
        s.grainMs         = 90.0;
        s.reverseProb     = 0.0;
        s.motion          = 0.0;
        s.stepSeconds     = 0.0;
        s.sprayMs         = 0.0;
        s.panMode         = Pan::Center;
        s.gainLinear      = 1.0;
        return s;
    }

    void applyGlobalBaseline (TumbleDelay& e)
    {
        e.setBoxShape (Shape::Square);
        e.setBoxSizeSeconds (0.40);   // vRef = 5
        e.setSpinRevPerSec (0.0);
        e.setPivot (0.0, 0.0);
        e.setGravity (0.0);
        e.setBallCollide (false);
        e.setSenseDb (-30.0);
        e.setRetrigMs (150.0);
        e.setSpawnSpread (0.0);
        e.setRefeed (0.0);
        e.setToneHz (20000.0);        // near-Nyquist: short LP tail so gaps read as silence
        e.setMix (1.0);               // output = wet only
    }

    // Canonical harness order: prepare -> setters -> reset (snaps smoothed params
    // to targets, reseeds RNG) -> feed signal.
    void arm (TumbleDelay& e, double fs, const Slot& s0)
    {
        e.prepare (fs);
        applyGlobalBaseline (e);
        e.setSlotParams (0, s0);
        e.reset();
    }

    // ------------------------------------------------------------------ signals
    std::vector<double> makeBurstKernel (double fs, double freq = 220.0,
                                         double amp = 0.5, double ms = 10.0)
    {
        const size_t n = (size_t) (ms * 1.0e-3 * fs);
        std::vector<double> b (std::max<size_t> (1, n));
        for (size_t i = 0; i < b.size(); ++i)
        {
            const double ph = (double) i / (double) b.size();
            const double w  = 0.5 - 0.5 * std::cos (2.0 * kPi * ph);     // Hann (zero start)
            b[i] = amp * w * std::sin (2.0 * kPi * freq * (double) i / fs);
        }
        return b;
    }

    // Add a Hann-windowed sine burst into L/R at time tStart (identical channels).
    void addBurst (std::vector<double>& L, std::vector<double>& R, double fs,
                   double tStart, double freq = 220.0, double amp = 0.5, double ms = 10.0)
    {
        const auto k = makeBurstKernel (fs, freq, amp, ms);
        const size_t s = (size_t) (tStart * fs);
        for (size_t i = 0; i < k.size() && s + i < L.size(); ++i)
        {
            L[s + i] += k[i];
            R[s + i] += k[i];
        }
    }

    // ------------------------------------------------------------------ render
    void renderInto (TumbleDelay& e, const std::vector<double>& inL, const std::vector<double>& inR,
                     std::vector<double>& outL, std::vector<double>& outR)
    {
        outL.resize (inL.size());
        outR.resize (inR.size());
        for (size_t i = 0; i < inL.size(); ++i)
        {
            double l = inL[i], r = inR[i];
            e.processStereo (l, r);
            outL[i] = l;
            outR[i] = r;
        }
    }

    // ------------------------------------------------------------------ analysis
    // Grain onset = first |out| > thr after >= gap consecutive quiet samples.
    std::vector<size_t> grainOnsets (const std::vector<double>& y,
                                     double thr = 1.0e-9, size_t gap = 100)
    {
        std::vector<size_t> on;
        size_t quiet = gap;
        for (size_t i = 0; i < y.size(); ++i)
        {
            if (std::abs (y[i]) <= thr) { ++quiet; }
            else { if (quiet >= gap) on.push_back (i); quiet = 0; }
        }
        return on;
    }

    double peakWin (const std::vector<double>& y, size_t start, size_t len)
    {
        double p = 0.0;
        for (size_t i = start; i < start + len && i < y.size(); ++i)
            p = std::max (p, std::abs (y[i]));
        return p;
    }

    // Count sign changes of the windowed grain over its window -> ~2*f*len zero
    // crossings. The grain is doubly Hann-tapered (burst window x grain window),
    // so the gate must be well below the peak to reach the low-amplitude edges
    // while still rejecting sub-null numerical fuzz (~1e-15).
    int zeroCrossings (const std::vector<double>& y, size_t start, size_t len)
    {
        const double pk = peakWin (y, start, len);
        if (pk <= 0.0) return 0;
        const double t = 1.0e-6 * pk;
        int c = 0, prev = 0;
        for (size_t i = start; i < start + len && i < y.size(); ++i)
        {
            const double v = y[i];
            if (std::abs (v) < t) continue;
            const int s = (v > 0.0) ? 1 : -1;
            if (prev != 0 && s != prev) ++c;
            prev = s;
        }
        return c;
    }

    bool bitEqual (const std::vector<double>& a, const std::vector<double>& b)
    {
        return a.size() == b.size()
            && std::memcmp (a.data(), b.data(), a.size() * sizeof (double)) == 0;
    }

    // Test-local RNG (independent of the engine's) for the noise-floor silence test.
    struct XorShift32 { std::uint32_t s = 0x1234567u;
        double bipolar() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (double) s / 2147483648.0 - 1.0; } };

    // ================================================================= T1a
    // Vertical billiard: consecutive grain-onset intervals == 2L/v (round to
    // samples), +/- 2.5 samples, for >= 6 intervals.
    void t1a (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0; s.grainMs = 10.0;

        TumbleDelay e; arm (e, Fs, s);

        const double v = 1.0 * vRefFor (0.40);
        const double L = insetL (s.ballSize);
        const double expInt = 2.0 * L / v;                    // seconds
        const double expSamp = expInt * Fs;

        std::vector<double> inL (size_t (2.5 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 7) { fail ("T1a: too few grain onsets (" + std::to_string (on.size()) + ")"); return; }

        int bad = 0; double worst = 0.0;
        for (int i = 0; i < 6; ++i)
        {
            const double meas = (double) (on[(size_t) i + 1] - on[(size_t) i]);
            const double err  = std::abs (meas - expSamp);
            worst = std::max (worst, err);
            if (err > 2.5) ++bad;
        }
        if (bad > 0)
            fail ("T1a vertical billiard: " + std::to_string (bad) + "/6 intervals off (worst "
                  + std::to_string (worst) + " samp, expected " + std::to_string (expSamp) + ")");
        else
            note ("T1a ok: interval " + std::to_string (expSamp) + " samp, worst err " + std::to_string (worst));
    }

    // ================================================================= T1b
    // Diagonal billiard (unfolding): per-axis wall-hit times merged ascending.
    void t1b (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.directionDeg = 60.0; s.bounce = 1.0;
        s.lifeTimeSeconds = 16.0; s.grainMs = 10.0;

        TumbleDelay e; arm (e, Fs, s);

        const double v  = 1.0 * vRefFor (0.40);
        const double L  = insetL (s.ballSize);
        const double vx = v * std::cos (60.0 * kPi / 180.0);
        const double vy = v * std::sin (60.0 * kPi / 180.0);
        const double Tmax = 2.0;

        std::vector<double> ev;
        for (int j = 1; (2 * j - 1) * L / vx < Tmax; ++j) ev.push_back ((2 * j - 1) * L / vx);
        for (int k = 1; (2 * k - 1) * L / vy < Tmax; ++k) ev.push_back ((2 * k - 1) * L / vy);
        std::sort (ev.begin(), ev.end());
        if (ev.size() < 8) { fail ("T1b: oracle produced < 8 events"); return; }

        // Oracle intervals for the first 8 events; verify all > 20 ms (min-fire guard).
        std::vector<double> oiv;
        for (int i = 0; i < 7; ++i)
        {
            const double g = ev[(size_t) i + 1] - ev[(size_t) i];
            if (g < 0.020) { fail ("T1b: oracle event gap < 20 ms (unexpected)"); return; }
            oiv.push_back (g);
        }

        std::vector<double> inL (size_t (2.5 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 7) { fail ("T1b: too few grain onsets (" + std::to_string (on.size()) + ")"); return; }

        int bad = 0; double worst = 0.0;
        for (int i = 0; i < 6; ++i)
        {
            const double meas = (double) (on[(size_t) i + 1] - on[(size_t) i]);
            const double err  = std::abs (meas - oiv[(size_t) i] * Fs);
            worst = std::max (worst, err);
            if (err > 2.5) ++bad;
        }
        if (bad > 0)
            fail ("T1b diagonal billiard: " + std::to_string (bad) + "/6 intervals off (worst "
                  + std::to_string (worst) + " samp)");
        else
            note ("T1b ok: worst interval err " + std::to_string (worst) + " samp");
    }

    // ================================================================= T2
    // Gravity bounce: floor-hit intervals and grain-peak ratios are geometric
    // with ratio e = 0.7. Engine integrates with 1 ms Euler -> assert RATIOS.
    void t2 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.directionDeg = 270.0; s.speed = 0.25; s.bounce = 0.7;
        s.lifeTimeSeconds = 16.0; s.grainMs = 10.0;

        TumbleDelay e;
        e.prepare (Fs); applyGlobalBaseline (e);
        e.setGravity (0.5);                       // 4 R/s^2
        e.setSlotParams (0, s); e.reset();

        std::vector<double> inL (size_t (4.0 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 7) { fail ("T2: too few floor-hit grains (" + std::to_string (on.size()) + ")"); return; }

        const size_t win = (size_t) (0.013 * Fs);
        std::vector<double> iv, pk;
        for (size_t i = 0; i + 1 < on.size(); ++i) iv.push_back ((double) (on[i + 1] - on[i]));
        for (size_t i = 0; i < on.size(); ++i)     pk.push_back (peakWin (oL, on[i], win));

        int bad = 0;
        for (int i = 0; i < 4; ++i)   // interval ratios
        {
            const double ratio = iv[(size_t) i + 1] / iv[(size_t) i];
            if (std::abs (ratio - 0.7) > 0.02 * 0.7 + 1e-9) { ++bad;
                note ("T2 interval ratio[" + std::to_string (i) + "]=" + std::to_string (ratio)); }
        }
        for (int i = 0; i < 4; ++i)   // peak ratios
        {
            const double ratio = pk[(size_t) i + 1] / pk[(size_t) i];
            if (std::abs (ratio - 0.7) > 0.02 * 0.7 + 1e-9) { ++bad;
                note ("T2 peak ratio[" + std::to_string (i) + "]=" + std::to_string (ratio)); }
        }
        if (bad > 0) fail ("T2 gravity bounce: " + std::to_string (bad) + " ratios outside 0.7 +/- 2%");
        else         note ("T2 ok: interval & peak ratios ~0.7");
    }

    // ================================================================= T3
    // Refeed generation ratio: child grain / parent grain peak = 0.5 (+/- 1%).
    void t3 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0; s.lifeIsBounces = true; s.lifeBounces = 2;
        s.grainMs = 10.0;

        TumbleDelay e;
        e.prepare (Fs); applyGlobalBaseline (e);
        e.setRefeed (0.5);
        e.setSlotParams (0, s); e.reset();

        std::vector<double> inL (size_t (1.5 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 3) { fail ("T3: expected >= 3 grain onsets, got " + std::to_string (on.size())); return; }

        const size_t win = (size_t) (0.013 * Fs);
        const double pParent = peakWin (oL, on[0], win);   // parent grain #1 (amp 1.0)
        const double pChild  = peakWin (oL, on[2], win);   // child  grain #1 (amp 0.5)
        const double ratio = pChild / pParent;
        if (std::abs (ratio - 0.5) > 0.01 * 0.5 + 1e-12)
            fail ("T3 refeed ratio: " + std::to_string (ratio) + " (expected 0.5 +/- 1%)");
        else
            note ("T3 ok: child/parent peak = " + std::to_string (ratio));
    }

    // ================================================================= T4
    // Worst-case boundedness (class A/C/H): 4 slots x count 8, spin +2, refeed
    // 0.95, e=1, drag 0. One burst, hold 12 s -> allFinite + peakAbs < 4.0.
    // lifeTime = 2 s (< hold) so balls DIE during the hold and the Refeed 0.95
    // generation chain actually runs (gen ~6 by 12 s) — with a 16 s life no ball
    // would die and the generation-stacking path would go untested.
    void t4 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.count = 8; s.speed = 1.0; s.directionDeg = 90.0; s.dirRandom = 0.0;
        s.bounce = 1.0; s.drag = 0.0; s.lifeTimeSeconds = 2.0; s.grainMs = 90.0;
        s.panMode = Pan::Physics;

        TumbleDelay e;
        e.prepare (Fs); applyGlobalBaseline (e);
        e.setSpinRevPerSec (2.0);
        e.setGravity (0.0);
        e.setRefeed (0.95);
        e.setSpawnSpread (0.0);
        for (int i = 0; i < 4; ++i) e.setSlotParams (i, s);
        e.reset();

        const auto burst = makeBurstKernel (Fs);
        const size_t N = (size_t) (12.0 * Fs);
        const size_t at10 = (size_t) (10.0 * Fs);   // gen ~5 of the refeed chain
        std::vector<double> oL (N), oR (N);
        int alive10 = -1;
        for (size_t i = 0; i < N; ++i)
        {
            const double in = (i < burst.size()) ? burst[i] : 0.0;
            double l = in, r = in;
            e.processStereo (l, r);
            oL[i] = l; oR[i] = r;
            if (i == at10) alive10 = e.aliveBalls();
        }
        const bool finite = fct::allFinite (oL) && fct::allFinite (oR);
        const double peak = std::max (fct::peakAbs (oL), fct::peakAbs (oR));
        if (! finite)      fail ("T4: non-finite output during 12 s hold");
        if (peak >= 4.0)   fail ("T4: peak " + std::to_string (peak) + " >= 4.0 (worst-case bound)");
        if (alive10 <= 0)  fail ("T4: refeed chain dead at 10 s (alive=" + std::to_string (alive10)
                                 + ") — generation-stacking path not exercised");
        if (finite && peak < 4.0 && alive10 > 0)
            note ("T4 ok: finite, peak " + std::to_string (peak) + " < 4.0, alive@10s=" + std::to_string (alive10));
    }

    // ================================================================= T5
    // Silence / absolute floor (class J): exact digital silence and -90 dBFS
    // noise -> output exactly 0.0 (mix=1 wet is identically zero), aliveBalls==0.
    void t5 (double Fs)
    {
        auto runSilence = [&] (bool noise, const std::string& tag)
        {
            Slot s = makeBaselineSlot();
            TumbleDelay e;
            e.prepare (Fs); applyGlobalBaseline (e);
            e.setSenseDb (-60.0);
            e.setSlotParams (0, s); e.reset();

            XorShift32 rng;
            const double amp = std::pow (10.0, -90.0 / 20.0);   // ~3.16e-5
            const size_t N = (size_t) (2.0 * Fs);
            const size_t probe = std::max<size_t> (1, (size_t) (0.1 * Fs));

            bool nonzero = false; int aliveMax = 0;
            for (size_t i = 0; i < N; ++i)
            {
                double x = noise ? amp * rng.bipolar() : 0.0;
                double l = x, r = x;
                e.processStereo (l, r);
                if (l != 0.0 || r != 0.0) nonzero = true;
                if ((i % probe) == 0) aliveMax = std::max (aliveMax, e.aliveBalls());
            }
            if (nonzero)     fail ("T5" + tag + ": wet output not exactly 0.0");
            if (aliveMax > 0) fail ("T5" + tag + ": spawned a ball (" + std::to_string (aliveMax) + ")");
            if (! nonzero && aliveMax == 0) note ("T5" + tag + " ok: silent, no spawn");
        };
        runSilence (false, "a-silence");
        runSilence (true,  "b-noise");
    }

    // ================================================================= T6
    // Determinism with stochastic paths active: two runs (reset between) with
    // identical input -> bitwise identical output.
    void t6 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.dirRandom = 1.0; s.sprayMs = 100.0; s.pitchRandSemis = 3.0;
        s.reverseProb = 0.5; s.panMode = Pan::Random; s.bounce = 0.8;
        s.lifeTimeSeconds = 2.0; s.count = 3;

        std::vector<double> inL (size_t (2.5 * Fs), 0.0), inR = inL;
        addBurst (inL, inR, Fs, 0.0);

        auto run = [&] (std::vector<double>& oL, std::vector<double>& oR)
        {
            TumbleDelay e;
            e.prepare (Fs); applyGlobalBaseline (e);
            e.setSpinRevPerSec (1.0);
            e.setGravity (0.3);
            e.setSpawnSpread (0.3);
            e.setSlotParams (0, s); e.reset();
            renderInto (e, inL, inR, oL, oR);
        };
        std::vector<double> a, b, c, d;
        run (a, b); run (c, d);
        if (! bitEqual (a, c) || ! bitEqual (b, d))
            fail ("T6: stochastic render not bit-reproducible");
        else
            note ("T6 ok: bit-identical across runs");
    }

    // ================================================================= T7
    // Reset clears state (class E): run -> reset -> silence is exactly 0 with
    // aliveBalls==0 -> re-run bit-identical to the first run.
    void t7 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0;                 // vertical baseline

        TumbleDelay e; arm (e, Fs, s);

        std::vector<double> inL (size_t (1.5 * Fs), 0.0), inR = inL, r1L, r1R;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, r1L, r1R);            // run 1

        e.reset();

        std::vector<double> silL (size_t (0.5 * Fs), 0.0), silR = silL, soL, soR;
        bool resid = false; int aliveMax = 0;
        for (size_t i = 0; i < silL.size(); ++i)
        {
            double l = 0.0, r = 0.0;
            e.processStereo (l, r);
            if (l != 0.0 || r != 0.0) resid = true;
            aliveMax = std::max (aliveMax, e.aliveBalls());
        }
        if (resid)        fail ("T7: residual output after reset()");
        if (aliveMax > 0) fail ("T7: balls alive after reset() (" + std::to_string (aliveMax) + ")");

        std::vector<double> r2L, r2R;
        renderInto (e, inL, inR, r2L, r2R);            // run 2
        if (! bitEqual (r1L, r2L) || ! bitEqual (r1R, r2R))
            fail ("T7: post-reset re-run not bit-identical to first run");
        else if (! resid && aliveMax == 0)
            note ("T7 ok: clean reset + bit-identical re-run");
    }

    // ================================================================= T8a
    // Horizon retire via Step (class D): step=-0.5 pushes tau ~9x age; horizon
    // (33 s) reached ~age 3.7 s. aliveBalls > 0 at 3.0 s, == 0 at 4.5 s.
    void t8a (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 4.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0;
        s.stepSeconds = -0.5; s.grainMs = 10.0;

        TumbleDelay e; arm (e, Fs, s);

        const auto burst = makeBurstKernel (Fs);
        const size_t N = (size_t) (6.0 * Fs);
        const size_t at30 = (size_t) (3.0 * Fs);
        const size_t at45 = (size_t) (4.5 * Fs);
        std::vector<double> oL (N), oR (N);
        int alive30 = -1, alive45 = -1;
        for (size_t i = 0; i < N; ++i)
        {
            const double in = (i < burst.size()) ? burst[i] : 0.0;
            double l = in, r = in;
            e.processStereo (l, r);
            oL[i] = l; oR[i] = r;
            if (i == at30) alive30 = e.aliveBalls();
            if (i == at45) alive45 = e.aliveBalls();
        }
        const bool finite = fct::allFinite (oL) && fct::allFinite (oR);
        if (! finite)       fail ("T8a: non-finite output");
        if (alive30 <= 0)   fail ("T8a: ball already dead at 3.0 s (alive=" + std::to_string (alive30) + ")");
        if (alive45 != 0)   fail ("T8a: ball not horizon-retired by 4.5 s (alive=" + std::to_string (alive45) + ")");
        if (finite && alive30 > 0 && alive45 == 0)
            note ("T8a ok: alive@3.0=" + std::to_string (alive30) + ", alive@4.5=0");
    }

    // ================================================================= T8b
    // Time clamp (class D): timeSeconds=5.0 clamps to 2.0 -> ball 2 grain fires
    // 2.000 s after ball 1 (+/- 3 ms). bounce=1 so both balls fly identically.
    void t8b (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.count = 2; s.timeSeconds = 5.0; s.bounce = 1.0;
        s.lifeIsBounces = true; s.lifeBounces = 1; s.grainMs = 10.0;

        TumbleDelay e; arm (e, Fs, s);

        std::vector<double> inL (size_t (2.5 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() != 2) { fail ("T8b: expected exactly 2 grains, got " + std::to_string (on.size())); return; }

        const double meas = (double) (on[1] - on[0]);
        const double err  = std::abs (meas - 2.0 * Fs);
        if (err > 0.003 * Fs)
            fail ("T8b time clamp: grain gap " + std::to_string (meas / Fs) + " s (expected 2.000 +/- 0.003)");
        else
            note ("T8b ok: grain gap err " + std::to_string (err) + " samp");
    }

    // ================================================================= T9
    // Physics pan law: contact.x -> equal-power pan. (a) vertical top x=0 -> 1;
    // (b) direction 0 right wall x=apothem -> gL/gR = cos(th)/sin(th).
    void t9 (double Fs)
    {
        auto oneGrain = [&] (double dirDeg, std::vector<double>& oL, std::vector<double>& oR, std::vector<size_t>& on)
        {
            Slot s = makeBaselineSlot();
            s.directionDeg = dirDeg; s.bounce = 1.0; s.grainMs = 10.0;
            s.lifeIsBounces = true; s.lifeBounces = 1; s.panMode = Pan::Physics;
            TumbleDelay e; arm (e, Fs, s);
            std::vector<double> inL (size_t (0.5 * Fs), 0.0), inR = inL;
            addBurst (inL, inR, Fs, 0.0);
            renderInto (e, inL, inR, oL, oR);
            on = grainOnsets (oL);
        };
        const size_t win = (size_t) (0.013 * Fs);

        // (a) vertical -> centre pan.
        {
            std::vector<double> oL, oR; std::vector<size_t> on;
            oneGrain (90.0, oL, oR, on);
            if (on.empty()) { fail ("T9a: no grain"); }
            else
            {
                const double pl = peakWin (oL, on[0], win), pr = peakWin (oR, on[0], win);
                const double ratio = pl / pr;
                if (std::abs (ratio - 1.0) > 1.0e-6)
                    fail ("T9a centre pan: peakL/peakR = " + std::to_string (ratio) + " (expected 1)");
                else note ("T9a ok: peakL/peakR = " + std::to_string (ratio));
            }
        }
        // (b) direction 0 -> right wall, contact.x = apothem.
        {
            std::vector<double> oL, oR; std::vector<size_t> on;
            oneGrain (0.0, oL, oR, on);
            if (on.empty()) { fail ("T9b: no grain"); }
            else
            {
                const double pan = std::clamp (apothemSquare(), -1.0, 1.0);
                const double th  = (pan + 1.0) * 0.25 * kPi;
                const double expected = std::cos (th) / std::sin (th);
                const double pl = peakWin (oL, on[0], win), pr = peakWin (oR, on[0], win);
                const double ratio = pl / pr;
                if (std::abs (ratio - expected) > 0.01 * expected)
                    fail ("T9b physics pan: gL/gR = " + std::to_string (ratio)
                          + " (expected " + std::to_string (expected) + ")");
                else note ("T9b ok: gL/gR = " + std::to_string (ratio) + " ~ " + std::to_string (expected));
            }
        }
    }

    // ================================================================= T10
    // Life Bounces: exactly 5 grain onsets, then aliveBalls==0.
    void t10 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0; s.lifeIsBounces = true; s.lifeBounces = 5;
        s.grainMs = 10.0;

        TumbleDelay e; arm (e, Fs, s);

        std::vector<double> inL (size_t (3.0 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() != 5)
            fail ("T10 life bounces: expected 5 grains, got " + std::to_string (on.size()));
        if (e.aliveBalls() != 0)
            fail ("T10: ball still alive at end (" + std::to_string (e.aliveBalls()) + ")");
        if (on.size() == 5 && e.aliveBalls() == 0)
            note ("T10 ok: 5 grains then dead");
    }

    // ================================================================= T11
    // Trigger sequence (D11 initial-speed decay): ball k grain time offset and
    // peak ratio follow the closed form.
    void t11 (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.count = 4; s.timeSeconds = 0.35; s.bounce = 0.7; s.speed = 1.0;
        s.lifeIsBounces = true; s.lifeBounces = 1; s.grainMs = 10.0; s.motion = 0.0;

        TumbleDelay e; arm (e, Fs, s);

        const double v = 1.0 * vRefFor (0.40);
        const double L = insetL (s.ballSize);
        const double flight1 = L / v;

        std::vector<double> inL (size_t (2.0 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 4) { fail ("T11: expected 4 grains, got " + std::to_string (on.size())); return; }

        const size_t win = (size_t) (0.013 * Fs);
        const double p1 = peakWin (oL, on[0], win);
        int badT = 0, badP = 0;
        for (int k = 2; k <= 4; ++k)
        {
            const double oracleDt = (double) (k - 1) * 0.35
                                  + flight1 * (std::pow (0.7, -(double) (k - 1)) - 1.0);
            const double measDt = (double) (on[(size_t) (k - 1)] - on[0]) / Fs;
            if (std::abs (measDt - oracleDt) > 0.0025) { ++badT;
                note ("T11 dt[k=" + std::to_string (k) + "] meas " + std::to_string (measDt)
                      + " oracle " + std::to_string (oracleDt)); }

            const double pk = peakWin (oL, on[(size_t) (k - 1)], win);
            const double ratio = pk / p1;
            const double expected = std::pow (0.7, (double) (k - 1));
            if (std::abs (ratio - expected) > 0.02 * expected) { ++badP;
                note ("T11 peak[k=" + std::to_string (k) + "] ratio " + std::to_string (ratio)
                      + " expected " + std::to_string (expected)); }
        }
        if (badT > 0) fail ("T11: " + std::to_string (badT) + " grain times off (> 2.5 ms)");
        if (badP > 0) fail ("T11: " + std::to_string (badP) + " peak ratios off (> 2%)");
        if (badT == 0 && badP == 0) note ("T11 ok: D11 times & peak ratios match");
    }

    // ================================================================= T12a
    // Motion: input = 220 Hz tone with linear amplitude ramp 0.5->0 over 4 s.
    // (i) motion=0 -> grain peaks ~equal (max/min <= 1.10). (ii) motion=+1,
    // preDelay=0.2 -> tau=0.2 constant -> peaks track ramp(t_n - 0.2).
    void t12a (double Fs)
    {
        auto ramp = [] (double t) { return (t >= 0.0 && t <= 4.0) ? 0.5 * (1.0 - t / 4.0) : 0.0; };
        auto makeRampTone = [&] () {
            std::vector<double> x (size_t (4.5 * Fs), 0.0);
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / Fs;
                x[i] = ramp (t) * std::sin (2.0 * kPi * 220.0 * t);
            }
            return x;
        };
        const std::vector<double> tone = makeRampTone();
        const size_t win = (size_t) (0.095 * Fs);

        // (i) motion = 0 -> all grains read the birth transient -> equal peaks.
        {
            Slot s = makeBaselineSlot();
            s.speed = 1.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0; s.grainMs = 90.0;
            s.motion = 0.0;
            TumbleDelay e;
            e.prepare (Fs); applyGlobalBaseline (e);
            e.setRetrigMs (2000.0);
            e.setSlotParams (0, s); e.reset();

            std::vector<double> oL, oR; renderInto (e, tone, tone, oL, oR);
            auto on = grainOnsets (oL);
            double lo = 1e30, hi = 0.0; int n = 0;
            for (size_t idx = 0; idx < on.size(); ++idx)
            {
                const double t = (double) on[idx] / Fs;
                if (t < 0.3 || t > 3.5) continue;
                const double p = peakWin (oL, on[idx], win);
                lo = std::min (lo, p); hi = std::max (hi, p); ++n;
            }
            if (n < 4)             fail ("T12a(i): too few grains in [0.3,3.5] (" + std::to_string (n) + ")");
            else if (hi / lo > 1.10) fail ("T12a(i) motion=0: peak max/min = " + std::to_string (hi / lo) + " > 1.10");
            else                   note ("T12a(i) ok: peak max/min = " + std::to_string (hi / lo));
        }
        // (ii) motion = +1, preDelay = 0.2 -> peaks track ramp(t_n - 0.2).
        {
            Slot s = makeBaselineSlot();
            s.speed = 1.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0; s.grainMs = 90.0;
            s.motion = 1.0; s.preDelaySeconds = 0.2;
            TumbleDelay e;
            e.prepare (Fs); applyGlobalBaseline (e);
            e.setRetrigMs (2000.0);
            e.setSlotParams (0, s); e.reset();

            std::vector<double> oL, oR; renderInto (e, tone, tone, oL, oR);
            auto on = grainOnsets (oL);
            std::vector<double> tt, pp;
            for (size_t idx = 0; idx < on.size(); ++idx)
            {
                const double t = (double) on[idx] / Fs;
                if (t < 0.3 || t > 3.9) continue;
                tt.push_back (t);
                pp.push_back (peakWin (oL, on[idx], win));
            }
            if (tt.size() < 4) { fail ("T12a(ii): too few grains (" + std::to_string (tt.size()) + ")"); }
            else
            {
                // tau = preDelay = 0.2 s constant, so the grain reads the input at
                // (t_n - 0.2). The Hann grain's peak sample sits grainMs/2 into its
                // span, so the peak reads ramp at (t_n - 0.2 + grainMs/2) — evaluate
                // the independent ramp oracle there.
                const double half = 0.5 * 0.090;
                const double ref  = pp[0];
                const double rref = ramp (tt[0] - 0.2 + half);
                int bad = 0;
                for (size_t i = 1; i < tt.size(); ++i)
                {
                    const double measRatio   = pp[i] / ref;
                    const double oracleRatio = ramp (tt[i] - 0.2 + half) / rref;
                    if (std::abs (measRatio - oracleRatio) > 0.10 * oracleRatio) { ++bad;
                        note ("T12a(ii) grain@" + std::to_string (tt[i]) + " meas " + std::to_string (measRatio)
                              + " oracle " + std::to_string (oracleRatio)); }
                }
                if (bad > 0) fail ("T12a(ii) motion tracking: " + std::to_string (bad) + " peaks off > 10%");
                else         note ("T12a(ii) ok: peaks track ramp(t-0.2)");
            }
        }
    }

    // ================================================================= T12b
    // Step (phrase scanner): grains n=0,1,2 read anchor+0.12*n -> 220/440/880 Hz.
    // Zero crossings in each 10 ms grain window ~ 2*f*0.010.
    void t12b (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0;
        s.stepSeconds = 0.12; s.grainMs = 10.0;

        TumbleDelay e;
        e.prepare (Fs); applyGlobalBaseline (e);
        e.setRetrigMs (2000.0);       // only burst A triggers
        e.setSlotParams (0, s); e.reset();

        std::vector<double> inL (size_t (1.0 * Fs), 0.0), inR = inL, oL, oR;
        addBurst (inL, inR, Fs, 0.00, 220.0, 0.5, 12.0);
        addBurst (inL, inR, Fs, 0.12, 440.0, 0.5, 12.0);
        addBurst (inL, inR, Fs, 0.24, 880.0, 0.5, 12.0);
        renderInto (e, inL, inR, oL, oR);

        auto on = grainOnsets (oL);
        if (on.size() < 3) { fail ("T12b: expected >= 3 grains, got " + std::to_string (on.size())); return; }

        const size_t win = (size_t) (0.010 * Fs);
        const double freqs[3] = { 220.0, 440.0, 880.0 };
        int bad = 0;
        for (int n = 0; n < 3; ++n)
        {
            const int zc = zeroCrossings (oL, on[(size_t) n], win);
            const double expected = 2.0 * freqs[n] * 0.010;
            if (std::abs ((double) zc - expected) > 0.30 * expected) { ++bad;
                note ("T12b grain " + std::to_string (n) + ": zc=" + std::to_string (zc)
                      + " expected ~" + std::to_string (expected)); }
        }
        if (bad > 0) fail ("T12b step scanner: " + std::to_string (bad) + "/3 grains wrong frequency");
        else         note ("T12b ok: grains read 220/440/880 Hz");
    }

    // ================================================================= T12c
    // Spray determinism: T12b config + spray 200 ms -> two runs bit-identical.
    void t12c (double Fs)
    {
        Slot s = makeBaselineSlot();
        s.speed = 1.0; s.bounce = 1.0; s.lifeTimeSeconds = 16.0;
        s.stepSeconds = 0.12; s.grainMs = 10.0; s.sprayMs = 200.0;

        std::vector<double> inL (size_t (1.0 * Fs), 0.0), inR = inL;
        addBurst (inL, inR, Fs, 0.00, 220.0, 0.5, 12.0);
        addBurst (inL, inR, Fs, 0.12, 440.0, 0.5, 12.0);
        addBurst (inL, inR, Fs, 0.24, 880.0, 0.5, 12.0);

        auto run = [&] (std::vector<double>& oL, std::vector<double>& oR)
        {
            TumbleDelay e;
            e.prepare (Fs); applyGlobalBaseline (e);
            e.setRetrigMs (2000.0);
            e.setSlotParams (0, s); e.reset();
            renderInto (e, inL, inR, oL, oR);
        };
        std::vector<double> a, b, c, d;
        run (a, b); run (c, d);
        if (! bitEqual (a, c) || ! bitEqual (b, d))
            fail ("T12c: spray render not bit-reproducible");
        else
            note ("T12c ok: spray bit-identical across runs");
    }

    void coreTests (double Fs)
    {
        std::printf ("tumble-delay core @ Fs=%.0f\n", Fs);
        t1a (Fs);  t1b (Fs); t2 (Fs);  t3 (Fs);
        t4 (Fs);   t5 (Fs);  t6 (Fs);  t7 (Fs);
        t8a (Fs);  t8b (Fs); t9 (Fs);  t10 (Fs);
        t11 (Fs);  t12a (Fs); t12b (Fs); t12c (Fs);
    }
}

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
