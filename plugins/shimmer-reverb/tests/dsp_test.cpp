//
// dsp_test.cpp — headless verification of the shimmer reverb DSP core
// (factory_core::OnePole / PitchShifter / ShimmerReverb). The reverb *sound* is
// a sonic judgement; what is tested is the deterministic machinery:
//
//   1. OnePole: unity at DC for the lowpass, zero at DC for the highpass.
//   2. PitchShifter: a tone at f comes out dominated by ratio*f (+12 -> 2f,
//      -12 -> f/2), measured by DFT.
//   3. Reverb decay: the tail decays, and a longer Decay setting sustains more.
//   4. Damping darkens the tail (less high-frequency energy).
//   5. Freeze sustains the tail (orthonormal feedback, no damping).
//   6. Mix=0 is exact dry passthrough.
//   7. Stability: no NaN/Inf at extreme settings.
//
#include "factory_core/OnePole.h"
#include "factory_core/PitchShifter.h"
#include "factory_core/ShimmerReverb.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
        for (auto c : { Case { 12.0, 2.0, "+12" }, Case { -12.0, 0.5, "-12" } })
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
            double peak = 0.0;
            for (int n = 0; n < hold; ++n)
            {
                double l = 0.0, r = 0.0;
                rv.processStereo (l, r);
                if (! std::isfinite (l) || ! std::isfinite (r))
                { fail ("freeze+shimmer non-finite at shimmer=" + std::to_string (shimmer) + " n=" + std::to_string (n)); return; }
                peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
            }
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

        double peak = 0.0;
        const int burst = (int) (0.1 * Fs);
        const int N = (int) (20.0 * Fs);
        for (int n = 0; n < N; ++n)
        {
            const double x = (n < burst) ? 0.4 * std::sin (2.0 * kPi * 220.0 * n / Fs) : 0.0;
            double l = x, r = x;
            rv.processStereo (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r))
            { fail ("high-shimmer transient non-finite at n=" + std::to_string (n)); return; }
            peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
        }
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

        double peak = 0.0;
        for (int n = 0; n < (int) (2.0 * Fs); ++n)
        {
            const double x = 0.3 * std::sin (2.0 * kPi * 220.0 * n / Fs);
            double l = x, r = x;
            rv.processStereo (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) { fail ("non-finite output at n=" + std::to_string (n)); break; }
            peak = std::max (peak, std::abs (l));
        }
        if (peak > kPeakCeiling) fail ("output grew unbounded (peak " + std::to_string (peak) + ")");
        std::printf ("  finite, peak=%.2f\n", peak);
    }
}

int main (int argc, char** argv)
{
    onePoleTest();

    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    for (double Fs : rates)
    {
        pitchTest (Fs);
        decayTest (Fs);
        dampingTest (Fs);
        freezeTest (Fs);
        freezeShimmerTest (Fs);
        highShimmerTransientTest (Fs);
        mixTest (Fs);
        stabilityTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
