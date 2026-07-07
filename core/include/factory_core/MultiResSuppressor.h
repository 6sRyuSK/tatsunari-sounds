#pragma once
//
// factory_core/MultiResSuppressor.h — a dual-resolution synthesis front end for
// the ResonanceSuppressor. It quadruples the time resolution of the high band by
// splitting the input into two bands with an LR4 crossover and running each band
// through its own streaming-STFT suppressor at a different window length:
//
//   in ┬─ LR4 split @ kSplitHz ─ low ───────────────→ RS_low  (order O)   ─┐
//      │                         high ─ DelayLine ───→ RS_high (order O-2) ─┴─ sum → out
//      ├─ dryRef: (low+high) delayed by N_L  (= allpass(in), for Delta)
//      └─ dryPure: raw input delayed by N_L  (bit-transparent bypass source)
//
// The low band keeps the full-length window (order O -> N_L = 1<<O), so its
// frequency resolution is unchanged; the high band uses a window two orders
// shorter (order O-2 -> N_S = N_L/4) whose hop is therefore 4x shorter, so its
// frames advance 4x faster and it reacts 4x sooner to transient harshness. The
// high band is pre-delayed by N_L - N_S so both paths carry the same total
// latency N_L and the two suppressor outputs are sample-aligned when summed.
//
// Crossover frequency (kSplitHz = 3 kHz, a LISTENING CHECKPOINT — 1.5k vs 3k is a
// tuning-by-ear decision, not a test): the split has to sit ABOVE the main
// harshness band (roughly 1.5-3 kHz) so that band is handled by the fast
// small-window high path. A 1.5 kHz split would push 1.5-3 kHz into the small
// window's coarse low bins (93.75 Hz per bin @ 48k Normal) where the detector
// can't resolve individual resonances; 3 kHz keeps the harshness region in the
// large-window low path's fine bins while still giving the high path the airband
// where fast response matters most.
//
// Reconstruction (why the sum is phase-correct): for an LR4 crossover low + high
// is an allpass — flat magnitude, so at depth 0 (no suppression) each band STFT
// perfectly reconstructs its own input and the sum is allpass(in) delayed by N_L.
// The Delta reference (dryRef) is exactly that: the SAME split's (low+high)
// delayed by N_L, so Delta = dryRef - sum monitors only the removed resonances
// (its depth-0 residual is just the two STFTs' reconstruction error, ~1e-15).
// Note low+high is taken from the same split that feeds the two bands, so it is
// bit-identical to a separate "unity twin" LR4 (same class, same coefficients,
// same input) while costing no extra filtering and never able to drift from the
// band split.
//
// Mix and Delta ride the COMPOSITE, never the sub-engines: Mix is forwarded to
// both sub-engines (each blends its own band's dry reconstruction against its wet
// on the spectral gain, gEff = 1 + Mix*(g-1)), so the band sums add up to the
// correct global Mix while staying phase-aligned; Delta and Bypass are applied
// once here (the sub-engines' own Delta/Bypass stay false). Bypass crossfades to
// dryPure (the raw delayed input) over the same 10 ms ramp the single engine
// uses, and full bypass passes dryPure through bit-transparently.
//
// Quality switching: setQuality is forwarded to both sub-engines (each latches
// and swaps at its next frame boundary, holding its own latency-aligned dry while
// its OLA ring refills). The high-band pre-delay is recomputed per sample from
// the LIVE sub-engine latencies (D = N_L(now) - N_S(now)), so the high path's
// total delay equals the low engine's current N_L at EVERY sample, whichever
// engine happens to switch first — once both have switched the paths sit at the
// new N_L, and during each engine's refill hold it emits its own aligned dry, so
// the summed output stays on the aligned composite dry (≈ dryRef) through the
// switch. No extra composite-level mask is applied: the only transient left is
// the sub-hop retiming step in the high pre-delay when D changes (a < N_L window,
// finite by construction, and the host PDC change already covers the moment).
//
// Display grid: numBins()/binToHz() report the LOW engine's grid (the finest,
// full-band grid). magnitudeDb()/reductionDb() merge the two engines onto that
// grid — bins below kSplitHz read the low engine directly, bins at/above it are
// resampled from the high engine by linear interpolation in dB along log-f. GUI
// reads race benignly against the audio thread, exactly like the single engine's
// meters.
//
// Header-only, JUCE-independent, allocation-free in process(): every buffer is
// sized in prepare() at the maximum order.
//
#include "DelayLine.h"
#include "LinkwitzRiley.h"
#include "ResonanceSuppressor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace factory_core
{
    class MultiResSuppressor
    {
    public:
        // order is the LOW-band Normal FFT order (callers pass an
        // fftOrderForSampleRate-derived value, same as the single engine). The
        // high band runs two orders shorter (order-2), so N_S = N_L/4 at every
        // Quality. maxOrder caps the low band's High-quality order (0 -> order+1);
        // the high band's cap is maxOrder-2. Rings/delays are sized at the low
        // band's maximum order so a Quality switch never allocates.
        void prepare (double sampleRate, int order = 11, int maxOrderIn = 0)
        {
            fs = sampleRate;

            int lowMax = (maxOrderIn > 0) ? maxOrderIn : (order + 1);
            lowMax = std::clamp (lowMax, order, kMaxAllowedOrder);
            const int highOrd = std::max (1, order - kOrderDrop);
            const int highMax = std::max (highOrd, lowMax - kOrderDrop);

            lowEng.prepare  (fs, order,   lowMax);
            highEng.prepare (fs, highOrd, highMax);

            for (int ch = 0; ch < 2; ++ch)
                splitLR[(size_t) ch].setCutoff (kSplitHz, fs);

            // Rings sized to hold up to the maximum low-band latency (the largest
            // pre-delay D = N_L - N_S and the largest dry delay N_L both fit).
            const int ringSize = (1 << lowMax) + 16;
            for (int ch = 0; ch < 2; ++ch)
            {
                dryPure[(size_t) ch].prepare   (ringSize);
                dryRef[(size_t) ch].prepare    (ringSize);
                highDelay[(size_t) ch].prepare (ringSize);
            }

            // Display-merge scratch on the HIGH engine's grid, sized for its
            // maximum (High-quality) bin count (GUI thread only).
            mergeHi.assign ((size_t) ((1 << highMax) / 2 + 1), 0.0);

            bypassStep = 1.0 / (kBypassRampSec * fs);
            reset();
        }

        void reset() noexcept
        {
            lowEng.reset();
            highEng.reset();
            for (int ch = 0; ch < 2; ++ch)
            {
                splitLR[(size_t) ch].reset();
                dryPure[(size_t) ch].reset();
                dryRef[(size_t) ch].reset();
                highDelay[(size_t) ch].reset();
            }
            bypassMix = bypassTarget; // snap the crossfade (transparent right after prepare/reset)
        }

        // --- parameters forwarded to BOTH sub-engines (per-band, summed) --------
        void setDepth          (double d)   noexcept { lowEng.setDepth (d);            highEng.setDepth (d); }
        void setSharpness      (double oct) noexcept { lowEng.setSharpness (oct);      highEng.setSharpness (oct); }
        void setSelectivity    (double s)   noexcept { lowEng.setSelectivity (s);      highEng.setSelectivity (s); }
        void setSmoothingWidth (double oct) noexcept { lowEng.setSmoothingWidth (oct); highEng.setSmoothingWidth (oct); }
        void setTilt           (double t)   noexcept { lowEng.setTilt (t);             highEng.setTilt (t); }
        void setMode           (int m)      noexcept { lowEng.setMode (m);             highEng.setMode (m); }
        void setStereoLink     (bool b)     noexcept { lowEng.setStereoLink (b);       highEng.setStereoLink (b); }
        void setRange (double lowHz, double highHz) noexcept
        {
            lowEng.setRange (lowHz, highHz);
            highEng.setRange (lowHz, highHz);
        }
        void setTimes (double attackMs, double releaseMs) noexcept
        {
            lowEng.setTimes (attackMs, releaseMs);
            highEng.setTimes (attackMs, releaseMs);
        }
        // Mix rides each band's spectral gain (gEff = 1 + Mix*(g-1)), so each band
        // blends against its OWN dry reconstruction and the two band sums add up
        // to the correct global Mix while staying phase-aligned.
        void setMix (double m) noexcept { lowEng.setMix (m); highEng.setMix (m); }

        // --- composite-level controls (sub-engine Delta/Bypass stay false) ------
        void setDelta    (bool b) noexcept { delta = b; }
        void setBypassed (bool b) noexcept { bypassTarget = b ? 1.0 : 0.0; }
        // Forwarded to both sub-engines; each swaps at its next frame boundary and
        // holds its aligned dry while its OLA ring refills. The high-band pre-delay
        // tracks the live latencies per sample, so the paths re-align automatically.
        void setQuality (int q) noexcept { lowEng.setQuality (q); highEng.setQuality (q); }

        // Per-band reduction profiles, each on its own engine's grid.
        void setProfile (const double* low, int nLow, const double* high, int nHigh) noexcept
        {
            lowEng.setProfile (low, nLow);
            highEng.setProfile (high, nHigh);
        }

        // --- reporting: the display grid is the LOW engine's -------------------
        int    latencySamples() const noexcept { return lowEng.latencySamples(); }
        int    numBins()        const noexcept { return lowEng.numBins(); }
        double binToHz (int k)  const noexcept { return lowEng.binToHz (k); }
        double splitHz()        const noexcept { return kSplitHz; }

        const ResonanceSuppressor& lowEngine()  const noexcept { return lowEng; }
        const ResonanceSuppressor& highEngine() const noexcept { return highEng; }

        // Merge the two engines' magnitude spectra onto the low (display) grid:
        // bins below kSplitHz come from the low engine verbatim, bins at/above it
        // are resampled from the high engine (linear in dB along log-frequency).
        // `scratch` is caller-owned, sized numBins(); an internal buffer holds the
        // high engine's grid (GUI thread only, benign race like the meters).
        const double* magnitudeDb (double* scratch) const noexcept
        {
            lowEng.magnitudeDb (scratch);          // low grid, in dB
            highEng.magnitudeDb (mergeHi.data());  // high grid, in dB
            mergeHighInto (scratch, mergeHi.data());
            return scratch;
        }

        // Same merge for the per-bin reduction curve. Both sub-engines expose
        // their reduction buffers directly, so no internal high scratch is needed.
        const double* reductionDb (double* scratch) const noexcept
        {
            const double* rLow  = lowEng.reductionDb();
            const double* rHigh = highEng.reductionDb();
            const int nb = lowEng.numBins();
            for (int k = 0; k < nb; ++k) scratch[k] = rLow[k];
            mergeHighInto (scratch, rHigh);
            return scratch;
        }

        // Process one stereo sample in place. Output is latency-aligned (N_L).
        void process (double& l, double& r) noexcept
        {
            const int NL = lowEng.latencySamples();
            const int NS = highEng.latencySamples();
            const int D  = NL - NS; // high-band pre-delay so both paths total N_L

            // dryPure: the raw input delayed by N_L — the bit-transparent bypass
            // source (integer delay -> the fractional read is exact).
            dryPure[0].write (l); dryPure[1].write (r);
            const double dryPureL = dryPure[0].readInterpolated ((double) NL);
            const double dryPureR = dryPure[1].readInterpolated ((double) NL);

            // LR4 split. low/high feed the two bands; their sum is allpass(in).
            double lowL, highL, lowR, highR;
            splitLR[0].process (l, lowL, highL);
            splitLR[1].process (r, lowR, highR);

            // dryRef: allpass(in) delayed by N_L — the Delta reference (equals the
            // depth-0 summed output up to the STFTs' reconstruction residual).
            dryRef[0].write (lowL + highL); dryRef[1].write (lowR + highR);
            const double dryRefL = dryRef[0].readInterpolated ((double) NL);
            const double dryRefR = dryRef[1].readInterpolated ((double) NL);

            // High band pre-delayed by D so its total latency matches N_L.
            highDelay[0].write (highL); highDelay[1].write (highR);
            const double hdL = highDelay[0].readInterpolated ((double) D);
            const double hdR = highDelay[1].readInterpolated ((double) D);

            // Sub-engines (stereo, in place). Their Delta/Bypass stay false.
            double loOutL = lowL, loOutR = lowR;
            lowEng.process (loOutL, loOutR);
            double hiOutL = hdL, hiOutR = hdR;
            highEng.process (hiOutL, hiOutR);

            const double sumL = loOutL + hiOutL;
            const double sumR = loOutR + hiOutR;

            // Delta at the composite level: dryRef - sum keeps only the removed part.
            double outL, outR;
            if (delta) { outL = dryRefL - sumL; outR = dryRefR - sumR; }
            else       { outL = sumL;           outR = sumR;           }

            // Latency-preserving bypass: ramp toward dryPure (bit-transparent at
            // the fully-bypassed end, untouched at the fully-active end).
            if (bypassMix < bypassTarget)      bypassMix = std::min (bypassTarget, bypassMix + bypassStep);
            else if (bypassMix > bypassTarget) bypassMix = std::max (bypassTarget, bypassMix - bypassStep);

            if (bypassMix >= 1.0)
            {
                l = dryPureL; r = dryPureR;
            }
            else if (bypassMix <= 0.0)
            {
                l = outL; r = outR;
            }
            else
            {
                l = outL + bypassMix * (dryPureL - outL);
                r = outR + bypassMix * (dryPureR - outR);
            }
        }

    private:
        static constexpr double kSplitHz         = 3000.0; // LR4 crossover (LISTENING CHECKPOINT: 1.5k vs 3k)
        static constexpr int    kOrderDrop       = 2;      // high band window 2 orders shorter (N_S = N_L/4)
        static constexpr int    kMaxAllowedOrder = 14;     // mirrors ResonanceSuppressor's High-quality cap
        static constexpr double kBypassRampSec   = 0.010;  // latency-preserving bypass crossfade ramp

        // Overwrite the at/above-kSplitHz bins of `dst` (on the low/display grid)
        // with values resampled from the high engine's `hi` buffer, interpolated
        // linearly (in dB) along log-frequency between the bracketing high bins.
        void mergeHighInto (double* dst, const double* hi) const noexcept
        {
            const int nbLow  = lowEng.numBins();
            const int nbHigh = highEng.numBins();
            if (nbHigh < 2) return;

            for (int k = 0; k < nbLow; ++k)
            {
                const double f = lowEng.binToHz (k);
                if (f < kSplitHz) continue;

                // Bracketing high bins j0 <= f/binHz < j0+1 (the high grid is
                // linear in frequency: f_high(j) = j * highEng.binToHz(1)). Clamp
                // to [1, last] so the log interpolation sees positive frequencies.
                const double jf = f / highEng.binToHz (1);
                int j0 = (int) std::floor (jf);
                j0 = std::clamp (j0, 1, nbHigh - 1);
                const int j1 = std::min (j0 + 1, nbHigh - 1);
                if (j1 == j0) { dst[k] = hi[j0]; continue; }

                const double f0 = highEng.binToHz (j0);
                const double f1 = highEng.binToHz (j1);
                const double t  = (std::log (f) - std::log (f0)) / (std::log (f1) - std::log (f0));
                dst[k] = hi[j0] + t * (hi[j1] - hi[j0]);
            }
        }

        double fs = 44100.0;

        ResonanceSuppressor lowEng, highEng;
        std::array<LinkwitzRiley, 2> splitLR;   // [ch] band split (its low+high = allpass(in))
        std::array<DelayLine, 2>     dryPure;   // [ch] raw input delayed N_L (bypass source)
        std::array<DelayLine, 2>     dryRef;    // [ch] allpass(in) delayed N_L (Delta reference)
        std::array<DelayLine, 2>     highDelay; // [ch] high band pre-delay (D = N_L - N_S)

        mutable std::vector<double> mergeHi;    // high-grid display scratch (GUI thread)

        bool   delta = false;
        double bypassMix = 0.0, bypassTarget = 0.0, bypassStep = 0.0;
    };
} // namespace factory_core
