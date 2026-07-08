#pragma once
//
// factory_core/ResonanceSuppressor.h — a Soothe-style dynamic resonance
// suppressor. A streaming STFT (Hann analysis+synthesis, perfect reconstruction)
// estimates a local spectral envelope each frame in the log-magnitude domain (a
// log-mean over a log-symmetric window with the window's own centre excluded — a
// self-excluding notch — so a peak cannot lift its own baseline and neighbouring
// resonances stop masking one another), measures the per-bin "excess" of the
// magnitude over that envelope (i.e. resonant peaks), and turns it into a per-bin
// gain reduction via a soft-knee contrast law (Selectivity sweeps its
// threshold/knee) in Soft mode or a finite-slope absolute law in Hard mode, scaled
// by the global Depth and a user reduction profile (a per-frequency multiplier —
// the "EQ-like" curve). The per-bin reduction is smoothed across frequency before
// being applied, then follows attack/release per bin. Detection can be
// stereo-linked (same gain on L/R, preserving the image), per-channel, or a
// continuous blend between the two (setLinkAmount: a log-domain lerp of the
// per-channel gains toward a shared max-detection gain); it can also run off a
// separate external-detector input (setExternalDetector / the 4-arg process),
// whose spectrum drives the gains while they are still applied to the main
// signal (detection and the suppressed signal are decoupled). Mix rides
// the spectral gain rather than a time-domain dry/wet blend: the effective per-bin
// gain is gEff = 1 + Mix*(g - 1) (g = the detector's per-bin gain), applied in
// processFrame, so a Mix move is frame-rate quantised like Depth, and the aligned
// dry never blends against the wet in the time domain. Delta monitors the removed
// signal as dry - out (out already carries the Mix scaling, so out + delta
// reconstructs the aligned dry).
//
// Per-bin ballistics + Tilt (B2-1): the attack/release time constants are a
// per-bin array, not a scalar. Tilt (-1..+1) scales each bin's time constant by
// s(k) = (f_k / 1 kHz)^(-Tilt * log10 4) about a 1 kHz pivot (Tilt = +1 makes
// 10 kHz react 4x faster and 100 Hz 4x slower). With Tilt = 0 the array is uniform
// (one code path). Tilt != 0 recomputes the array in 1024-bin chunks across
// processFrame calls (old/new coefficients may coexist mid-rebuild) so no single
// audio callback pays for a full-band pow() sweep; Tilt = 0 is a cheap synchronous
// uniform fill.
//
// Overlap / Quality (B2-2): three quality settings trade CPU/latency for time
// resolution. Fast (order N-1, 4x overlap), Normal (order N, 8x, default), High
// (order N+1, 8x). The analysis/synthesis rings are sized to the maximum order and
// the active window is the most recent N samples, so switching Quality keeps the
// raw input history; only the OLA ring, per-bin gains and frame phase reset. A
// Quality switch changes the reported latency (= active window length N), so it
// holds the latency-aligned dry for N samples while the cleared OLA ring refills,
// then fades the wet back in over the existing 10 ms bypass ramp. All three
// (order, hop) configurations precompute their window + overlap-add normalisation
// and FFT in prepare(), so a switch never allocates or computes a window.
//
// Bypass (setBypassed) is latency-preserving: the engine keeps running (frames,
// gains and display snapshots update as normal) and only the output stage
// crossfades, over a short per-sample ramp, toward the latency-aligned dry -- so
// the reported latency N is held and host PDC stays intact. Full bypass passes the
// aligned dry through bit-transparently.
//
// Latency = active window length N (report it to the host). Header-only,
// JUCE-independent, allocation-free in process(): all buffers are sized in
// prepare() at the maximum order.
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

        // order: the Normal-quality FFT order; N = 1<<order (default 2048). maxOrder
        // caps the allocation and the High-quality order (0 -> order+1, clamped to
        // 14). Fast = order-1 @ 4x overlap, Normal = order @ 8x, High = order+1 @ 8x
        // (hop = N/overlap). Existing callers pass just (sampleRate, order) and get
        // Normal with the reported latency N -- the same latency the 4x engine had.
        void prepare (double sampleRate, int order = 11, int maxOrderIn = 0)
        {
            fs          = sampleRate;
            normalOrder = order;
            maxOrder    = (maxOrderIn > 0) ? maxOrderIn : (order + 1);
            maxOrder    = std::clamp (maxOrder, order, kMaxAllowedOrder);

            // Per-quality (order, hop) configs; each precomputes its Hann window,
            // steady-state overlap-add normalisation and FFT so a switch is a
            // pointer swap (no allocation / window build on the audio thread).
            for (int q = 0; q < kNumQuality; ++q)
            {
                const int oq = std::clamp (normalOrder + (q - 1), 1, maxOrder);
                const int ov = (q == 0) ? 4 : 8;   // Fast = 4x, Normal/High = 8x
                const int Nq = 1 << oq;
                const int Hq = Nq / ov;
                orderCfg[(size_t) q] = oq;
                hopCfg[(size_t) q]   = Hq;
                fftCfg[(size_t) q].prepare (oq);

                auto& w = winCfg[(size_t) q];
                w.assign ((size_t) Nq, 0.0);
                for (int i = 0; i < Nq; ++i)
                    w[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / Nq);

                // Steady-state overlap-add normalisation (sum of win^2 at hop Hq).
                double acc = 0.0;
                for (int m = -(Nq / Hq); m <= (Nq / Hq); ++m)
                {
                    const int p = Nq / 2 - m * Hq;
                    if (p >= 0 && p < Nq) acc += w[(size_t) p] * w[(size_t) p];
                }
                olaScaleCfg[(size_t) q] = (acc > 0.0) ? 1.0 / acc : 1.0;
            }

            bufLen  = 1 << maxOrder;
            bufMask = bufLen - 1;
            maxBins = bufLen / 2 + 1;
            bypassStep = 1.0 / (kBypassRampSec * fs); // per-sample bypass ramp step

            for (int ch = 0; ch < 2; ++ch)
            {
                in[ch].assign    ((size_t) bufLen, 0.0);
                inDet[ch].assign ((size_t) bufLen, 0.0);
                out[ch].assign   ((size_t) bufLen, 0.0);
                spec[ch].assign  ((size_t) bufLen, cd {});
                mag[ch].assign   ((size_t) maxBins, 0.0);
                magDet[ch].assign((size_t) maxBins, 0.0);
                gain[ch].assign  ((size_t) maxBins, 1.0);
                gOut[ch].assign  ((size_t) maxBins, 1.0);
            }
            specDet.assign  ((size_t) bufLen, cd {});
            gainM.assign    ((size_t) maxBins, 1.0);
            det.assign      ((size_t) maxBins, 0.0);
            env.assign      ((size_t) maxBins, 0.0);
            logMag.assign   ((size_t) maxBins, 0.0);
            redDbBuf.assign ((size_t) maxBins, 0.0);
            prefix.assign   ((size_t) (maxBins + 1), 0.0);
            dispMag.assign  ((size_t) maxBins, 0.0);
            dispRedDb.assign((size_t) maxBins, 0.0);
            profile.assign  ((size_t) maxBins, 1.0);

            // Per-bin ballistics coefficient arrays + the order-independent ln(k)
            // table for the Tilt scaling (computed once here).
            atkCoef.assign ((size_t) maxBins, 0.0);
            relCoef.assign ((size_t) maxBins, 0.0);
            lnK.assign     ((size_t) maxBins, 0.0);
            for (int k = 1; k < maxBins; ++k) lnK[(size_t) k] = std::log ((double) k);

            quality = qualityReq = 1; // Normal
            applyActiveConfig();

            rangeLoHz = 20.0; rangeHiHz = 20000.0;
            updateRangeBins();

            atkMsCur = 8.0; relMsCur = 80.0; tiltCur = 0.0;
            coefFrameRate = fs / (double) H;
            rebuildAllCoefNow();

            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                std::fill (in[ch].begin(),    in[ch].end(),    0.0);
                std::fill (inDet[ch].begin(), inDet[ch].end(), 0.0);
                std::fill (out[ch].begin(),   out[ch].end(),   0.0);
                std::fill (gain[ch].begin(),  gain[ch].end(),  1.0);
            }
            std::fill (gainM.begin(), gainM.end(), 1.0);
            idx = 0;
            hop = 0;
            switchHold = 0;           // no refill hold pending
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
        // Continuous stereo-link amount (0..1, default 1). Effective link amount
        // lambda = link ? linkAmount : 0, so setStereoLink(false) still forces
        // per-channel. lambda == 1 is the v1 linked path, lambda == 0 the v1
        // per-channel path (both bit-for-bit preserved); 0 < lambda < 1 blends.
        void setLinkAmount (double amt) noexcept { linkAmount = std::clamp (amt, 0.0, 1.0); }
        // External-detector input (default off). When on, detection reads a separate
        // detector-input spectrum (fed via the 4-arg process) instead of the main
        // signal, while the gains still apply to the main l/r. Off is bit-identical
        // to v1 (the detector FFT never runs).
        void setExternalDetector (bool b) noexcept { extDet = b; }
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

        // Frequency-dependent time constant tilt (-1..+1, default 0). s(k) =
        // (f_k / 1 kHz)^(-Tilt * log10 4): Tilt +1 makes highs react 4x faster and
        // lows 4x slower about a 1 kHz pivot. Tilt 0 -> uniform ballistics.
        void setTilt (double t) noexcept
        {
            t = std::clamp (t, -1.0, 1.0);
            if (std::abs (t - tiltCur) > 1.0e-9) { tiltCur = t; requestCoefUpdate(); }
        }

        void setRange (double lowHz, double highHz) noexcept
        {
            rangeLoHz = lowHz; rangeHiHz = highHz;
            updateRangeBins();
        }

        void setTimes (double attackMs, double releaseMs) noexcept
        {
            if (std::abs (attackMs - atkMsCur) > 1.0e-6 || std::abs (releaseMs - relMsCur) > 1.0e-6)
            {
                atkMsCur = attackMs; relMsCur = releaseMs;
                requestCoefUpdate();
            }
        }

        // Quality: 0 = Fast (order-1, 4x), 1 = Normal (order, 8x), 2 = High
        // (order+1, 8x). Latched; the swap happens at the next frame boundary in
        // process() (config swap + OLA/gain/phase clear, input history kept, then a
        // hold+ramp masks the latency change). No-op if unchanged.
        void setQuality (int q) noexcept { qualityReq = std::clamp (q, 0, kNumQuality - 1); }

        // Per-bin reduction multiplier (>=0; 1 = nominal). Copied; call per block.
        void setProfile (const double* mul, int count) noexcept
        {
            const int n = std::min (count, (int) profile.size());
            for (int k = 0; k < n; ++k) profile[(size_t) k] = std::max (0.0, mul[(size_t) k]);
        }

        int latencySamples() const noexcept { return N; }
        int hopSamples()     const noexcept { return H; }
        int numBins()        const noexcept { return N / 2 + 1; }
        int currentQuality() const noexcept { return quality; }
        double binToHz (int k) const noexcept { return (double) k * fs / N; }

        // Display snapshots (GUI thread reads; benign race, like a meter). Only the
        // active bins are written, so a caller scratch sized for the Normal order is
        // never overrun by a larger allocation.
        const double* magnitudeDb (double* scratch) const noexcept
        {
            const int nb = N / 2 + 1;
            for (int k = 0; k < nb; ++k)
                scratch[k] = 20.0 * std::log10 (dispMag[(size_t) k] + 1.0e-12);
            return scratch;
        }
        const double* reductionDb() const noexcept { return dispRedDb.data(); }

        // Process one stereo sample in place. Output is latency-aligned dry/wet.
        // The 2-arg form detects on the main signal; it delegates to the 4-arg form
        // with dl = l, dr = r, so it is behaviour- and bit-identical to v1.
        void process (double& l, double& r) noexcept { process (l, r, l, r); }

        // 4-arg form: (dl, dr) is a separate detector input. It is always written to
        // the detector ring, but that ring is only READ (windowed FFT) when
        // setExternalDetector(true); otherwise detection uses the main spectrum and
        // this is a zero-FFT, bit-identical path. When external detection is on, the
        // gains derived from (dl, dr) are applied to the main (l, r).
        void process (double& l, double& r, double dl, double dr) noexcept
        {
            // Dry / OLA read positions sit N samples behind the write pointer (when
            // N == the ring length this collapses to the classic read-before-write).
            const int rd = (idx + bufLen - N) & bufMask;
            const double dryL = in[0][(size_t) rd]; // input from N samples ago (== wet latency)
            const double dryR = in[1][(size_t) rd];
            in[0][(size_t) idx] = l;
            in[1][(size_t) idx] = r;
            inDet[0][(size_t) idx] = dl; // detector-input ring (read only when extDet)
            inDet[1][(size_t) idx] = dr;

            const double ringL = out[0][(size_t) rd]; // OLA output: the Mix-scaled wet
            const double ringR = out[1][(size_t) rd];
            out[0][(size_t) rd] = 0.0;
            out[1][(size_t) rd] = 0.0;

            idx = (idx + 1) & bufMask;
            bool didSwitch = false;
            if (++hop >= H)
            {
                hop = 0;
                if (qualityReq != quality) { applyQualitySwitch(); didSwitch = true; }
                processFrame();
            }

            // Active output. The OLA ring already holds the Mix-scaled output, so the
            // normal path is that ring value verbatim -- no time-domain dry/wet blend.
            // Delta monitors the removed signal literally as dry - out.
            double outL, outR;
            if (delta) { outL = dryL - ringL; outR = dryR - ringR; }
            else       { outL = ringL;        outR = ringR;        }

            // Quality-switch refill hold: while the freshly cleared OLA ring refills,
            // force the latency-aligned dry (bit-exact ring read, N == the new
            // latency), then hand off to the bypass ramp so the wet fades back in over
            // the existing 10 ms crossfade. The switch sample itself is excluded (its
            // dryL is still aligned to the old latency); the hold spans the following
            // N samples, where dryL already reflects the new N.
            if (switchHold > 0 && ! didSwitch)
            {
                --switchHold;
                if (switchHold == 0) bypassMix = 1.0; // start the ramp from fully dry
                l = dryL; r = dryR;
                return;
            }

            // Latency-preserving bypass: ramp bypassMix toward its target (0 = active,
            // 1 = bypassed), then blend toward the aligned dry. Clamped ends keep full
            // bypass bit-transparent and full active untouched.
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
        static constexpr int    kNumQuality = 3;
        static constexpr int    kMaxAllowedOrder = 14;   // hard cap on the High-quality order
        static constexpr int    kCoefChunk = 1024;       // per-bin ballistics rebuilt this many bins per frame
        // Tilt exponent: log10(4). s(k) = (f_k/1kHz)^(-Tilt*kTiltExp), so Tilt=+1
        // scales a decade of frequency by 4x (10 kHz 4x faster, 100 Hz 4x slower).
        static constexpr double kTiltExp = 0.60205999132796239;
        // Absolute detection floor (dBFS): a bin quieter than this is treated as
        // silence and never suppressed. 0 dBFS here is a full-scale sine, whose bin
        // magnitude is N/4. (issue #24)
        static constexpr double kFloorDb = -80.0;
        // Hard (level-dependent) mode: Depth doubles as the internal absolute
        // threshold, swept from kHardThrHiDb (gentle) down to kHardThrLoDb
        // (aggressive). Reduction is a finite-slope fraction of the resonant peak's
        // absolute level over that threshold.
        static constexpr double kHardThrHiDb = -6.0;
        static constexpr double kHardThrLoDb = -60.0;
        // Hard-mode finite slope (LISTENING CHECKPOINT — tune by ear, not by test):
        // kHardSlopeFrac of the peak's excess over the absolute threshold, through a
        // fixed kHardKneeDb soft knee (~6.7:1, leaves 15% of the excess).
        static constexpr double kHardSlopeFrac = 0.85;
        static constexpr double kHardKneeDb    = 6.0;

        // Point the active (ord, N, H, olaScale) at the current Quality config.
        void applyActiveConfig() noexcept
        {
            ord      = orderCfg[(size_t) quality];
            N        = 1 << ord;
            H        = hopCfg[(size_t) quality];
            olaScale = olaScaleCfg[(size_t) quality];
        }

        void updateRangeBins() noexcept
        {
            const int half = N / 2;
            lowBin  = std::clamp ((int) std::round (rangeLoHz * N / fs), 1, half);
            highBin = std::clamp ((int) std::round (rangeHiHz * N / fs), 1, half);
            if (highBin < lowBin) std::swap (lowBin, highBin);
        }

        // --- per-bin ballistics coefficients (Tilt) ------------------------------
        // Tilt=0 is a uniform array: a cheap synchronous fill (2 exp + a std::fill,
        // no per-bin transcendentals, so no audio-thread spike). Tilt!=0 needs a
        // per-bin pow() sweep, spread over frames in kCoefChunk-sized chunks.
        void requestCoefUpdate() noexcept
        {
            if (atkCoef.empty()) return; // not prepared yet
            if (std::abs (tiltCur) < 1.0e-12)
            {
                fillCoefUniform();
                coefRebuilding = false;
            }
            else
            {
                coefRebuildPos = 0;
                coefRebuilding = true;
            }
        }

        void fillCoefUniform() noexcept
        {
            const int half = N / 2;
            const double a = onePoleCoeffForMs (std::clamp (atkMsCur, 0.05, 5000.0), coefFrameRate);
            const double r = onePoleCoeffForMs (std::clamp (relMsCur, 0.05, 5000.0), coefFrameRate);
            std::fill (atkCoef.begin(), atkCoef.begin() + (half + 1), a);
            std::fill (relCoef.begin(), relCoef.begin() + (half + 1), r);
        }

        // atkCoef/relCoef for bins [lo, hi) from the committed (atk, rel, tilt,
        // frameRate). ln(f_k/1kHz) = lnK[k] + ln(fs/(N*1000)), so the tilt scale is a
        // single exp per bin (bin 0 is pinned to s = 1).
        void computeCoefRange (int lo, int hi) noexcept
        {
            const double off = std::log (fs / ((double) N * 1000.0));
            const double e   = -tiltCur * kTiltExp;
            for (int k = lo; k < hi; ++k)
            {
                const double s = (k == 0) ? 1.0 : std::exp (e * (lnK[(size_t) k] + off));
                atkCoef[(size_t) k] = onePoleCoeffForMs (std::clamp (atkMsCur * s, 0.05, 5000.0), coefFrameRate);
                relCoef[(size_t) k] = onePoleCoeffForMs (std::clamp (relMsCur * s, 0.05, 5000.0), coefFrameRate);
            }
        }

        // Synchronous full rebuild (prepare / quality-switch is not on the hot
        // per-frame budget the chunking protects).
        void rebuildAllCoefNow() noexcept
        {
            const int half = N / 2;
            if (std::abs (tiltCur) < 1.0e-12) fillCoefUniform();
            else                              computeCoefRange (0, half + 1);
            coefRebuilding = false;
        }

        // Advance an in-progress Tilt rebuild by one chunk (called once per frame).
        void stepCoefRebuild() noexcept
        {
            if (! coefRebuilding) return;
            const int half = N / 2;
            const int hi = std::min (coefRebuildPos + kCoefChunk, half + 1);
            computeCoefRange (coefRebuildPos, hi);
            coefRebuildPos = hi;
            if (coefRebuildPos > half) coefRebuilding = false;
        }

        // Apply a latched Quality request at a frame boundary: swap the config, clear
        // the OLA ring / per-bin gain / frame phase (KEEP the raw input history), and
        // arm the N-sample refill hold. The frame-rate change re-derives the
        // ballistics coefficients.
        void applyQualitySwitch() noexcept
        {
            quality = qualityReq;
            applyActiveConfig();
            updateRangeBins();

            const int half = N / 2;
            for (int ch = 0; ch < 2; ++ch)
            {
                std::fill (out[ch].begin(), out[ch].end(), 0.0);
                std::fill (gain[ch].begin(), gain[ch].begin() + (half + 1), 1.0);
            }
            std::fill (gainM.begin(), gainM.begin() + (half + 1), 1.0);
            hop = 0;
            switchHold = N;                 // hold the aligned dry while the OLA refills
            coefFrameRate = fs / (double) H;
            requestCoefUpdate();            // frame-rate change: rebuild ballistics
        }

        // Soft-knee excess above a threshold T with knee width W (all in dB).
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
        // (Detection A1..A4 unchanged; only the attack/release step reads the per-bin
        // coefficient arrays instead of a scalar.)
        void computeGains (const std::vector<double>& mag, std::vector<double>& g) noexcept
        {
            const int half = N / 2;
            const double wf = std::pow (2.0, smoothOct);  // envelope half-width factor (log-freq)
            const double nf = std::pow (wf, 0.25);        // notch half-width factor (1/4 of the envelope width)
            const double floorMag = 0.25 * (double) N * std::pow (10.0, kFloorDb / 20.0);

            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k)
            {
                logMag[(size_t) k] = std::log (mag[(size_t) k] + 1.0e-12);
                prefix[(size_t) (k + 1)] = prefix[(size_t) k] + logMag[(size_t) k];
            }

            for (int k = 0; k <= half; ++k)
            {
                const int lo  = std::clamp ((int) std::floor (k / wf), 0, half);
                const int hi  = std::clamp ((int) std::ceil  (k * wf), lo, half);
                const int loN = std::clamp ((int) std::floor (k / nf), lo, hi);
                const int hiN = std::clamp ((int) std::ceil  (k * nf), loN, hi);
                const int winCount   = hi - lo + 1;
                const int notchCount = hiN - loN + 1;
                const double winSum  = prefix[(size_t) (hi + 1)] - prefix[(size_t) lo];
                if (winCount <= 6 || winCount - notchCount < 4)
                    env[(size_t) k] = winSum / (double) winCount;
                else
                {
                    const double notchSum = prefix[(size_t) (hiN + 1)] - prefix[(size_t) loN];
                    env[(size_t) k] = (winSum - notchSum) / (double) (winCount - notchCount);
                }
            }

            const double T = 1.0 + 5.0 * selectivity;   // soft-knee threshold (dB)
            const double W = 6.0 - 4.0 * selectivity;   // soft-knee width (dB)
            for (int k = 0; k <= half; ++k)
            {
                double redDb = 0.0;
                if (k >= lowBin && k <= highBin && depth > 0.0 && mag[(size_t) k] > floorMag)
                {
                    const double exDb = (logMag[(size_t) k] - env[(size_t) k]) * (20.0 / kLn10);
                    if (mode == 0)
                    {
                        redDb = -depth * profile[(size_t) k] * softKneeOver (exDb, T, W); // negative = cut
                    }
                    else
                    {
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
            // The coefficient is per bin (Tilt); with Tilt=0 the array is uniform.
            for (int k = 0; k <= half; ++k)
            {
                const double target = std::pow (10.0, redDbBuf[(size_t) k] / 20.0);
                const double c = (target < g[(size_t) k]) ? atkCoef[(size_t) k] : relCoef[(size_t) k]; // attack when cutting more
                g[(size_t) k] = c * g[(size_t) k] + (1.0 - c) * target;
            }
        }

        void processFrame() noexcept
        {
            stepCoefRebuild(); // advance an in-progress Tilt rebuild (<=1024 bins)

            const int half = N / 2;
            const auto& w  = winCfg[(size_t) quality];
            auto& fftA     = fftCfg[(size_t) quality];

            // Windowed analysis frame = the most recent N samples (ending at the
            // sample just written), read from the ring N behind the write pointer.
            const int base = (idx + bufLen - N) & bufMask;
            for (int k = 0; k < N; ++k)
            {
                const int p = (base + k) & bufMask;
                for (int ch = 0; ch < 2; ++ch)
                    spec[ch][(size_t) k] = cd (in[ch][(size_t) p] * w[(size_t) k], 0.0);
            }
            for (int ch = 0; ch < 2; ++ch) fftA.forward (spec[ch].data());

            for (int k = 0; k <= half; ++k)
                for (int ch = 0; ch < 2; ++ch)
                    mag[ch][(size_t) k] = std::abs (spec[ch][(size_t) k]); // MAIN magnitude (analyser + default detection)

            // External detector (3A-2): when enabled, DETECTION magnitudes come from a
            // separate detector-input frame's own windowed FFT; mag[] stays the MAIN
            // magnitude (used for the analyser display below), so extDet == false runs
            // no extra FFT and is bit-identical to v1. The gains still apply to spec[].
            if (extDet)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    for (int k = 0; k < N; ++k)
                    {
                        const int p = (base + k) & bufMask;
                        specDet[(size_t) k] = cd (inDet[ch][(size_t) p] * w[(size_t) k], 0.0);
                    }
                    fftA.forward (specDet.data());
                    for (int k = 0; k <= half; ++k) magDet[ch][(size_t) k] = std::abs (specDet[(size_t) k]);
                }
            }
            const std::vector<double>& dmag0 = extDet ? magDet[0] : mag[0];
            const std::vector<double>& dmag1 = extDet ? magDet[1] : mag[1];

            // Continuous stereo link (3A-1): effective lambda in [0,1]. lambda == 1 is
            // the v1 linked path (one max-detection gain on both channels), lambda == 0
            // the v1 per-channel path -- both preserved bit-for-bit. 0 < lambda < 1
            // blends the per-channel gains gL/gR with a linked reference gM (its own
            // ballistics state gainM) in the log domain, once per frame after
            // ballistics: g'ch = exp((1-lambda)*ln(gch) + lambda*ln(gM)) (gains are in
            // (0,1], clamped to a 1e-6 floor so the logs are finite).
            const double lambda = link ? linkAmount : 0.0;
            const std::vector<double>* gA = &gain[0]; // effective per-channel output gains
            const std::vector<double>* gB = &gain[1];
            if (lambda >= 1.0)
            {
                for (int k = 0; k <= half; ++k) det[(size_t) k] = std::max (dmag0[(size_t) k], dmag1[(size_t) k]);
                computeGains (det, gain[0]);
                for (int k = 0; k <= half; ++k)
                {
                    gain[1][(size_t) k] = gain[0][(size_t) k];
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (gain[0][(size_t) k], 1.0e-6));
                }
            }
            else if (lambda <= 0.0)
            {
                computeGains (dmag0, gain[0]);
                computeGains (dmag1, gain[1]);
                for (int k = 0; k <= half; ++k)
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (std::min (gain[0][(size_t) k], gain[1][(size_t) k]), 1.0e-6));
            }
            else
            {
                computeGains (dmag0, gain[0]);   // per-channel gL (own ballistics state)
                computeGains (dmag1, gain[1]);   // per-channel gR (own ballistics state)
                for (int k = 0; k <= half; ++k) det[(size_t) k] = std::max (dmag0[(size_t) k], dmag1[(size_t) k]);
                computeGains (det, gainM);        // linked reference gM (own ballistics state)
                const double om = 1.0 - lambda;
                for (int k = 0; k <= half; ++k)
                {
                    const double lnM = std::log (std::max (gainM[(size_t) k], 1.0e-6));
                    const double g0 = std::exp (om * std::log (std::max (gain[0][(size_t) k], 1.0e-6)) + lambda * lnM);
                    const double g1 = std::exp (om * std::log (std::max (gain[1][(size_t) k], 1.0e-6)) + lambda * lnM);
                    gOut[0][(size_t) k] = g0;
                    gOut[1][(size_t) k] = g1;
                    dispRedDb[(size_t) k] = 20.0 * std::log10 (std::max (std::min (g0, g1), 1.0e-6));
                }
                gA = &gOut[0];
                gB = &gOut[1];
            }
            const std::vector<double>& g0Eff = *gA;
            const std::vector<double>& g1Eff = *gB;

            // Analyser shows the post-processing (output) magnitude: the input
            // spectrum scaled by the per-bin detector gain g -- NOT the Mix-scaled
            // gEff applied below (v1 display semantics). Gain is real, so |spec*g| = mag*g.
            for (int k = 0; k <= half; ++k)
                dispMag[(size_t) k] = std::max (mag[0][(size_t) k] * g0Eff[(size_t) k],
                                                mag[1][(size_t) k] * g1Eff[(size_t) k]) / (0.5 * (double) N);

            // Apply the real per-bin gains, keeping the spectrum Hermitian. Mix rides
            // the spectral gain: gEff = 1 + mix*(g - 1), mix read once per frame.
            const double m = mix;
            const bool fullWet = (m >= 1.0);
            auto gEff = [m, fullWet] (double g) noexcept { return fullWet ? g : 1.0 + m * (g - 1.0); };

            spec[0][0] *= gEff (g0Eff[0]);               spec[1][0] *= gEff (g1Eff[0]);
            spec[0][(size_t) half] *= gEff (g0Eff[(size_t) half]);
            spec[1][(size_t) half] *= gEff (g1Eff[(size_t) half]);
            for (int k = 1; k < half; ++k)
            {
                const double ge0 = gEff (g0Eff[(size_t) k]);
                const double ge1 = gEff (g1Eff[(size_t) k]);
                spec[0][(size_t) k]       *= ge0;
                spec[0][(size_t) (N - k)] *= ge0;
                spec[1][(size_t) k]       *= ge1;
                spec[1][(size_t) (N - k)] *= ge1;
            }

            fftA.inverse (spec[0].data());
            fftA.inverse (spec[1].data());

            // Windowed overlap-add back to the output ring (same alignment as analysis).
            for (int k = 0; k < N; ++k)
            {
                const int p = (base + k) & bufMask;
                const double wk = w[(size_t) k] * olaScale;
                out[0][(size_t) p] += spec[0][(size_t) k].real() * wk;
                out[1][(size_t) p] += spec[1][(size_t) k].real() * wk;
            }
        }

        // --- per-quality precomputed configs (window / hop / olaScale / FFT) ---
        std::array<int, kNumQuality>                 orderCfg { 10, 11, 12 };
        std::array<int, kNumQuality>                 hopCfg   { 256, 256, 512 };
        std::array<double, kNumQuality>              olaScaleCfg { 1.0, 1.0, 1.0 };
        std::array<std::vector<double>, kNumQuality> winCfg;
        std::array<FFT, kNumQuality>                 fftCfg;

        double fs = 44100.0;
        int normalOrder = 11, maxOrder = 12;
        int bufLen = 4096, bufMask = 4095, maxBins = 2049;

        int quality = 1, qualityReq = 1;    // 0 Fast, 1 Normal, 2 High
        int ord = 11, N = 2048, H = 256;    // active order / window / hop
        double olaScale = 1.0;

        std::array<std::vector<double>, 2> in, out;   // [ch] input / output rings (sized 1<<maxOrder)
        std::array<std::vector<double>, 2> inDet;     // [ch] external-detector input rings (read only when extDet)
        int idx = 0, hop = 0;
        int switchHold = 0;                           // Quality-switch refill hold (samples of forced dry)
        std::array<std::vector<cd>, 2> spec;          // [ch] analysis/synthesis spectrum
        std::vector<cd> specDet;                      // detector-frame FFT scratch (extDet only)

        std::array<std::vector<double>, 2> mag, gain; // [ch] MAIN magnitude / per-bin gain
        std::array<std::vector<double>, 2> magDet;    // [ch] detector magnitude (extDet only)
        std::array<std::vector<double>, 2> gOut;      // [ch] blended effective output gain (0<lambda<1)
        std::vector<double> gainM;                    // linked-reference ballistics state (0<lambda<1)
        std::vector<double> det, env, logMag, redDbBuf, prefix, profile;
        std::vector<double> dispMag, dispRedDb;

        // per-bin ballistics
        std::vector<double> atkCoef, relCoef, lnK;
        double atkMsCur = 8.0, relMsCur = 80.0, tiltCur = 0.0;
        double coefFrameRate = 0.0;
        bool coefRebuilding = false;
        int  coefRebuildPos = 0;

        // params
        double depth = 0.0;
        double smoothOct = 0.5;
        double selectivity = 0.5;          // soft-knee contrast sweep (A2)
        double gainSmoothOct = 1.0 / 12.0; // per-bin reduction smoothing width, octaves (A4)
        double rangeLoHz = 20.0, rangeHiHz = 20000.0;
        int    lowBin = 1, highBin = 1024;
        double mix = 1.0;
        bool   delta = false;
        bool   link = true;
        double linkAmount = 1.0;         // continuous link amount (effective lambda = link ? linkAmount : 0)
        bool   extDet = false;           // external-detector input enabled (3A-2)
        int    mode = 0; // 0 = Soft (adaptive), 1 = Hard (absolute level)
        // Latency-preserving bypass crossfade (0 = active, 1 = bypassed); step set in prepare.
        double bypassMix = 0.0, bypassTarget = 0.0, bypassStep = 0.0;
    };
} // namespace factory_core
