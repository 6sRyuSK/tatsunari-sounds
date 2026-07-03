#pragma once
//
// factory_core/Oversampler.h — integer-ratio (1x / 2x / 4x) oversampler with a
// Kaiser windowed-sinc anti-image / anti-alias FIR. Header-only, JUCE-independent,
// allocation-free in process. Built specifically for wrapping a NON-LINEAR section
// (a harmonic shaper) whose output has energy above the host Nyquist.
//
// Why not PolyphaseResampler? That resampler's group delay is a fixed 31 INPUT
// samples, so an M:1 round trip lands at a NON-integer host-sample latency
// (e.g. 4x: 31 + 31/4 = 38.75 host samples) — a plugin cannot report that
// exactly. This integer-ratio design instead sizes the FIR half-width H so the
// round-trip delay 2H/M is an EXACT integer number of host samples.
//
// Design (single-stage FIR at the OS rate M*fs_host):
//   * Kaiser windowed sinc, beta = 7.857 => ~80 dB stopband (same as
//     PolyphaseResampler). Cutoff = host Nyquist = 0.5/M cycles/OS-sample.
//   * Pass-band edge 0.45*fs_host, stop-band edge 0.55*fs_host: the transition
//     band straddles the host Nyquist so aliasing/imaging only folds into the
//     [0.45, 0.5]*fs_host don't-care region. Transition width dOmega = 0.628/M rad.
//   * Kaiser length estimate L-1 = (A-7.95)/(2.285*dOmega); the half-width H is
//     rounded UP until (2H % M) == 0, so the up+down round trip delay
//     latencyHostSamples() = 2H/M is an exact integer:
//        M=4: H=102 (L=205) -> 51    M=2: H=51 (L=103) -> 51    M=1: bypass -> 0
//     => HQ latency is a uniform 51 host samples at 44.1..96 kHz, and 0 at
//        176.4/192 kHz and in Zero-Latency mode (both use M=1).
//
// Polyphase: up-sampling is zero-stuff-equivalent (branch p reads taps p, p+M, …)
// with a gain-M compensation; down-sampling filters at the OS rate and decimates
// by M with the decimation phase aligned to the input grid (OS index == 0 mod M).
// M == 1 is a pure pass-through (identity, zero latency).
//
#include <algorithm>
#include <cmath>
#include <vector>

namespace factory_core
{
    class Oversampler
    {
    public:
        static constexpr double kBeta = 7.857;   // Kaiser beta for ~80 dB stopband
        static constexpr double kAttenDb = 80.0; // stopband attenuation used for L estimate

        // Prepare for `factor` (1, 2 or 4) at `hostRate`, sized for up to
        // `maxBlock` host samples per process call. Allocates here only.
        void prepare (double hostRate, int factor, int maxBlock)
        {
            M          = std::max (1, factor);
            hostFs     = hostRate;
            maxHostN   = std::max (1, maxBlock);

            if (M == 1)
            {
                // Identity bypass: no filter, no latency.
                H = 0;
                coeffs.clear();
                extUp.assign ((size_t) maxHostN, 0.0f);
                extDown.assign ((size_t) maxHostN, 0.0f);
                reset();
                return;
            }

            // Kaiser length estimate for an 80 dB stopband and a transition width
            // dOmega = 2*pi*(0.1/M) rad/sample at the OS rate, then round the
            // half-width H up until 2H is a multiple of M (integer host latency).
            constexpr double pi = 3.14159265358979323846;
            const double dOmega = 2.0 * pi * (0.1 / (double) M);
            const double order  = (kAttenDb - 7.95) / (2.285 * dOmega);   // L - 1
            H = (int) std::ceil (order / 2.0);
            while ((2 * H) % M != 0) ++H;

            buildKernel();

            // History extensions: [past .. current block]. The up FIR spans the
            // full 2H OS-sample kernel, i.e. it reaches back 2H/M host inputs.
            histUp   = (2 * H) / M + 2;            // past host inputs the up FIR reaches
            histDown = 2 * H;                      // past OS samples the down FIR reaches (= L-1)
            extUp.assign   ((size_t) (histUp   + maxHostN),       0.0f);
            extDown.assign ((size_t) (histDown + maxHostN * M),   0.0f);
            reset();
        }

        void reset() noexcept
        {
            std::fill (extUp.begin(),   extUp.end(),   0.0f);
            std::fill (extDown.begin(), extDown.end(), 0.0f);
        }

