//
// dsp_test.cpp — headless verification of the single-band peaking EQ DSP core.
//
// Two gates, run at Fs = 44100 / 48000 / 96000:
//   1. Oracle vs measurement. An INDEPENDENT reference (re-implemented RBJ
//      coefficients + closed-form discrete transfer function H(e^jw)) is
//      compared against the response measured by running a unit impulse through
//      the actual DSP core (factory_core::designPeaking + Biquad), then taking
//      the DFT of the captured impulse response.
//   2. Formula-independent invariants: |H(f0)| == gain dB, |H(DC)| == 0 dB,
//      |H(Nyquist)| == 0 dB. These follow from first principles, not from the
//      RBJ formula, so they catch bugs in the coefficient formulas themselves.
//
// The oracle is a SEPARATE code path from the implementation on purpose — it
// never calls into factory_core. Expected magnitudes are evaluated in the
// z-domain (H(e^jw)), never from the analog prototype.
//
#include "factory_core/Biquad.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;

    // --- INDEPENDENT oracle (does NOT touch factory_core) --------------------

    // Un-normalized RBJ peaking coefficients (a0 retained for the z-domain eval).
    struct OracleCoeffs
    {
        double b0, b1, b2, a0, a1, a2;
    };

    OracleCoeffs oracleRBJ (double f0, double gainDb, double Q, double Fs)
    {
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * kPi * f0 / Fs;
        const double cw    = std::cos (w0);
        const double sw    = std::sin (w0);
        const double alpha = sw / (2.0 * Q);

        return { 1.0 + alpha * A,   // b0
                 -2.0 * cw,         // b1
                 1.0 - alpha * A,   // b2
                 1.0 + alpha / A,   // a0
                 -2.0 * cw,         // a1
                 1.0 - alpha / A }; // a2
    }

    // Discrete transfer function evaluated at z = e^{jw}.
    cd oracleH (const OracleCoeffs& c, double w)
    {
        const cd z1 = std::exp (cd (0.0, -w));
        const cd z2 = std::exp (cd (0.0, -2.0 * w));
        const cd num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const cd den = c.a0 + c.a1 * z1 + c.a2 * z2;
        return num / den;
    }

    // --- Measurement through the actual DSP core -----------------------------

    // Capture N samples of the impulse response of the implementation.
    std::vector<double> measureImpulseResponse (double f0, double gainDb, double Q, double Fs, int N)
    {
        factory_core::Biquad bq;
        bq.setCoeffs (factory_core::designPeaking (f0, gainDb, Q, Fs));
        bq.reset();

        std::vector<double> h (static_cast<size_t> (N));
        h[0] = bq.processSample (1.0);
        for (int n = 1; n < N; ++n)
            h[n] = bq.processSample (0.0);
        return h;
    }

    // H(e^{jw}) from the measured impulse response, via direct DFT at angular
    // frequency w using a phase recurrence (no std::exp in the inner loop).
    cd dftAt (const std::vector<double>& h, double w)
    {
        const cd step = std::exp (cd (0.0, -w)); // e^{-jw}
        cd zk (1.0, 0.0);                          // e^{-jw*n}, n = 0
        cd acc (0.0, 0.0);
        for (const double hn : h)
        {
            acc += hn * zk;
            zk *= step;
        }
        return acc;
    }

    double magDb (const cd& h) { return 20.0 * std::log10 (std::abs (h)); }

    struct Combo { double f0, gainDb, Q; const char* name; };

    // Representative spread: boost/cut, low/mid/high freq, low..high Q. Kept
    // clear of the simultaneous (very-low-freq + very-high-Q + max-boost) corner
    // whose impulse response would not decay within N samples — that is a
    // measurement limit, not a tolerance we are allowed to widen.
    const Combo kCombos[] = {
        { 1000.0,  6.0, 0.707, "1k +6 Q0.707" },
        { 1000.0, -6.0, 2.0,   "1k -6 Q2"     },
        {  200.0, 12.0, 1.0,   "200 +12 Q1"   },
        { 5000.0,-12.0, 4.0,   "5k -12 Q4"    },
        { 8000.0,  9.0, 8.0,   "8k +9 Q8"     },
        { 1000.0, 12.0, 12.0,  "1k +12 Q12"   },
    };

    // N is sized so the impulse response of every combo above has decayed far
    // below the tolerances by the time it is truncated (verified: tail < ~1e-7).
    constexpr int kIRLength = 1 << 16;

    // Gate tolerances.
    constexpr double kMagTolDb       = 0.05;  // primary gate, oracle vs measured
    constexpr double kComplexRelTol  = 1e-3;  // phase, via the complex response
    constexpr double kInvariantExact = 1e-9;  // invariants on the closed form
    constexpr double kInvariantMeasTolDb = 0.01; // invariants on measured IR (bounded by IR truncation)

    int g_failures = 0;

    void fail (const std::string& msg)
    {
        std::printf ("  FAIL: %s\n", msg.c_str());
        ++g_failures;
    }

    bool runAtSampleRate (double Fs)
    {
        std::printf ("Fs = %.0f Hz\n", Fs);
        const int before = g_failures;

        for (const auto& c : kCombos)
        {
            const OracleCoeffs oc = oracleRBJ (c.f0, c.gainDb, c.Q, Fs);
            const std::vector<double> ir = measureImpulseResponse (c.f0, c.gainDb, c.Q, Fs, kIRLength);

            // --- Test 1: oracle vs measurement over a log-spaced grid ---
            const double fLo = 20.0;
            const double fHi = 0.49 * Fs;       // just shy of Nyquist
            const int    M   = 64;
            double maxMagErr = 0.0, maxCplxErr = 0.0;

            for (int k = 0; k < M; ++k)
            {
                const double f = fLo * std::pow (fHi / fLo, static_cast<double> (k) / (M - 1));
                const double w = 2.0 * kPi * f / Fs;

                const cd Ho = oracleH (oc, w);
                const cd Hm = dftAt (ir, w);

                const double magErr  = std::abs (magDb (Hm) - magDb (Ho));
                const double cplxErr = std::abs (Hm - Ho) / std::max (std::abs (Ho), 1e-12);
                maxMagErr  = std::max (maxMagErr,  magErr);
                maxCplxErr = std::max (maxCplxErr, cplxErr);

                if (magErr > kMagTolDb)
                    fail (std::string (c.name) + ": |dH|=" + std::to_string (magErr)
                          + " dB at f=" + std::to_string (f) + " Hz");
                if (cplxErr > kComplexRelTol)
                    fail (std::string (c.name) + ": complex rel err=" + std::to_string (cplxErr)
                          + " at f=" + std::to_string (f) + " Hz");
            }

            // --- Test 2: formula-independent invariants ---
            const double w0   = 2.0 * kPi * c.f0 / Fs;
            const double wNyq = kPi;

            // Exact, on the closed-form oracle.
            const double dcDb   = magDb (oracleH (oc, 0.0));
            const double f0Db   = magDb (oracleH (oc, w0));
            const double nyqDb  = magDb (oracleH (oc, wNyq));
            if (std::abs (dcDb - 0.0)      > kInvariantExact) fail (std::string (c.name) + ": oracle DC != 0 dB");
            if (std::abs (f0Db - c.gainDb) > kInvariantExact) fail (std::string (c.name) + ": oracle |H(f0)| != gain");
            if (std::abs (nyqDb - 0.0)     > kInvariantExact) fail (std::string (c.name) + ": oracle Nyquist != 0 dB");

            // On the measured implementation (bounded by IR truncation).
            const double dcMeas  = magDb (dftAt (ir, 0.0));
            const double f0Meas  = magDb (dftAt (ir, w0));
            const double nyqMeas = magDb (dftAt (ir, wNyq));
            if (std::abs (dcMeas - 0.0)      > kInvariantMeasTolDb) fail (std::string (c.name) + ": measured DC != 0 dB");
            if (std::abs (f0Meas - c.gainDb) > kInvariantMeasTolDb) fail (std::string (c.name) + ": measured |H(f0)| != gain");
            if (std::abs (nyqMeas - 0.0)     > kInvariantMeasTolDb) fail (std::string (c.name) + ": measured Nyquist != 0 dB");

            std::printf ("  %-14s maxMagErr=%.2e dB  maxCplxErr=%.2e  (f0=%.2f DC=%.2e Nyq=%.2e dB)\n",
                         c.name, maxMagErr, maxCplxErr, f0Meas, dcMeas, nyqMeas);
        }

        return g_failures == before;
    }
}

int main (int argc, char** argv)
{
    std::vector<double> rates;
    if (argc > 1)
        rates.push_back (std::atof (argv[1]));
    else
        rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    for (double Fs : rates)
        runAtSampleRate (Fs);

    if (g_failures == 0)
    {
        std::printf ("OK: all checks passed.\n");
        return 0;
    }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
