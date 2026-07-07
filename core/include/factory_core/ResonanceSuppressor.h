#pragma once
//
// factory_core/ResonanceSuppressor.h — a Soothe-style dynamic resonance
// suppressor. A streaming STFT (Hann analysis+synthesis, 75% overlap, perfect
// reconstruction) estimates a local spectral envelope each frame in the
// log-magnitude domain (a log-mean over a log-symmetric window with the window's
// own centre excluded — a self-excluding notch — so a peak cannot lift its own
// baseline and neighbouring resonances stop masking one another), measures the
// per-bin "excess" of the magnitude over that envelope (i.e. resonant peaks), and
// turns it into a per-bin gain reduction via a soft-knee contrast law (Selectivity
// sweeps its threshold/knee) in Soft mode or a finite-slope absolute law in Hard
// mode, scaled by the global Depth and a user reduction profile (a per-frequency
// multiplier — the "EQ-like" curve). The per-bin reduction is smoothed across
// frequency before being applied, then follows attack/release per bin. Detection
// can be stereo-linked (same gain on L/R, preserving the image) or per-channel.
// Mix rides the spectral gain rather than a time-domain dry/wet blend: the
// effective per-bin gain is gEff = 1 + Mix*(g - 1) (g = the detector's per-bin
// gain), applied in processFrame, so a Mix move is frame-rate quantised like
// Depth, and the aligned dry never blends against the wet in the time domain --
// the structural prerequisite for a later dual-resolution split (mixing dry and
// wet in time would comb a low-res dry against a high-res wet). Delta monitors
// the removed signal as dry - out (out already carries the Mix scaling, so the
// delta's Mix scaling is automatic and out + delta reconstructs the aligned dry).
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
            logMag.assign   ((size_t) (half + 1), 0.0);
            redDbBuf.assign ((size_t) (half + 1), 0.0);
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
        // Selectivity (0..1): sweeps the soft-knee contrast law that maps per-bin
        // excess to reduction — threshold T = 1 + 5*s dB, knee width W = 6 - 4*s dB.
        // s = 0.5 (T = 3.5, W = 4) is the nominal setting. See computeGains.
        void setSelectivity (double s) noexcept { selectivity = std::clamp (s, 0.0, 1.0); }
        // Frequency-domain smoothing width (octaves) applied to the per-bin
        // reduction before ballistics, to suppress single-bin comb artefacts /
        // musical noise. 0 disables it (bypass). See computeGains (A4).
        void setSmoothingWidth (double oct) noexcept { gainSmoothOct = std::clamp (oct, 0.0, 1.0 / 3.0); }

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

            const double ringL = out[0][(size_t) idx]; // OLA output: the Mix-scaled wet (gEff applied in processFrame)
            const double ringR = out[1][(size_t) idx];
            out[0][(size_t) idx] = 0.0;
            out[1][(size_t) idx] = 0.0;

            idx = (idx + 1) & mask;
            if (++hop >= H) { hop = 0; processFrame(); }

            // Active output. Mix now rides the spectral gain (gEff in processFrame),
            // so the OLA ring already holds the Mix-scaled output: the normal path
            // is that ring value verbatim -- no time-domain dry/wet blend. Delta
            // monitors the removed signal literally as dry - out, so out_normal +
            // out_delta reconstructs the aligned dry (exact in real arithmetic; to
            // within one rounding in floats) and the delta's Mix scaling is
            // automatic (out is already Mix-scaled).
            double outL, outR;
            if (delta)
            {
                outL = dryL - ringL;
                outR = dryR - ringR;
            }
            else
            {
                outL = ringL;
                outR = ringR;
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
        static constexpr double kLn10 = 2.30258509299404568402; // ln(10): nat-log excess -> dB
        // Absolute detection floor (dBFS): a bin quieter than this is treated as
        // silence and never suppressed. The soft-knee contrast gate is only a
        // *relative* one (peak-vs-local-envelope), so without an absolute floor the
        // detector paints a reduction "curtain" on the near-silent idle output of
        // some sources (e.g. a synth emitting ~-100 dBFS when a note is not held),
        // even though nothing audible is happening (issue #24). 0 dBFS here is a
        // full-scale sine, whose bin magnitude is N/4.
        static constexpr double kFloorDb = -80.0;
        // Hard (level-dependent) mode: Depth doubles as the internal absolute
        // threshold. Depth 0..1 sweeps the threshold from kHardThrHiDb (gentle,
        // only the hottest peaks touched) down to kHardThrLoDb (aggressive, deep
        // notches). Reduction is a finite-slope fraction of the resonant peak's
        // absolute level over that threshold, so the engine reacts to absolute
        // harmonic level (Soothe2 "Hard"), unlike Soft's adaptive/relative threshold.
        static constexpr double kHardThrHiDb = -6.0;
        static constexpr double kHardThrLoDb = -60.0;
        // Hard-mode finite slope (LISTENING CHECKPOINT — tune by ear, not by test):
        // reduction is kHardSlopeFrac of the peak's excess over the absolute
        // threshold, through a fixed kHardKneeDb soft knee. kHardSlopeFrac = 0.85
        // is a ~6.7:1 ratio (leaves 15% of the excess) rather than pinning the peak
        // to the threshold (the old infinite-ratio behaviour).
        static constexpr double kHardSlopeFrac = 0.85;
        static constexpr double kHardKneeDb    = 6.0;

        static double coeff (double ms, double rate) noexcept { return onePoleCoeffForMs (ms, rate); }

        // Soft-knee excess above a threshold T with knee width W (a standard
        // compressor knee, all in dB): 0 below the knee, a quadratic spline through
        // it, linear (x - T) above. Drives the Soft/Hard resonance gate (T, W from
        // Selectivity) and the Hard-mode finite slope (T = 0, W = kHardKneeDb).
        static double softKneeOver (double x, double T, double W) noexcept
        {
            const double d = x - T;
            if (W <= 0.0)      return std::max (0.0, d);
            if (d <= -0.5 * W) return 0.0;
            if (d >=  0.5 * W) return d;
            const double t = d + 0.5 * W;
            return t * t / (2.0 * W);
        }

        // Compute per-bin gain from a magnitude spectrum + persistent gain state.
        //
        // Detection runs in the log-magnitude domain (A1): the local envelope is a
        // log-mean over the log-symmetric window [k/wf, k*wf], with the window's own
        // centre [k/nf, k*nf] excluded (a self-excluding notch, nf = wf^(1/4)) so a
        // strong peak does not lift its own baseline and adjacent resonances stop
        // masking one another. The excess exDb = (L[k] - envLogMean) in dB then
        // drives a soft-knee contrast law (A2, Selectivity sets threshold/knee) in
        // Soft mode, or a finite-slope absolute law (A3) in Hard mode. The per-bin
        // reduction is smoothed across frequency (A4) before it becomes a linear
        // target for the attack/release smoother. Every step is O(1)/bin (prefix
        // sums) and allocation-free (scratch sized in prepare()).
        void computeGains (const std::vector<double>& mag, std::vector<double>& g) noexcept
        {
            const int half = N / 2;
            const double wf = std::pow (2.0, smoothOct);  // envelope half-width factor (log-freq)
            const double nf = std::pow (wf, 0.25);        // notch half-width factor (1/4 of the envelope width)
            // Raw-magnitude equivalent of kFloorDb (a full-scale sine bin is N/4).
            const double floorMag = 0.25 * (double) N * std::pow (10.0, kFloorDb / 20.0);

            // Log magnitude and its prefix sum (O(1) window means below).
            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k)
            {
                logMag[(size_t) k] = std::log (mag[(size_t) k] + 1.0e-12);
                prefix[(size_t) (k + 1)] = prefix[(size_t) k] + logMag[(size_t) k];
            }

            // Log-mean envelope with a self-excluding central notch.
            for (int k = 0; k <= half; ++k)
            {
                const int lo  = std::clamp ((int) std::floor (k / wf), 0, half);
                const int hi  = std::clamp ((int) std::ceil  (k * wf), lo, half);
                const int loN = std::clamp ((int) std::floor (k / nf), lo, hi);
                const int hiN = std::clamp ((int) std::ceil  (k * nf), loN, hi);
                const int winCount   = hi - lo + 1;
                const int notchCount = hiN - loN + 1;
                const double winSum  = prefix[(size_t) (hi + 1)] - prefix[(size_t) lo];
                // Fall back to the whole-window mean when the window is tiny or the
                // notch would leave too few bins to estimate a stable baseline.
                if (winCount <= 6 || winCount - notchCount < 4)
                    env[(size_t) k] = winSum / (double) winCount;
                else
                {
                    const double notchSum = prefix[(size_t) (hiN + 1)] - prefix[(size_t) loN];
                    env[(size_t) k] = (winSum - notchSum) / (double) (winCount - notchCount);
                }
            }

            // Per-bin reduction target (dB), pre-smoothing / pre-ballistics.
            const double T = 1.0 + 5.0 * selectivity;   // soft-knee threshold (dB)
            const double W = 6.0 - 4.0 * selectivity;   // soft-knee width (dB)
            for (int k = 0; k <= half; ++k)
            {
                double redDb = 0.0;
                // Only act on in-range bins that carry audible energy: the
                // absolute floor keeps the engine idle on near-silent input.
                if (k >= lowBin && k <= highBin && depth > 0.0 && mag[(size_t) k] > floorMag)
                {
                    const double exDb = (logMag[(size_t) k] - env[(size_t) k]) * (20.0 / kLn10);
                    if (mode == 0)
                    {
                        // Soft: adaptive threshold. Reduction scales with Depth and
                        // the soft-knee excess of the relative prominence, so it is
                        // invariant to input level.
                        redDb = -depth * profile[(size_t) k] * softKneeOver (exDb, T, W); // negative = cut
                    }
                    else
                    {
                        // Hard: absolute threshold set by Depth, finite slope.
                        // Reduction is kHardSlopeFrac of the peak's absolute level
                        // (dBFS, 0 dB = full-scale sine bin = N/4) over the threshold
                        // through a fixed knee, so it tracks input level without
                        // pinning the peak to the threshold.
                        const double magDb = 20.0 * std::log10 ((mag[(size_t) k] + 1.0e-12) / (0.25 * (double) N));
                        const double d     = std::clamp (depth / 1.5, 0.0, 1.0);
                        const double thrDb = kHardThrHiDb + d * (kHardThrLoDb - kHardThrHiDb);
                        if (softKneeOver (exDb, T, W) > 0.0)  // only suppress resonant peaks
                            redDb = -profile[(size_t) k] * kHardSlopeFrac * softKneeOver (magDb - thrDb, 0.0, kHardKneeDb);
                    }
                    redDb = std::max (redDb, -48.0);
                }
                redDbBuf[(size_t) k] = redDb;
            }

            // A4: smooth the per-bin reduction across frequency (two cascaded
            // variable-width box averages ~ a triangular window, constant width in
            // log-frequency) to suppress single-bin comb artefacts / musical noise.
            // gainSmoothOct == 0 bypasses it. O(1)/bin via prefix sums; edges clamp
            // the window to [0, half] and renormalise by the actual bin count.
            if (gainSmoothOct > 0.0)
            {
                const double smFac = std::pow (2.0, 0.5 * gainSmoothOct) - 1.0;
                for (int pass = 0; pass < 2; ++pass)
                {
                    prefix[0] = 0.0;
                    for (int k = 0; k <= half; ++k) prefix[(size_t) (k + 1)] = prefix[(size_t) k] + redDbBuf[(size_t) k];
                    for (int k = 0; k <= half; ++k)
                    {
                        const int h   = std::max (1, (int) std::lround (k * smFac));
                        const int loE = std::max (0, k - h);
                        const int hiE = std::min (half, k + h);
                        redDbBuf[(size_t) k] = (prefix[(size_t) (hiE + 1)] - prefix[(size_t) loE]) / (double) (hiE - loE + 1);
                    }
                }
            }

            // Linearise the (smoothed) target and advance the per-bin ballistics.
            for (int k = 0; k <= half; ++k)
            {
                const double target = std::pow (10.0, redDbBuf[(size_t) k] / 20.0);
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
            // spectrum scaled by the per-bin detector gain g -- NOT the Mix-scaled
            // gEff applied below. The reduction curtain is the amount the detector
            // is trying to remove, a full-processing view independent of Mix (v1
            // display semantics). Gain is real, so |spec * g| = mag * g.
            for (int k = 0; k <= half; ++k)
                dispMag[(size_t) k] = std::max (mag[0][(size_t) k] * gain[0][(size_t) k],
                                                mag[1][(size_t) k] * gain[1][(size_t) k]) / (0.5 * (double) N);

            // Apply the real per-bin gains, keeping the spectrum Hermitian. Mix
            // rides the spectral gain: the effective gain is gEff = 1 + mix*(g - 1),
            // with mix read once per frame, so a Mix move is frame-rate quantised
            // (like Depth) and dry/wet never blend in the time domain -- the
            // structural prerequisite for a later dual-resolution split. Strict
            // endpoints: mix >= 1 uses g verbatim (1 + (g - 1) can round 1 ULP off
            // g for g < 0.5, so the full-wet path stays bit-identical to the
            // pre-Mix-in-gain engine); mix <= 0 needs no branch since 1 + 0*(g - 1)
            // is exactly 1 in IEEE. gEff for bin k and its Hermitian mirror N-k
            // share one value (as the underlying g does).
            const double m = mix;
            const bool fullWet = (m >= 1.0);
            auto gEff = [m, fullWet] (double g) noexcept { return fullWet ? g : 1.0 + m * (g - 1.0); };

            spec[0][0] *= gEff (gain[0][0]);               spec[1][0] *= gEff (gain[1][0]);
            spec[0][(size_t) half] *= gEff (gain[0][(size_t) half]);
            spec[1][(size_t) half] *= gEff (gain[1][(size_t) half]);
            for (int k = 1; k < half; ++k)
            {
                const double ge0 = gEff (gain[0][(size_t) k]);
                const double ge1 = gEff (gain[1][(size_t) k]);
                spec[0][(size_t) k]       *= ge0;
                spec[0][(size_t) (N - k)] *= ge0;
                spec[1][(size_t) k]       *= ge1;
                spec[1][(size_t) (N - k)] *= ge1;
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
        std::vector<double> det, env, logMag, redDbBuf, prefix, profile;
        std::vector<double> dispMag, dispRedDb;

        // params
        double depth = 0.0;
        double smoothOct = 0.5;
        double selectivity = 0.5;          // soft-knee contrast sweep (A2)
        double gainSmoothOct = 1.0 / 12.0; // per-bin reduction smoothing width, octaves (A4)
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
