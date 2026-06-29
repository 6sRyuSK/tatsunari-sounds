//
// dsp_test.cpp — headless verification of the dynamic parametric EQ DSP core
// (factory_core::Filters + DynamicEqBand). Gates:
//
//   1. Per-type frequency response vs an INDEPENDENT z-domain oracle for each
//      band type (bell / low-shelf / high-shelf / high-pass / low-pass): impulse
//      through the real Biquad -> DFT, compared to the closed-form H(e^jw).
//   2. Formula-independent invariants per type (shelf gains at DC/Nyquist, pass
//      filters' stop/pass ends, bell gain at f0).
//   3. Cascaded multi-band response vs the product of the per-band oracles.
//   4. Dynamic gain-offset function (zero below threshold, full range above,
//      monotonic) and a dynamic band's steady-state gain moving by the range
//      between a quiet and a loud tone.
//
// The oracle re-implements the RBJ designs independently; it never calls the
// code under test.
//
#include "factory_core/DynamicEqBand.h"
#include "factory_core/Filters.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    using factory_core::BandType;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    struct OC { double b0, b1, b2, a0, a1, a2; };

    // INDEPENDENT RBJ designs (un-normalized, a0 kept).
    OC oracleDesign (BandType type, double f0, double gainDb, double Q, double Fs)
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * f0 / Fs;
        const double c  = std::cos (w0);
        const double s  = std::sin (w0);
        const double al = s / (2.0 * Q);
        const double ts = 2.0 * std::sqrt (A) * al;

        switch (type)
        {
            case BandType::Bell:
                return { 1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A };
            case BandType::LowShelf:
                return { A * ((A + 1) - (A - 1) * c + ts),
                         2.0 * A * ((A - 1) - (A + 1) * c),
                         A * ((A + 1) - (A - 1) * c - ts),
                         (A + 1) + (A - 1) * c + ts,
                         -2.0 * ((A - 1) + (A + 1) * c),
                         (A + 1) + (A - 1) * c - ts };
            case BandType::HighShelf:
                return { A * ((A + 1) + (A - 1) * c + ts),
                         -2.0 * A * ((A - 1) + (A + 1) * c),
                         A * ((A + 1) + (A - 1) * c - ts),
                         (A + 1) - (A - 1) * c + ts,
                         2.0 * ((A - 1) - (A + 1) * c),
                         (A + 1) - (A - 1) * c - ts };
            case BandType::HighPass:
                return { (1 + c) * 0.5, -(1 + c), (1 + c) * 0.5, 1 + al, -2.0 * c, 1 - al };
            case BandType::LowPass:
            default:
                return { (1 - c) * 0.5, 1 - c, (1 - c) * 0.5, 1 + al, -2.0 * c, 1 - al };
        }
    }

    cd oracleH (const OC& o, double w)
    {
        const cd z1 = std::exp (cd (0.0, -w));
        const cd z2 = std::exp (cd (0.0, -2.0 * w));
        return (o.b0 + o.b1 * z1 + o.b2 * z2) / (o.a0 + o.a1 * z1 + o.a2 * z2);
    }

    std::vector<double> impulseResponse (const factory_core::BiquadCoeffs& c, int N)
    {
        factory_core::Biquad bq;
        bq.setCoeffs (c);
        std::vector<double> h ((size_t) N);
        h[0] = bq.processSample (1.0);
        for (int n = 1; n < N; ++n) h[n] = bq.processSample (0.0);
        return h;
    }

    cd dftAt (const std::vector<double>& h, double w)
    {
        const cd step = std::exp (cd (0.0, -w));
        cd zk (1.0, 0.0), acc (0.0, 0.0);
        for (double hn : h) { acc += hn * zk; zk *= step; }
        return acc;
    }

    double magDb (const cd& h) { return 20.0 * std::log10 (std::abs (h)); }

    const char* typeName (BandType t)
    {
        switch (t)
        {
            case BandType::Bell: return "Bell";
            case BandType::LowShelf: return "LowShelf";
            case BandType::HighShelf: return "HighShelf";
            case BandType::HighPass: return "HighPass";
            default: return "LowPass";
        }
    }

    struct BandSpec { BandType type; double f0, gainDb, Q; };

    void perTypeTest (double Fs)
    {
        std::printf ("Per-type response @ Fs=%.0f\n", Fs);
        const int N = 1 << 16;
        const BandSpec specs[] = {
            { BandType::Bell,      1000.0,  6.0, 2.0 },
            { BandType::LowShelf,   200.0,  6.0, 0.707 },
            { BandType::HighShelf, 5000.0, -6.0, 0.707 },
            { BandType::HighPass,   100.0,  0.0, 0.707 },
            { BandType::LowPass,   8000.0,  0.0, 0.707 },
        };

        for (const auto& sp : specs)
        {
            const auto coeffs = factory_core::designFilter (sp.type, sp.f0, sp.gainDb, sp.Q, Fs);
            const OC oc = oracleDesign (sp.type, sp.f0, sp.gainDb, sp.Q, Fs);
            const auto ir = impulseResponse (coeffs, N);

            double maxAbs = 0.0;
            const int M = 64;
            for (int k = 0; k < M; ++k)
            {
                const double f = 20.0 * std::pow ((0.49 * Fs) / 20.0, (double) k / (M - 1));
                const double w = 2.0 * kPi * f / Fs;
                const cd Ho = oracleH (oc, w);
                const cd Hm = dftAt (ir, w);
                maxAbs = std::max (maxAbs, std::abs (Hm - Ho));
                if (std::abs (Ho) > 0.01 && std::abs (magDb (Hm) - magDb (Ho)) > 0.05)
                    fail (std::string (typeName (sp.type)) + ": dB mismatch at f=" + std::to_string (f));
            }
            if (maxAbs > 1.0e-4)
                fail (std::string (typeName (sp.type)) + ": complex abs err " + std::to_string (maxAbs));

            // Invariants on the oracle (exact).
            const double dc  = magDb (oracleH (oc, 0.0));
            const double nyq = magDb (oracleH (oc, kPi));
            const double atF0 = magDb (oracleH (oc, 2.0 * kPi * sp.f0 / Fs));
            auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-9; };

            switch (sp.type)
            {
                case BandType::Bell:
                    if (! near (dc, 0.0) || ! near (nyq, 0.0) || ! near (atF0, sp.gainDb)) fail ("Bell invariants");
                    break;
                case BandType::LowShelf:
                    if (! near (dc, sp.gainDb) || ! near (nyq, 0.0)) fail ("LowShelf invariants");
                    break;
                case BandType::HighShelf:
                    if (! near (dc, 0.0) || ! near (nyq, sp.gainDb)) fail ("HighShelf invariants");
                    break;
                case BandType::HighPass:
                    if (dc > -60.0 || ! near (nyq, 0.0)) fail ("HighPass invariants");
                    break;
                case BandType::LowPass:
                    if (! near (dc, 0.0) || nyq > -60.0) fail ("LowPass invariants");
                    break;
            }
            std::printf ("  %-10s maxAbsErr=%.2e  (DC=%.2f Nyq=%.2f f0=%.2f dB)\n",
                         typeName (sp.type), maxAbs, dc, nyq, atF0);
        }
    }

    void cascadeTest (double Fs)
    {
        std::printf ("Cascade @ Fs=%.0f\n", Fs);
        const int N = 1 << 16;
        const BandSpec bands[] = {
            { BandType::LowShelf,   200.0,  4.0, 0.707 },
            { BandType::Bell,      1000.0,  6.0, 2.0 },
            { BandType::Bell,      3000.0, -3.0, 1.5 },
            { BandType::HighShelf, 6000.0, -4.0, 0.707 },
        };

        // Measured: cascade real Biquads, impulse -> IR.
        factory_core::Biquad chain[4];
        for (int i = 0; i < 4; ++i)
            chain[i].setCoeffs (factory_core::designFilter (bands[i].type, bands[i].f0, bands[i].gainDb, bands[i].Q, Fs));
        std::vector<double> ir ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            double x = (n == 0) ? 1.0 : 0.0;
            for (auto& b : chain) x = b.processSample (x);
            ir[(size_t) n] = x;
        }

        OC ocs[4];
        for (int i = 0; i < 4; ++i)
            ocs[i] = oracleDesign (bands[i].type, bands[i].f0, bands[i].gainDb, bands[i].Q, Fs);

        double maxAbs = 0.0;
        const int Mgrid = 96;
        for (int k = 0; k < Mgrid; ++k)
        {
            const double f = 20.0 * std::pow ((0.49 * Fs) / 20.0, (double) k / (Mgrid - 1));
            const double w = 2.0 * kPi * f / Fs;
            cd Ho (1.0, 0.0);
            for (const auto& o : ocs) Ho *= oracleH (o, w);
            const cd Hm = dftAt (ir, w);
            maxAbs = std::max (maxAbs, std::abs (Hm - Ho));
            if (std::abs (Ho) > 0.01 && std::abs (magDb (Hm) - magDb (Ho)) > 0.05)
                fail ("cascade dB mismatch at f=" + std::to_string (f));
        }
        if (maxAbs > 1.0e-4) fail ("cascade complex abs err " + std::to_string (maxAbs));
        std::printf ("  maxAbsErr=%.2e\n", maxAbs);
    }

    void dynamicOffsetTest()
    {
        std::printf ("Dynamic offset function\n");
        const double thr = -30.0, range = -12.0;
        using factory_core::DynamicEqBand;
        if (std::abs (DynamicEqBand::dynamicOffsetDb (thr - 5.0, thr, range)) > 1e-12) fail ("offset below thr != 0");
        if (std::abs (DynamicEqBand::dynamicOffsetDb (thr + 100.0, thr, range) - range) > 1e-12) fail ("offset far above != range");
        if (std::abs (DynamicEqBand::dynamicOffsetDb (thr + 12.0, thr, range) - range * 0.5) > 1e-12) fail ("offset midpoint != range/2");
        // monotonic toward range
        double prev = 0.0;
        for (int i = 0; i <= 40; ++i)
        {
            const double off = DynamicEqBand::dynamicOffsetDb (thr + i, thr, range);
            if (off > prev + 1e-12) fail ("offset not monotonic (range<0)");
            prev = off;
        }
        std::printf ("  ok\n");
    }

    double measureBandGainDb (double Fs, double f0, double amp, int settle, int measure)
    {
        factory_core::DynamicEqBand band;
        band.setType (BandType::Bell);
        band.setFrequency (f0);
        band.setGainDb (6.0);
        band.setQ (2.0);
        band.setDynamics (true, -40.0, -12.0);
        band.setDynamicsTimes (5.0, 50.0);
        band.prepare (Fs);

        const double w = 2.0 * kPi * f0 / Fs;
        for (int i = 0; i < settle; ++i)
        {
            double l = amp * std::sin (w * i), r = l;
            band.processStereo (l, r);
        }
        double si = 0.0, so = 0.0;
        for (int i = 0; i < measure; ++i)
        {
            const double s = amp * std::sin (w * (settle + i));
            double l = s, r = s;
            band.processStereo (l, r);
            si += s * s;
            so += l * l;
        }
        return 10.0 * std::log10 (so / si);
    }

    void dynamicBandTest (double Fs)
    {
        std::printf ("Dynamic band steady-state @ Fs=%.0f\n", Fs);
        const double quiet = measureBandGainDb (Fs, 1000.0, 1.0e-3, 60000, 20000); // below threshold
        const double loud  = measureBandGainDb (Fs, 1000.0, 0.5,    60000, 20000); // above threshold+span

        if (std::abs (quiet - 6.0) > 0.7)  fail ("dynamic quiet gain " + std::to_string (quiet) + " != static +6");
        if (std::abs (loud - (-6.0)) > 0.7) fail ("dynamic loud gain " + std::to_string (loud) + " != +6-12");
        if (loud >= quiet)                 fail ("dynamics did not reduce gain under load");
        std::printf ("  quiet=%.2f dB  loud=%.2f dB\n", quiet, loud);
    }
}

int main (int argc, char** argv)
{
    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 96000.0 };

    dynamicOffsetTest();
    for (double Fs : rates)
    {
        perTypeTest (Fs);
        cascadeTest (Fs);
        dynamicBandTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
