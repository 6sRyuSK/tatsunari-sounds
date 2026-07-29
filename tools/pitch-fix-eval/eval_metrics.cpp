//
// tools/pitch-fix-eval/eval_metrics.cpp — non-gated pitch-fix metrics harness.
// Links only factory_core + PfCore headers. NOT part of CTest.
//
// Reports per Buffer mode (synthetic oracles):
//   * Gross Pitch Error rate  (|det − f0_true| > 50 ct, voiced frames)
//   * Harmonic / octave error rate (near 2f0 or 3f0)
//   * Voicing Decision Error  (false unvoiced on steady voiced tone)
//   * Correction residual cents (Accuracy / Stability contract spot-checks)
//
#include "PfCore.h"
#include "PfCorrection.h"
#include "factory_core/PsolaShifter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr double kPi = 3.14159265358979323846;

static double centsBetween (double a, double b)
{
    return 1200.0 * std::log2 (a / b);
}

static std::vector<float> makeVibratoVoice (int n, double Fs, double f0,
                                           double vibCt, double vibHz, float amp)
{
    std::vector<float> v ((size_t) n);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / Fs;
        const double f = f0 * std::pow (2.0, (vibCt / 1200.0) * std::sin (2.0 * kPi * vibHz * t));
        ph += 2.0 * kPi * f / Fs;
        double s = 0.0;
        for (int h = 1; h <= 8; ++h)
            s += std::pow ((double) h, -0.7) * std::sin ((double) h * ph);
        v[(size_t) i] = (float) (amp * 0.22 * s);
    }
    return v;
}

static std::vector<float> makeSine (int n, double Fs, double f, float amp)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = (float) (amp * std::sin (2.0 * kPi * f * (double) i / Fs));
    return v;
}

static void reportDetection (double Fs)
{
    std::printf ("=== detection metrics @ %.0f Hz ===\n", Fs);
    for (int mode = 0; mode < 4; ++mode)
    {
        for (double vibCt : { 0.0, 100.0, 150.0 })
        {
            pf_core::PfCore core;
            core.prepare (Fs, 512);
            pf_core::PfParamSnapshot s;
            s.amount = 0.0f;
            s.buffer = mode;
            s.minPitchHz = 75.0f;
            const double f0 = 220.0;
            auto x = makeVibratoVoice ((int) (2.5 * Fs), Fs, f0, vibCt, 5.5, 0.5f);
            std::vector<float> l (x), r (x);
            int voiced = 0, gpe = 0, harm = 0, miss = 0, frames = 0;
            for (int pos = 0; pos < (int) x.size(); pos += 512)
            {
                const int m = std::min (512, (int) x.size() - pos);
                core.process (l.data() + pos, r.data() + pos, m, s);
                if (pos < (int) (0.5 * Fs))
                    continue;
                ++frames;
                const double det = (double) core.uiDetectedHz.load();
                const double t = ((double) pos + 0.5 * (double) m) / Fs;
                const double fTrue = f0 * std::pow (2.0, (vibCt / 1200.0)
                                                    * std::sin (2.0 * kPi * 5.5 * t));
                if (det <= 0.0)
                {
                    ++miss;
                    continue;
                }
                ++voiced;
                if (std::abs (centsBetween (det, fTrue)) > 50.0)
                    ++gpe;
                if (std::abs (centsBetween (det, 2.0 * f0)) < 50.0
                    || std::abs (centsBetween (det, 3.0 * f0)) < 50.0)
                    ++harm;
            }
            const double gpePct = voiced ? 100.0 * gpe / voiced : 0.0;
            const double harmPct = voiced ? 100.0 * harm / voiced : 0.0;
            const double vdePct = frames ? 100.0 * miss / frames : 0.0;
            std::printf ("mode %d vib±%.0fct  GPE=%.1f%%  harm=%.1f%%  VDE(miss)=%.1f%%  voiced=%d/%d\n",
                         mode, vibCt, gpePct, harmPct, vdePct, voiced, frames);
        }
    }
}