        int    factor()              const noexcept { return M; }
        double osRate()              const noexcept { return hostFs * (double) M; }
        // Round-trip (up + down) latency in host samples = 2H/M (exact integer).
        int    latencyHostSamples()  const noexcept { return (M == 1) ? 0 : (2 * H) / M; }

        // Up-sample `n` host samples in `in` to `n*M` OS samples in `out`.
        void processUp (const float* in, int n, float* out) noexcept
        {
            if (M == 1) { std::copy (in, in + n, out); return; }

            // ext = [ histUp past inputs ][ in[0..n-1] ]
            for (int i = 0; i < n; ++i)
                extUp[(size_t) (histUp + i)] = in[i];

            const int L = 2 * H + 1;
            for (int i = 0; i < n; ++i)
            {
                const int base = histUp + i;      // ext index of in[i]
                for (int p = 0; p < M; ++p)
                {
                    double acc = 0.0;
                    // y[iM+p] = M * sum_m h[mM+p] * x[i-m]
                    for (int j = p, m = 0; j < L; j += M, ++m)
                        acc += (double) coeffs[(size_t) j] * (double) extUp[(size_t) (base - m)];
                    out[(size_t) (i * M + p)] = (float) ((double) M * acc);
                }
            }

            // Retain the last histUp inputs as next block's left context.
            for (int k = 0; k < histUp; ++k)
                extUp[(size_t) k] = extUp[(size_t) (n + k)];
        }

        // Down-sample `n*M` OS samples in `in` to `n` host samples in `out`.
        void processDown (const float* in, int n, float* out) noexcept
        {
            if (M == 1) { std::copy (in, in + n, out); return; }

            const int nOs = n * M;
            // ext = [ histDown past OS samples ][ in[0..nOs-1] ]
            for (int i = 0; i < nOs; ++i)
                extDown[(size_t) (histDown + i)] = in[i];

            const int L = 2 * H + 1;
            for (int i = 0; i < n; ++i)
            {
                const int centre = histDown + i * M;   // ext index of OS sample i*M
                double acc = 0.0;
                // y[i] = sum_j h[j] * s[i*M - j]
                for (int j = 0; j < L; ++j)
                    acc += (double) coeffs[(size_t) j] * (double) extDown[(size_t) (centre - j)];
                out[i] = (float) acc;
            }

            // Retain the last histDown OS samples as next block's left context.
            for (int k = 0; k < histDown; ++k)
                extDown[(size_t) k] = extDown[(size_t) (nOs + k)];
        }

    private:
        void buildKernel()
        {
            constexpr double pi = 3.14159265358979323846;
            const int L = 2 * H + 1;
            coeffs.assign ((size_t) L, 0.0f);

            const double i0b = besselI0 (kBeta);
            double sum = 0.0;
            std::vector<double> h ((size_t) L, 0.0);
            for (int j = 0; j < L; ++j)
            {
                const int n = j - H;              // OS-sample offset, symmetric
                // Ideal low-pass at cutoff 0.5/M cyc/sample: (1/M) sinc(n/M)
                //   = sin(pi*n/M)/(pi*n).
                const double s = (n == 0) ? (1.0 / (double) M)
                                          : std::sin (pi * (double) n / (double) M) / (pi * (double) n);
                const double r = (double) n / (double) H;
                const double win = besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
                h[(size_t) j] = s * win;
                sum += h[(size_t) j];
            }
            // Normalise to unity DC gain (down path); up path adds the M factor.
            for (int j = 0; j < L; ++j)
                coeffs[(size_t) j] = (float) (h[(size_t) j] / sum);
        }

        static double besselI0 (double x) noexcept
        {
            double s = 1.0, term = 1.0;
            const double y = x * x / 4.0;
            for (int k = 1; k < 64; ++k)
            {
                term *= y / (double) (k * k);
                s += term;
                if (term < 1.0e-14 * s) break;
            }
            return s;
        }

        int    M        = 1;
        int    H        = 0;
        double hostFs   = 44100.0;
        int    maxHostN = 0;
        int    histUp   = 0;
        int    histDown = 0;
        std::vector<float>  coeffs;   // symmetric FIR, length 2H+1, unity DC gain
        std::vector<float>  extUp;    // [history ++ input]  for up-sampling
        std::vector<float>  extDown;  // [history ++ OS input] for down-sampling
    };
} // namespace factory_core
