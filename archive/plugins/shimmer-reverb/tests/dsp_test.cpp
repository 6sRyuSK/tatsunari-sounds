//
// dsp_test.cpp — headless verification of the shimmer reverb DSP core
// (factory_core::OnePole / PitchShifter / ShimmerReverb). The reverb *sound* is
// a sonic judgement; what is tested is the deterministic machinery:
//
//   1. OnePole: unity at DC for the lowpass, zero at DC for the highpass.
//   2. PitchShifter: a tone at f comes out dominated by ratio*f (+12 -> 2f,
//      +7 -> 2^(7/12) f, -12 -> f/2), measured by DFT.
//   3. Reverb decay: the tail decays, and a longer Decay setting sustains more.
//   4. Damping darkens the tail (less high-frequency energy).
//   5. Freeze sustains the tail (orthonormal feedback, no damping).
//   6. Mix=0 is exact dry passthrough.
//   7. Stability: no NaN/Inf at extreme settings; feedback loop gain < 1
//      (impulse-response energy non-increasing, class A).
//   8. Mod headroom is a fixed *time* across all rates and the delay buffers
//      carry the matching rate-derived headroom (class G/D).
//   9. Pre-delay places the wet onset at the requested time (class D buffer).
//  10. Freeze toggle mid-tone is click-free (class E, no discontinuity).
//  11. Low/high cut tone-shape the shimmer tail in the expected direction.
//
#include "factory_core/OnePole.h"
#include "factory_core/PitchShifter.h"
#include "factory_core/ShimmerReverb.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fct = factory_core::testing;

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    double rms (const std::vector<double>& v, int start, int len)
    {
        double s = 0.0; int c = 0;
        for (int i = start; i < start + len && i < (int) v.size(); ++i) { s += v[(size_t) i] * v[(size_t) i]; ++c; }
        return c > 0 ? std::sqrt (s / c) : 0.0;
    }

    double dftMag (const std::vector<double>& x, double w, int start, int len)
    {
        double re = 0.0, im = 0.0;
        for (int i = start; i < start + len && i < (int) x.size(); ++i)
        {
            re += x[(size_t) i] * std::cos (w * i);
            im -= x[(size_t) i] * std::sin (w * i);
        }
        return std::sqrt (re * re + im * im) / len;
    }

    // Energy in a +/-8% band around a centre frequency. The crossfade pitch
    // shifter amplitude-modulates the output, spreading energy into sidebands,
    // so a single bin is not enough.
    double bandEnergy (const std::vector<double>& x, double centreHz, double Fs, int start, int len)
    {
        double e = 0.0;
        for (int k = -5; k <= 5; ++k)
        {
            const double f = centreHz * (1.0 + 0.016 * k);
            const double m = dftMag (x, 2.0 * kPi * f / Fs, start, len);
            e += m * m;
        }
        return e;
    }

    void onePoleTest()
    {
        std::printf ("OnePole\n");
        factory_core::OnePole lp; lp.setCutoff (1000.0, 48000.0);
        double y = 0.0;
        for (int i = 0; i < 20000; ++i) y = lp.lp (1.0);
        if (std::abs (y - 1.0) > 1e-3) fail ("LP DC gain != 1");

        factory_core::OnePole hp; hp.setCutoff (1000.0, 48000.0);
        double h = 0.0;
        for (int i = 0; i < 20000; ++i) h = hp.hp (1.0);
        if (std::abs (h) > 1e-3) fail ("HP DC gain != 0");
        std::printf ("  ok\n");
    }

    void pitchTest (double Fs)
    {
        std::printf ("PitchShifter @ Fs=%.0f\n", Fs);
        const double f = 440.0;
        struct Case { double semis; double targetMul; const char* name; };
        // +7 is a shimmer-path interval used by Pitch A/B (Voice B default);
        // 2^(7/12) ~= 1.4983. Kept alongside the octave cases so every shipped
        // shimmer interval is accuracy-checked, not just +/-12.
        for (auto c : { Case { 12.0, 2.0, "+12" },
                        Case { 7.0, std::pow (2.0, 7.0 / 12.0), "+7" },
                        Case { -12.0, 0.5, "-12" } })
        {
            factory_core::PitchShifter ps;
            ps.prepare (Fs);
            ps.setRatio (factory_core::PitchShifter::semitonesToRatio (c.semis));

            const int settle = (int) (0.15 * Fs);
            const int M = (int) (0.2 * Fs);
            std::vector<double> out ((size_t) (settle + M));
            for (int n = 0; n < settle + M; ++n)
                out[(size_t) n] = ps.process (std::sin (2.0 * kPi * f * n / Fs));

            const double eF      = bandEnergy (out, f, Fs, settle, M);
            const double eTarget = bandEnergy (out, f * c.targetMul, Fs, settle, M);
            if (eTarget < 2.0 * eF)
                fail (std::string (c.name) + ": target band not dominant (target=" + std::to_string (eTarget)
                      + " f=" + std::to_string (eF) + ")");
            std::printf ("  %s: energy@target=%.4e  energy@f=%.4e\n", c.name, eTarget, eF);
        }
    }

    std::vector<double> reverbImpulse (double Fs, double decaySec, double damping, double seconds)
    {
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setSize (1.0);
        rv.setDecaySec (decaySec);
        rv.setDamping (damping);
        rv.setShimmer (0.0);
        rv.setModDepth (0.0);
        rv.setMix (1.0);

        const int N = (int) (seconds * Fs);
        std::vector<double> out ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            double l = (n == 0) ? 1.0 : 0.0, r = l;
            rv.processStereo (l, r);
            out[(size_t) n] = 0.5 * (l + r);
        }
        return out;
    }

    void decayTest (double Fs)
    {
        std::printf ("Reverb decay @ Fs=%.0f\n", Fs);
        auto ratioFor = [&] (double dec) {
            const auto out = reverbImpulse (Fs, dec, 0.3, 1.0);
            const double early = rms (out, (int) (0.1 * Fs), (int) (0.1 * Fs));
            const double late  = rms (out, (int) (0.6 * Fs), (int) (0.1 * Fs));
            return late / std::max (early, 1e-12);
        };
        const double r1 = ratioFor (1.0);
        const double r3 = ratioFor (3.0);
        if (r1 >= 1.0)  fail ("short decay not decaying (ratio " + std::to_string (r1) + ")");
        if (r3 <= r1)   fail ("longer decay did not sustain more (" + std::to_string (r3) + " <= " + std::to_string (r1) + ")");
        std::printf ("  decay ratio: 1s=%.3f  3s=%.3f\n", r1, r3);
    }

    double tailHfEnergy (double Fs, double damping)
    {
        const auto out = reverbImpulse (Fs, 2.5, damping, 0.8);
        factory_core::OnePole hp; hp.setCutoff (4000.0, Fs);
        double s = 0.0; int c = 0;
        for (int i = (int) (0.1 * Fs); i < (int) out.size(); ++i) { const double h = hp.hp (out[(size_t) i]); s += h * h; ++c; }
        return std::sqrt (s / std::max (1, c));
    }

    void dampingTest (double Fs)
    {
        std::printf ("Damping @ Fs=%.0f\n", Fs);
        const double lowDamp = tailHfEnergy (Fs, 0.1);
        const double hiDamp  = tailHfEnergy (Fs, 0.9);
        if (hiDamp >= lowDamp) fail ("more damping did not reduce HF (" + std::to_string (hiDamp) + " >= " + std::to_string (lowDamp) + ")");
        std::printf ("  HF tail energy: damp0.1=%.4e  damp0.9=%.4e\n", lowDamp, hiDamp);
    }

    // A sane linear peak ceiling for the reverb output. The input bursts here
    // peak at 0.4, and a stable FDN — even with pitch-shifted shimmer feedback
    // and modulation — must not build far past the excitation level. 10.0 is
    // ~28 dB above the 0.4 burst: generous headroom for transient buildup, yet
    // it still catches the real divergence (freeze+shimmer used to run away to
    // 1e6+). The old 1e6 (~120 dB) bound was effectively "not NaN".
    constexpr double kPeakCeiling = 10.0;

    void freezeTest (double Fs)
    {
        std::printf ("Freeze @ Fs=%.0f\n", Fs);
        auto sustainRatio = [&] (bool freeze) {
            factory_core::ShimmerReverb rv;
            rv.prepare (Fs);
            rv.setSize (1.0); rv.setDecaySec (1.5); rv.setDamping (0.2);
            rv.setShimmer (0.0); rv.setModDepth (0.0); rv.setMix (1.0);

            std::vector<double> out;
            // Build the tail with a short tone burst.
            const int burst = (int) (0.1 * Fs);
            for (int n = 0; n < burst; ++n) { double l = 0.4 * std::sin (2.0 * kPi * 300.0 * n / Fs), r = l; rv.processStereo (l, r); }
            rv.setFreeze (freeze);
            const int hold = (int) (1.0 * Fs);
            out.resize ((size_t) hold);
            for (int n = 0; n < hold; ++n) { double l = 0.0, r = 0.0; rv.processStereo (l, r); out[(size_t) n] = 0.5 * (l + r); }

            const double early = rms (out, (int) (0.1 * Fs), (int) (0.1 * Fs));
            const double late  = rms (out, (int) (0.8 * Fs), (int) (0.1 * Fs));
            return late / std::max (early, 1e-12);
        };
        const double frozen = sustainRatio (true);
        const double normal = sustainRatio (false);
        if (frozen < 0.5)        fail ("freeze did not sustain (ratio " + std::to_string (frozen) + ")");
        if (frozen <= normal)    fail ("freeze not more sustained than normal");
        std::printf ("  sustain ratio: freeze=%.3f  normal=%.3f\n", frozen, normal);
    }

    // Guards the P0 that used to make Freeze + Shimmer diverge: with shimmer > 0
    // and Freeze ON, holding for tens of seconds must stay finite AND bounded to
    // a sane ceiling (issue #35).
    void freezeShimmerTest (double Fs)
    {
        std::printf ("Freeze+Shimmer @ Fs=%.0f\n", Fs);
        auto run = [&] (double shimmer) {
            factory_core::ShimmerReverb rv;
            rv.prepare (Fs);
            rv.setSize (1.2); rv.setDecaySec (4.0); rv.setDamping (0.1);
            rv.setShimmer (shimmer); rv.setPitchASemis (12.0); rv.setPitchBSemis (7.0);
            rv.setVoiceBMix (0.5); rv.setModDepth (0.5); rv.setMix (1.0);

            // Feed a burst to excite the tail, then freeze and hold ~45s silent.
            const int burst = (int) (0.2 * Fs);
            for (int n = 0; n < burst; ++n)
            {
                double l = 0.4 * std::sin (2.0 * kPi * 300.0 * n / Fs), r = l;
                rv.processStereo (l, r);
            }
            rv.setFreeze (true);

            const int hold = (int) (45.0 * Fs);
            std::vector<double> out ((size_t) hold);
            for (int n = 0; n < hold; ++n)
            {
                double l = 0.0, r = 0.0;
                rv.processStereo (l, r);
                out[(size_t) n] = std::max (std::abs (l), std::abs (r));
            }
            if (! fct::allFinite (out))
            { fail ("freeze+shimmer non-finite at shimmer=" + std::to_string (shimmer)); return; }
            const double peak = fct::peakAbs (out);
            if (peak > kPeakCeiling)
                fail ("freeze+shimmer grew past ceiling (shimmer=" + std::to_string (shimmer)
                      + " peak=" + std::to_string (peak) + ")");
            std::printf ("  shimmer=%.2f: finite over 45s, peak=%.3f\n", shimmer, peak);
        };
        run (0.35);
        run (0.95);
    }

    // High shimmer + long decay transient: peak must stay within the sane bound
    // (issue #35).
    void highShimmerTransientTest (double Fs)
    {
        std::printf ("High-shimmer transient @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setSize (1.4); rv.setDecaySec (12.0); rv.setDamping (0.0);
        rv.setShimmer (0.95); rv.setPitchASemis (12.0); rv.setPitchBSemis (19.0);
        rv.setVoiceBMix (0.5); rv.setModDepth (1.0); rv.setMix (1.0);

        const int burst = (int) (0.1 * Fs);
        const int N = (int) (20.0 * Fs);
        std::vector<double> out ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            const double x = (n < burst) ? 0.4 * std::sin (2.0 * kPi * 220.0 * n / Fs) : 0.0;
            double l = x, r = x;
            rv.processStereo (l, r);
            out[(size_t) n] = std::max (std::abs (l), std::abs (r));
        }
        if (! fct::allFinite (out)) { fail ("high-shimmer transient non-finite"); return; }
        const double peak = fct::peakAbs (out);
        if (peak > kPeakCeiling)
            fail ("high-shimmer transient exceeded ceiling (peak " + std::to_string (peak) + ")");
        std::printf ("  finite, peak=%.3f\n", peak);
    }

    void mixTest (double Fs)
    {
        std::printf ("Mix=0 dry passthrough @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setMix (0.0);
        rv.setShimmer (0.5);
        double maxErr = 0.0;
        for (int n = 0; n < (int) (0.2 * Fs); ++n)
        {
            const double x = 0.4 * std::sin (2.0 * kPi * 330.0 * n / Fs);
            double l = x, r = x;
            rv.processStereo (l, r);
            maxErr = std::max (maxErr, std::abs (l - x));
        }
        if (maxErr > 1e-12) fail ("mix=0 not exact dry: " + std::to_string (maxErr));
        std::printf ("  max err = %.2e\n", maxErr);
    }

    void stabilityTest (double Fs)
    {
        std::printf ("Stability @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setSize (1.4); rv.setDecaySec (10.0); rv.setDamping (0.0);
        rv.setShimmer (0.9); rv.setPitchASemis (12.0); rv.setPitchBSemis (7.0);
        rv.setVoiceBMix (0.5); rv.setModDepth (1.0); rv.setMix (1.0);

        const int N = (int) (2.0 * Fs);
        std::vector<double> out ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            const double x = 0.3 * std::sin (2.0 * kPi * 220.0 * n / Fs);
            double l = x, r = x;
            rv.processStereo (l, r);
            out[(size_t) n] = std::abs (l);
        }
        if (! fct::allFinite (out)) { fail ("non-finite output"); return; }
        const double peak = fct::peakAbs (out);
        if (peak > kPeakCeiling) fail ("output grew unbounded (peak " + std::to_string (peak) + ")");
        std::printf ("  finite, peak=%.2f\n", peak);
    }

    // Class A (regression-policy): the FDN feedback loop gain must be < 1 at the
    // worst-case (max size, max decay, no damping) so the impulse-response
    // energy is non-increasing window-over-window and the tail cannot run away.
    // Shimmer is off here so this isolates the FDN loop (the shimmer + freeze
    // divergence path is gated separately by freezeShimmerTest). Uses the shared
    // oracle-free invariant across the full rate matrix.
    void feedbackNonIncreasingTest (double Fs)
    {
        std::printf ("Feedback loop gain < 1 @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setSize (1.6); rv.setDecaySec (15.0); rv.setDamping (0.0);
        rv.setShimmer (0.0); rv.setModDepth (0.0); rv.setMix (1.0);
        rv.setFreeze (false);
        const bool ok = fct::impulseResponseNonIncreasing (
            [&] (double x) { double l = x, r = x; rv.processStereo (l, r); return 0.5 * (l + r); },
            Fs);
        if (! ok) fail ("impulse-response energy increased (loop gain >= 1)");
        else      std::printf ("  non-increasing tail (loop gain < 1)\n");
    }

    // Class G/D: the LFO tail modulation is a fixed *time* depth, so its sample
    // count must scale with the rate (the old fixed 24-sample constant gave 1/4
    // the time-depth at 192 kHz). Assert (a) the depth-in-time is rate-invariant
    // and equals the 44.1 kHz reference, and (b) the delay buffers carry the
    // matching headroom: at max size + full mod depth the tail stays finite,
    // bounded, and non-silent (no silent buffer clamp / wrap).
    void modHeadroomTest (double Fs)
    {
        std::printf ("Mod headroom (rate-consistent) @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);

        const double got     = rv.maxModDepthSamples();
        const double expSamp = 24.0 * Fs / 44100.0;                 // fixed time in samples
        const double refMs   = 24.0 / 44100.0 * 1000.0;             // ~0.544 ms reference
        const double gotMs   = got / Fs * 1000.0;
        if (std::abs (got - expSamp) > 1e-6 * std::max (1.0, expSamp))
            fail ("mod depth sample count not rate-derived (got " + std::to_string (got)
                  + " expected " + std::to_string (expSamp) + ")");
        if (std::abs (gotMs - refMs) > 1e-6)
            fail ("mod depth time not rate-invariant (" + std::to_string (gotMs) + "ms vs "
                  + std::to_string (refMs) + "ms)");

        // Worst-case buffer exercise: max size, full mod depth, non-zero mod rate.
        rv.setSize (1.6); rv.setDecaySec (4.0); rv.setDamping (0.1);
        rv.setShimmer (0.5); rv.setPitchASemis (12.0); rv.setVoiceBMix (0.0);
        rv.setModRateHz (5.0); rv.setModDepth (1.0); rv.setMix (1.0);

        const int N = (int) (3.0 * Fs);
        std::vector<double> out ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            const double x = (n == 0) ? 1.0 : 0.0;
            double l = x, r = x;
            rv.processStereo (l, r);
            out[(size_t) n] = 0.5 * (l + r);
        }
        if (! fct::allFinite (out)) { fail ("mod-depth=100% non-finite"); return; }
        if (fct::peakAbs (out) > kPeakCeiling) fail ("mod-depth=100% exceeded ceiling");
        const double tail = rms (out, (int) (2.0 * Fs), (int) (0.25 * Fs));
        if (tail <= 1e-9)
            fail ("mod-depth=100% tail silent (buffer clamp/wrap?)");
        std::printf ("  modSamples=%.3f (%.4f ms), tail rms=%.3e\n", got, gotMs, tail);
    }

    // Class D: pre-delay must place the wet onset later by exactly the requested
    // time. The FDN's own first-reflection latency (shortest comb) is identical
    // regardless of pre-delay, so onset(preDelay) - onset(0) == preDelaySamples
    // is a clean, implementation-independent oracle. Tested at max (250 ms) and a
    // mid value (100 ms), across the full rate matrix.
    void preDelayTest (double Fs)
    {
        std::printf ("Pre-delay onset @ Fs=%.0f\n", Fs);
        auto onsetFor = [&] (double preMs) {
            factory_core::ShimmerReverb rv;
            rv.prepare (Fs);
            rv.setSize (1.0); rv.setDecaySec (2.5); rv.setDamping (0.3);
            rv.setShimmer (0.0); rv.setModDepth (0.0); rv.setMix (1.0);
            rv.setPreDelayMs (preMs);
            const int N = (int) (0.6 * Fs + preMs * 1.0e-3 * Fs);
            std::vector<double> out ((size_t) N);
            for (int n = 0; n < N; ++n)
            {
                double l = (n == 0) ? 1.0 : 0.0, r = l;
                rv.processStereo (l, r);
                out[(size_t) n] = 0.5 * (l + r);
            }
            const double peak = fct::peakAbs (out);
            const double thr  = 0.01 * peak;
            for (int n = 0; n < N; ++n)
                if (std::abs (out[(size_t) n]) > thr) return n;
            return N;
        };
        const int on0 = onsetFor (0.0);
        const double window = std::max (4.0, 0.002 * Fs); // 2 ms slack for threshold crossing
        for (double preMs : { 100.0, 250.0 })
        {
            const int    on   = onsetFor (preMs);
            const double pdS  = preMs * 1.0e-3 * Fs;
            const double diff = (double) (on - on0);
            if (on <= (int) pdS)
                fail ("wet appeared before pre-delay (" + std::to_string (preMs) + "ms)");
            if (std::abs (diff - pdS) > window)
                fail ("pre-delay onset off (" + std::to_string (preMs) + "ms: diff="
                      + std::to_string (diff) + " expected " + std::to_string (pdS) + ")");
            std::printf ("  preDelay=%.0fms: onset shift=%.0f samples (expected %.0f)\n", preMs, diff, pdS);
        }
    }

    // Class E: toggling Freeze on then off mid-stream, under a sustained tone,
    // must not click. Measured directly: the largest sample-to-sample step near
    // each toggle stays a small fraction of the running signal peak. An abrupt
    // gain step (a real click) has a step of order the signal peak, so a 0.15x
    // ceiling fails on it with margin while the (continuous) toggle passes.
    void freezeEdgeTest (double Fs)
    {
        std::printf ("Freeze toggle click-free @ Fs=%.0f\n", Fs);
        factory_core::ShimmerReverb rv;
        rv.prepare (Fs);
        rv.setSize (1.0); rv.setDecaySec (2.5); rv.setDamping (0.2);
        rv.setShimmer (0.0); rv.setModDepth (0.0); rv.setMix (1.0);

        const int N   = (int) (3.0 * Fs);
        const int onN  = (int) (1.0 * Fs);
        const int offN = (int) (2.0 * Fs);
        std::vector<double> out ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            if (n == onN)  rv.setFreeze (true);
            if (n == offN) rv.setFreeze (false);
            const double x = 0.3 * std::sin (2.0 * kPi * 300.0 * n / Fs);
            double l = x, r = x;
            rv.processStereo (l, r);
            out[(size_t) n] = 0.5 * (l + r);
        }
        if (! fct::allFinite (out)) { fail ("freeze-edge non-finite"); return; }
        const double peak = fct::peakAbs (out);
        auto maxDeltaNear = [&] (int c) {
            double m = 0.0;
            for (int n = std::max (1, c - 64); n < c + 256 && n < N; ++n)
                m = std::max (m, std::abs (out[(size_t) n] - out[(size_t) n - 1]));
            return m;
        };
        const double dOn  = maxDeltaNear (onN);
        const double dOff = maxDeltaNear (offN);
        const double bound = 0.15 * peak;
        if (dOn > bound)  fail ("freeze-ON click (delta " + std::to_string (dOn) + " > " + std::to_string (bound) + ")");
        if (dOff > bound) fail ("freeze-OFF click (delta " + std::to_string (dOff) + " > " + std::to_string (bound) + ")");
        std::printf ("  peak=%.3f  dOn=%.4f  dOff=%.4f  bound=%.4f\n", peak, dOn, dOff, bound);
    }

    // Class H (real bug path) for the tone-shaping cuts, which act only on the
    // shimmer feedback voice. With shimmer high and a long recirculating tail,
    // engaging the high cut low must drop the tail HF/LF ratio, and engaging the
    // low cut high must drop the tail LF energy — direction + a meaningful
    // magnitude, measured with independent OnePole split filters (band edges are
    // fixed audio frequencies, well below Nyquist at every rate).
    void cutShapingTest (double Fs)
    {
        std::printf ("Low/High-cut tone shaping @ Fs=%.0f\n", Fs);
        auto tailBands = [&] (double hicut, double locut, double lpHz, double hpHz,
                              double& hf, double& lf) {
            factory_core::ShimmerReverb rv;
            rv.prepare (Fs);
            rv.setSize (1.2); rv.setDecaySec (6.0); rv.setDamping (0.05);
            rv.setShimmer (0.85); rv.setPitchASemis (12.0); rv.setVoiceBMix (0.0);
            rv.setHighCutHz (hicut); rv.setLowCutHz (locut);
            rv.setModDepth (0.0); rv.setMix (1.0);
            const int burst = (int) (0.2 * Fs);
            const int N = (int) (4.0 * Fs);
            std::vector<double> out ((size_t) N);
            for (int n = 0; n < N; ++n)
            {
                const double x = (n < burst) ? 0.4 * std::sin (2.0 * kPi * 300.0 * n / Fs) : 0.0;
                double l = x, r = x;
                rv.processStereo (l, r);
                out[(size_t) n] = 0.5 * (l + r);
            }
            factory_core::OnePole hp, lp; hp.setCutoff (hpHz, Fs); lp.setCutoff (lpHz, Fs);
            const int s = (int) (2.5 * Fs);
            hf = 0.0; lf = 0.0;
            for (int n = 0; n < N; ++n)
            {
                const double h = hp.hp (out[(size_t) n]);
                const double o = lp.lp (out[(size_t) n]);
                if (n >= s) { hf += h * h; lf += o * o; }
            }
        };

        // High cut: HF/LF ratio must fall when the cut is brought down low.
        double hf, lf;
        tailBands (16000.0, 20.0, 600.0, 3000.0, hf, lf);   const double rOpen = hf / std::max (lf, 1e-30);
        tailBands ( 1200.0, 20.0, 600.0, 3000.0, hf, lf);   const double rLow  = hf / std::max (lf, 1e-30);
        if (! (rLow < 0.5 * rOpen))
            fail ("high-cut did not reduce HF/LF ratio (rLow " + std::to_string (rLow)
                  + " vs rOpen " + std::to_string (rOpen) + ")");

        // Low cut: LF energy must fall when the cut is brought up high.
        double lfOpen, lfHigh, d;
        tailBands (16000.0,   20.0, 400.0, 3000.0, d, lfOpen);
        tailBands (16000.0, 1000.0, 400.0, 3000.0, d, lfHigh);
        if (! (lfHigh < 0.7 * lfOpen))
            fail ("low-cut did not reduce LF energy (lfHigh " + std::to_string (lfHigh)
                  + " vs lfOpen " + std::to_string (lfOpen) + ")");
        std::printf ("  highcut HF/LF: open=%.4f low=%.4f | lowcut LF: open=%.3e high=%.3e\n",
                     rOpen, rLow, lfOpen, lfHigh);
    }
}

int main (int argc, char** argv)
{
    onePoleTest();

    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
    {
        pitchTest (Fs);
        decayTest (Fs);
        dampingTest (Fs);
        cutShapingTest (Fs);
        preDelayTest (Fs);
        modHeadroomTest (Fs);
        freezeTest (Fs);
        freezeEdgeTest (Fs);
        freezeShimmerTest (Fs);
        highShimmerTransientTest (Fs);
        feedbackNonIncreasingTest (Fs);
        mixTest (Fs);
        stabilityTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
