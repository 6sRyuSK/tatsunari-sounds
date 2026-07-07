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
// removed signal, scaled by Mix (mix*(dry-wet)) so it matches exactly what Mix is
// subtracting from the dry; Mix blends the reduction into the latency-aligned dry.
//
// Bypass (setBypassed) is latency-preserving: the engine keeps running (frames,
// gains and display snapshots update as normal) and only the output stage
// crossfades, over a short per-sample ramp, toward the latency-aligned dry -- so
// the reported latency N is held and host PDC stays intact. Full bypass passes the
// aligned dry through bit-transparently.
//
// Latency = window length N (report it to the host). Header-only, JUCE-independent,
// allocation-free in process(): all buffers are sized in prepare().
//
#include "FFT.h"
#include "SmoothingCoeff.h"

#include <algorithm>
#include <array>
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
            bypassStep = 1.0 / (kBypassRampSec * fs); // per-sample bypass ramp step (no per-sample division)

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

            for (int ch = 0; ch < 2; ++ch)
            {
                in[ch].assign   ((size_t) N, 0.0);
                out[ch].assign  ((size_t) N, 0.0);
                spec[ch].assign ((size_t) N, cd {});
                mag[ch].assign  ((size_t) (half + 1), 0.0);
                gain[ch].assign ((size_t) (half + 1), 1.0);
            }
            det.assign  ((size_t) (half + 1), 0.0);
            env.assign  ((size_t) (half + 1), 0.0);
            prefix.assign ((size_t) (half + 2), 0.0);
            dispMag.assign ((size_t) (half + 1), 0.0);
            dispRedDb.assign ((size_t) (half + 1), 0.0);
            profile.assign ((size_t) (half + 1), 1.0);

            setRange (20.0, 20000.0);
            setTimes (8.0, 80.0);
            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                std::fill (in[ch].begin(),   in[ch].end(),   0.0);
                std::fill (out[ch].begin(),  out[ch].end(),  0.0);
                std::fill (gain[ch].begin(), gain[ch].end(), 1.0);
            }
            idx = 0;
            hop = 0;
            bypassMix = bypassTarget; // snap the crossfade (no fade right after prepare/reset)
        }

        // --- configuration ---
        void setDepth     (double d)   noexcept { depth = std::max (0.0, d); }       // 0..~1.5
        void setSharpness (double oct) noexcept { smoothOct = std::clamp (oct, 0.05, 2.0); }
        void setMix       (double m)   noexcept { mix = std::clamp (m, 0.0, 1.0); }
        void setDelta     (bool b)     noexcept { delta = b; }
        // Latency-preserving bypass: crossfades the output to the aligned dry
        // without changing the reported latency (the engine keeps running).
        void setBypassed  (bool b)     noexcept { bypassTarget = b ? 1.0 : 0.0; }
        void setStereoLink (bool b)    noexcept { link = b; }
        // Detection mode: 0 = Soft (adaptive threshold, level-independent),
        // 1 = Hard (absolute level threshold, level-dependent). See computeGains.
        void setMode      (int m)      noexcept { mode = std::clamp (m, 0, 1); }

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
            const double dryL = in[0][(size_t) idx]; // input from N samples ago (== wet latency)
            const double dryR = in[1][(size_t) idx];
            in[0][(size_t) idx] = l;
            in[1][(size_t) idx] = r;

            const double wetL = out[0][(size_t) idx];
            const double wetR = out[1][(size_t) idx];
            out[0][(size_t) idx] = 0.0;
            out[1][(size_t) idx] = 0.0;

            idx = (idx + 1) & mask;
            if (++hop >= H) { hop = 0; processFrame(); }

            // Normal (active) output: the mix-scaled removed signal in delta mode,
            // otherwise the latency-aligned dry/wet blend. Delta is scaled by mix
            // (mix*(dry-wet)) -- the exact IEEE sign inversion of the normal path's
            // blend term mix*(wet-dry) -- so out_normal + out_delta reconstructs the
            // dry (== dry in exact arithmetic; to within one rounding in floats).
            double outL, outR;
            if (delta)
            {
                outL = mix * (dryL - wetL);
                outR = mix * (dryR - wetR);
            }
            else
            {
                outL = dryL + mix * (wetL - dryL);
                outR = dryR + mix * (wetR - dryR);
            }

            // Latency-preserving bypass: ramp bypassMix toward its target (0 =
            // active, 1 = bypassed) by the step precomputed in prepare, then blend
            // toward the aligned dry. The ramp clamps to the ends, so full bypass
            // is bit-transparent and full active passes the normal output untouched.
            if (bypassMix < bypassTarget)      bypassMix = std::min (bypassTarget, bypassMix + bypassStep);
            else if (bypassMix > bypassTarget) bypassMix = std::max (bypassTarget, bypassMix - bypassStep);

            if (bypassMix >= 1.0)
            {
                l = dryL; // fully bypassed: aligned dry, untouched
                r = dryR;
            }
            else if (bypassMix <= 0.0)
            {
                l = outL; // fully active
                r = outR;
            }
            else
            {
                // Linear crossfade (dry and wet are highly correlated: no equal-power).
                l = outL + bypassMix * (dryL - outL);
                r = outR + bypassMix * (dryR - outR);
            }
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;
        static constexpr double kBypassRampSec = 0.010; // latency-preserving bypass crossfade ramp
        static constexpr double kThreshDb = 3.0; // excess below this is not a resonance
        // Absolute detection floor (dBFS): a bin quieter than this is treated as
        // silence and never suppressed. kThreshDb is only a *relative* gate
        // (peak-vs-local-envelope), so without an absolute floor the detector
        // paints a reduction "curtain" on the near-silent idle output of some
        // sources (e.g. a synth emitting ~-100 dBFS when a note is not held),
        // even though nothing audible is happening (issue #24). 0 dBFS here is a
        // full-scale sine, whose bin magnitude is N/4.
        static constexpr double kFloorDb = -80.0;
        // Hard (level-dependent) mode: Depth doubles as the internal absolute
        // threshold. Depth 0..1 sweeps the threshold from kHardThrHiDb (gentle,
        // only the hottest peaks touched) down to kHardThrLoDb (aggressive, deep
        // notches). Reduction is the absolute excess of a resonant peak over that
        // threshold, so the engine reacts to absolute harmonic level (Soothe2
        // "Hard"), unlike Soft's adaptive/relative threshold.
        static constexpr double kHardThrHiDb = -6.0;
        static constexpr double kHardThrLoDb = -60.0;

        static double coeff (double ms, double rate) noexcept { return onePoleCoeffForMs (ms, rate); }

        // Compute per-bin gain from a magnitude spectrum + persistent gain state.
        void computeGains (const std::vector<double>& mag, std::vector<double>& g) noexcept
        {
            const int half = N / 2;
            const double wf = std::pow (2.0, smoothOct); // envelope half-width factor
            // Raw-magnitude equivalent of kFloorDb (a full-scale sine bin is N/4).
            const double floorMag = 0.25 * (double) N * std::pow (10.0, kFloorDb / 20.0);

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
                // Only act on in-range bins that carry audible energy: the
                // absolute floor keeps the engine idle on near-silent input.
                if (k >= lowBin && k <= highBin && depth > 0.0 && mag[(size_t) k] > floorMag)
                {
                    // Relative prominence over the local envelope identifies a
                    // genuine resonant peak in both modes (broadband / noisy
                    // material sits within kThreshDb and is left alone).
                    const double exDb = 20.0 * std::log10 ((mag[(size_t) k] + 1.0e-12) / (env[(size_t) k] + 1.0e-12));
                    double redDb = 0.0;
                    if (mode == 0)
                    {
                        // Soft: adaptive threshold. Reduction scales with Depth and
                        // the relative excess, so it is invariant to input level.
                        const double over = std::max (0.0, exDb - kThreshDb);
                        redDb = -depth * profile[(size_t) k] * over;  // negative = cut
                    }
                    else
                    {
                        // Hard: absolute threshold set by Depth. Reduction is the
                        // peak's absolute level (dBFS, 0 dB = full-scale sine bin
                        // = N/4) above that threshold, so it tracks input level.
                        const double magDb = 20.0 * std::log10 ((mag[(size_t) k] + 1.0e-12) / (0.25 * (double) N));
                        const double d     = std::clamp (depth / 1.5, 0.0, 1.0);
                        const double thrDb = kHardThrHiDb + d * (kHardThrLoDb - kHardThrHiDb);
                        if (exDb > kThreshDb)  // only suppress resonant peaks
                            redDb = -profile[(size_t) k] * std::max (0.0, magDb - thrDb);
                    }
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
                for (int ch = 0; ch < 2; ++ch)
                    spec[ch][(size_t) k] = cd (in[ch][(size_t) p] * win[(size_t) k], 0.0);
            }
            for (int ch = 0; ch < 2; ++ch) fft.forward (spec[ch].data());

            for (int k = 0; k <= half; ++k)
                for (int ch = 0; ch < 2; ++ch)
                    mag[ch][(size_t) k] = std::abs (spec[ch][(size_t) k]);

            if (link)
            {
                for (int k = 0; k <= half; ++k) det[(size_t) k] = std::max (mag[0][(size_t) k], mag[1][(size_t) k]);
                computeGains (det, gain[0]);
                for (int k = 0; k <= half; ++k)
                {
                    gain[1][(size_t) k] = gain[0][(size_t) k];
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (gain[0][(size_t) k], 1.0e-6));
                }
            }
            else
            {
                computeGains (mag[0], gain[0]);
                computeGains (mag[1], gain[1]);
                for (int k = 0; k <= half; ++k)
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (std::min (gain[0][(size_t) k], gain[1][(size_t) k]), 1.0e-6));
            }

            // Analyser shows the post-processing (output) magnitude: the input
            // spectrum scaled by the per-bin gain just computed (the reduction
            // curtain shows that gain). Gain is real, so |spec * g| = mag * g.
            for (int k = 0; k <= half; ++k)
                dispMag[(size_t) k] = std::max (mag[0][(size_t) k] * gain[0][(size_t) k],
                                                mag[1][(size_t) k] * gain[1][(size_t) k]) / (0.5 * (double) N);

            // Apply the real per-bin gains, keeping the spectrum Hermitian.
            spec[0][0] *= gain[0][0];               spec[1][0] *= gain[1][0];
            spec[0][(size_t) half] *= gain[0][(size_t) half];
            spec[1][(size_t) half] *= gain[1][(size_t) half];
            for (int k = 1; k < half; ++k)
            {
                spec[0][(size_t) k]       *= gain[0][(size_t) k];
                spec[0][(size_t) (N - k)] *= gain[0][(size_t) k];
                spec[1][(size_t) k]       *= gain[1][(size_t) k];
                spec[1][(size_t) (N - k)] *= gain[1][(size_t) k];
            }

            fft.inverse (spec[0].data());
            fft.inverse (spec[1].data());

            // Windowed overlap-add back to the output ring (same alignment).
            for (int k = 0; k < N; ++k)
            {
                const int p = (idx + k) & mask;
                const double w = win[(size_t) k] * olaScale;
                out[0][(size_t) p] += spec[0][(size_t) k].real() * w;
                out[1][(size_t) p] += spec[1][(size_t) k].real() * w;
            }
        }

        FFT fft;
        double fs = 44100.0;
        int ord = 11, N = 2048, H = 512, mask = 2047;

        std::vector<double> win; double olaScale = 1.0;
        std::array<std::vector<double>, 2> in, out;   // [ch] input / output rings
        int idx = 0, hop = 0;
        std::array<std::vector<cd>, 2> spec;          // [ch] analysis/synthesis spectrum

        std::array<std::vector<double>, 2> mag, gain; // [ch] magnitude / per-bin gain
        std::vector<double> det, env, prefix, profile;
        std::vector<double> dispMag, dispRedDb;

        // params
        double depth = 0.0;
        double smoothOct = 0.5;
        int    lowBin = 1, highBin = 1024;
        double atkCoeff = 0.0, relCoeff = 0.0;
        double mix = 1.0;
        bool   delta = false;
        bool   link = true;
        int    mode = 0; // 0 = Soft (adaptive), 1 = Hard (absolute level)
        // Latency-preserving bypass crossfade (0 = active, 1 = bypassed); step set in prepare.
        double bypassMix = 0.0, bypassTarget = 0.0, bypassStep = 0.0;
    };
} // namespace factory_core