static void reportCorrection (double Fs)
{
    std::printf ("=== correction residuals (pure math + core) @ %.0f Hz ===\n", Fs);
    struct Case { double err, stab, acc, expect; };
    const Case cases[] = {
        { 30, 12, 12, 18 },
        { 30, 12,  0, 30 },
        { 30, 12,  6, 24 },
        {  5, 12,  0,  0 },
    };
    for (const auto& c : cases)
    {
        const double d = pf_core::correctionDeadzone (c.err, c.stab, c.acc);
        std::printf ("deadzone(err=%.0f stab=%.0f acc=%.0f) = %.3f (expect %.0f) %s\n",
                     c.err, c.stab, c.acc, d, c.expect,
                     std::abs (d - c.expect) < 1.0e-6 ? "OK" : "MISMATCH");
    }

    // Core end-to-end: Stability=12 Accuracy=0 lands on target.
    pf_core::PfCore core;
    core.prepare (Fs, 512);
    pf_core::PfParamSnapshot s;
    s.retuneMs = 0.0f;
    s.glideMs = 0.0f;
    s.stabilityCt = 12.0f;
    s.accuracyCt = 0.0f;
    const double fIn = 440.0 * std::pow (2.0, 30.0 / 1200.0);
    auto x = makeSine ((int) (2.0 * Fs), Fs, fIn, 0.5f);
    std::vector<float> l (x), r (x);
    for (int pos = 0; pos < (int) x.size(); pos += 512)
    {
        const int m = std::min (512, (int) x.size() - pos);
        core.process (l.data() + pos, r.data() + pos, m, s);
    }
    std::printf ("core Accuracy=0 shift readout = %+.2f ct (want ~-30)\n",
                 (double) core.uiShiftCents.load());
}

// P5 scaffold: drive PsolaShifter with a fixed ratio curve and report energy /
// peak bounds. Epoch-sync PSOLA / phase-locked STFT engines are not in-tree yet;
// this path pins the shared ratio-curve input contract for when they arrive.
static void reportResynth (double Fs)
{
    std::printf ("=== P5 resynth scaffold (PsolaShifter @ fixed ratio) @ %.0f Hz ===\n", Fs);
    factory_core::PsolaShifter sh;
    const int maxLook = (int) std::ceil (5.0 * Fs / 25.0) + 8;
    const int maxPeriod = (int) std::ceil (Fs / 25.0) + 4;
    sh.prepare (Fs, 512, maxLook, maxPeriod);
    sh.setLookahead ((int) std::lround (3.5 * Fs / 75.0));
    sh.setTrack (Fs / 220.0, true);
    sh.setRatio (std::exp2 (50.0 / 1200.0)); // +50 ct fixed

    const int N = (int) (1.5 * Fs);
    auto x = makeSine (N, Fs, 220.0, 0.5f);
    std::vector<float> l (x), r (x);
    for (int pos = 0; pos < N; pos += 512)
    {
        const int m = std::min (512, N - pos);
        sh.process (l.data() + pos, r.data() + pos, m);
    }
    double peak = 0.0, energy = 0.0;
    for (int i = (int) (0.5 * Fs); i < N; ++i)
    {
        peak = std::max (peak, std::abs ((double) l[(size_t) i]));
        energy += (double) l[(size_t) i] * (double) l[(size_t) i];
    }
    std::printf ("PsolaShifter +50ct: peak=%.4f  energy=%.4f  latency=%d\n",
                 peak, energy, sh.latencySamples());
    std::printf ("(epoch-sync PSOLA / phase-locked STFT: not implemented — same ratio curve TBD)\n");
}

int main (int argc, char** argv)
{
    bool resynth = false;
    double Fs = 48000.0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--resynth") == 0)
            resynth = true;
        else
            Fs = std::atof (argv[i]);
    }
    if (! (Fs > 0.0))
        Fs = 48000.0;

    reportDetection (Fs);
    reportCorrection (Fs);
    if (resynth)
        reportResynth (Fs);
    return 0;
}
