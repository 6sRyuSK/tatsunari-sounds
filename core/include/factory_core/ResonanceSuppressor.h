#pragma once
//
// factory_core/ResonanceSuppressor.h — a Soothe-style dynamic resonance
// suppressor. A streaming STFT (Hann analysis+synthesis, 75% overlap, perfect
// reconstruction) estimates a smoothed spectral envelope each frame, measures the
// per-bin "excess" of the magnitude over that envelope (i.e. resonant peaks), and
// applies a per-bin dynamic gain reduction proportional to the excess, the global
// Depth, and a user reduction profile (a per-frequency multiplier — the "EQ-like"
// curve). Reduction follows attack/release per bin. Detection can be stereo-linked
// (same gain on L/R, preserving the image) or per-channel. Delta monitors the
// removed signal; Mix blends with the latency-aligned dry.
//
// Latency = window length N (report it to the host). Header-only, JUCE-independent,
// allocation-free in process(): all buffers are sized in prepare().
//
#include "FFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace factory_core
{
    class ResonanceSuppressor
    {
    public:
        using cd = std::complex<double>;

        // order: FFT order; N = 1<<order (default 2048). Overlap fixed at 4x.
        void prepare (double sampleRate, int order = 11)
        {
            fs    = sampleRate;
            ord   = order;
            N     = 1 << order;
            H     = N / 4;            // 75% overlap
            mask  = N - 1;
            fft.prepare (order);

            const int half = N / 2;
            win.assign ((size_t) N, 0.0);
            for (int i = 0; i < N; ++i)
                win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / N);

            // Steady-state overlap-add normalisation (sum of win^2 at hop H).
            double acc = 0.0;
            for (int m = -(N / H); m <= (N / H); ++m)
            {
                const int p = half - m * H;
                if (p >= 0 && p < N) acc += win[(size_t) p] * win[(size_t) p];
            }
            olaScale = (acc > 0.0) ? 1.0 / acc : 1.0;

            inL.assign ((size_t) N, 0.0);  inR.assign ((size_t) N, 0.0);
            outL.assign ((size_t) N, 0.0); outR.assign ((size_t) N, 0.0);
            specL.assign ((size_t) N, cd {}); specR.assign ((size_t) N, cd {});

            magL.assign ((size_t) (half + 1), 0.0);
            magR.assign ((size_t) (half + 1), 0.0);
            det.assign  ((size_t) (half + 1), 0.0);
            env.assign  ((size_t) (half + 1), 0.0);
            prefix.assign ((size_t) (half + 2), 0.0);
            gainL.assign ((size_t) (half + 1), 1.0);
            gainR.assign ((size_t) (half + 1), 1.0);
            dispMag.assign ((size_t) (half + 1), 0.0);
            dispRedDb.assign ((size_t) (half + 1), 0.0);
            profile.assign ((size_t) (half + 1), 1.0);

            setRange (20.0, 20000.0);
            setTimes (8.0, 80.0);
            reset();
        }

        void reset() noexcept
        {
            std::fill (inL.begin(),  inL.end(),  0.0);
            std::fill (inR.begin(),  inR.end(),  0.0);
            std::fill (outL.begin(), outL.end(), 0.0);
            std::fill (outR.begin(), outR.end(), 0.0);
            std::fill (gainL.begin(), gainL.end(), 1.0);
            std::fill (gainR.begin(), gainR.end(), 1.0);
            idx = 0;
            hop = 0;
        }

        // --- configuration ---
        void setDepth     (double d)   noexcept { depth = std::max (0.0, d); }       // 0..~1.5
        void setSharpness (double oct) noexcept { smoothOct = std::clamp (oct, 0.05, 2.0); }
        void setMix       (double m)   noexcept { mix = std::clamp (m, 0.0, 1.0); }
        void setDelta     (bool b)     noexcept { delta = b; }
        void setStereoLink (bool b)    noexcept { link = b; }

        void setRange (double lowHz, double highHz) noexcept
        {
            const int half = N / 2;
            lowBin  = std::clamp ((int) std::round (lowHz  * N / fs), 1, half);
            highBin = std::clamp ((int) std::round (highHz * N / fs), 1, half);
            if (highBin < lowBin) std::swap (lowBin, highBin);
        }

        void setTimes (double attackMs, double releaseMs) noexcept
        {
            const double frameRate = fs / H; // frames per second
            atkCoeff = coeff (attackMs,  frameRate);
            relCoeff = coeff (releaseMs, frameRate);
        }

        // Per-bin reduction multiplier (>=0; 1 = nominal). Copied; call per block.
        void setProfile (const double* mul, int count) noexcept
        {
            const int n = std::min (count, (int) profile.size());
            for (int k = 0; k < n; ++k) profile[(size_t) k] = std::max (0.0, mul[(size_t) k]);
        }

        int latencySamples() const noexcept { return N; }
        int numBins()        const noexcept { return N / 2 + 1; }
        double binToHz (int k) const noexcept { return (double) k * fs / N; }

        // Display snapshots (GUI thread reads; benign race, like a meter).
        const double* magnitudeDb (double* scratch) const noexcept
        {
            for (size_t k = 0; k < dispMag.size(); ++k)
                scratch[k] = 20.0 * std::log10 (dispMag[k] + 1.0e-12);
            return scratch;
        }
        const double* reductionDb() const noexcept { return dispRedDb.data(); }

        // Process one stereo sample in place. Output is latency-aligned dry/wet.
        void process (double& l, double& r) noexcept
        {
            const double dryL = inL[(size_t) idx]; // input from N samples ago (== wet latency)
            const double dryR = inR[(size_t) idx];
            inL[(size_t) idx] = l;
            inR[(size_t) idx] = r;

            const double wetL = outL[(size_t) idx];
            const double wetR = outR[(size_t) idx];
            outL[(size_t) idx] = 0.0;
            outR[(size_t) idx] = 0.0;

            idx = (idx + 1) & mask;
            if (++hop >= H) { hop = 0; processFrame(); }

            if (delta)
            {
                l = dryL - wetL;
                r = dryR - wetR;
            }
            else
            {
                l = dryL + mix * (wetL - dryL);
                r = dryR + mix * (wetR - dryR);
            }
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;
        static constexpr double kThreshDb = 3.0; // excess below this is not a resonance

        static double coeff (double ms, double rate) noexcept
        {
            const double t = ms * 1.0e-3;
            if (t <= 0.0 || rate <= 0.0) return 0.0;
            return std::exp (-1.0 / (t * rate));
        }

        // Compute per-bin gain from a magnitude spectrum + persistent gain state.
        void computeGains (const std::vector<double>& mag, std::vector<double>& g) noexcept
        {
            const int half = N / 2;
            const double wf = std::pow (2.0, smoothOct); // envelope half-width factor

            // Smoothed envelope via running prefix sum of magnitude.
            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k) prefix[(size_t) (k + 1)] = prefix[(size_t) k] + mag[(size_t) k];
            for (int k = 0; k <= half; ++k)
            {
                int lo = (int) std::floor (k / wf);
                int hi = (int) std::ceil  (k * wf);
                lo = std::clamp (lo, 0, half);
                hi = std::clamp (hi, lo, half);
                env[(size_t) k] = (prefix[(size_t) (hi + 1)] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
            }

            for (int k = 0; k <= half; ++k)
            {
                double target = 1.0;
                if (k >= lowBin && k <= highBin && depth > 0.0)
                {
                    const double exDb = 20.0 * std::log10 ((mag[(size_t) k] + 1.0e-12) / (env[(size_t) k] + 1.0e-12));
                    // Only act on genuine peaks: ignore excess within a small
                    // threshold so broadband / noisy material is left alone.
                    const double over = std::max (0.0, exDb - kThreshDb);
                    double redDb = -depth * profile[(size_t) k] * over;  // negative = cut
                    redDb = std::max (redDb, -48.0);
                    target = std::pow (10.0, redDb / 20.0);
                }
                const double c = (target < g[(size_t) k]) ? atkCoeff : relCoeff; // attack when cutting more
                g[(size_t) k] = c * g[(size_t) k] + (1.0 - c) * target;
            }
        }

        void processFrame() noexcept
        {
            const int half = N / 2;

            // Windowed analysis frame (oldest sample at idx).
            for (int k = 0; k < N; ++k)
            {
                const int p = (idx + k) & mask;
                specL[(size_t) k] = cd (inL[(size_t) p] * win[(size_t) k], 0.0);
                specR[(size_t) k] = cd (inR[(size_t) p] * win[(size_t) k], 0.0);
            }
            fft.forward (specL.data());
            fft.forward (specR.data());

            for (int k = 0; k <= half; ++k)
            {
                magL[(size_t) k] = std::abs (specL[(size_t) k]);
                magR[(size_t) k] = std::abs (specR[(size_t) k]);
                dispMag[(size_t) k] = std::max (magL[(size_t) k], magR[(size_t) k]) / (0.5 * N);
            }

            if (link)
            {
                for (int k = 0; k <= half; ++k) det[(size_t) k] = std::max (magL[(size_t) k], magR[(size_t) k]);
                computeGains (det, gainL);
                for (int k = 0; k <= half; ++k)
                {
                    gainR[(size_t) k] = gainL[(size_t) k];
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (gainL[(size_t) k], 1.0e-6));
                }
            }
            else
            {
                computeGains (magL, gainL);
                computeGains (magR, gainR);
                for (int k = 0; k <= half; ++k)
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (std::min (gainL[(size_t) k], gainR[(size_t) k]), 1.0e-6));
            }

            // Apply the real per-bin gains, keeping the spectrum Hermitian.
            specL[0] *= gainL[0];               specR[0] *= gainR[0];
            specL[(size_t) half] *= gainL[(size_t) half];
            specR[(size_t) half] *= gainR[(size_t) half];
            for (int k = 1; k < half; ++k)
            {
                specL[(size_t) k]       *= gainL[(size_t) k];
                specL[(size_t) (N - k)] *= gainL[(size_t) k];
                specR[(size_t) k]       *= gainR[(size_t) k];
                specR[(size_t) (N - k)] *= gainR[(size_t) k];
            }

            fft.inverse (specL.data());
            fft.inverse (specR.data());

            // Windowed overlap-add back to the output ring (same alignment).
            for (int k = 0; k < N; ++k)
            {
                const int p = (idx + k) & mask;
                const double w = win[(size_t) k] * olaScale;
                outL[(size_t) p] += specL[(size_t) k].real() * w;
                outR[(size_t) p] += specR[(size_t) k].real() * w;
            }
        }

        FFT fft;
        double fs = 44100.0;
        int ord = 11, N = 2048, H = 512, mask = 2047;

        std::vector<double> win; double olaScale = 1.0;
        std::vector<double> inL, inR, outL, outR;
        int idx = 0, hop = 0;
        std::vector<cd> specL, specR;

        std::vector<double> magL, magR, det, env, prefix, gainL, gainR, profile;
        std::vector<double> dispMag, dispRedDb;

        // params
        double depth = 0.0;
        double smoothOct = 0.5;
        int    lowBin = 1, highBin = 1024;
        double atkCoeff = 0.0, relCoeff = 0.0;
        double mix = 1.0;
        bool   delta = false;
        bool   link = true;
    };
} // namespace factory_core
