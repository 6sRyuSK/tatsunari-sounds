//
// dsp_test.cpp — headless verification of the resonance suppressor DSP core
// (factory_core::FFT + ResonanceSuppressor). This is a non-linear / adaptive
// processor, so the gates are invariants rather than a single closed-form oracle:
//
//   1. FFT round-trips to identity and matches a direct DFT.
//   2. STFT perfect reconstruction: depth=0 => output == input delayed by the
//      reported latency N (proves windowing / overlap-add / latency).
//   3. Delta: wet (mix=1) + removed (delta) == dry.
//   4. Suppression + selectivity: a strong steady tone over noise is reduced,
//      while a resonance-free control band is left alone.
//   5. Reduction profile: zeroing the profile around a resonance disables
//      suppression there (the "EQ-like" curve scales suppression locally).
//   6. Stereo link: identical L==R input yields identical L==R output.
//   7. Resolution vs sample rate: the order chosen by
//      factory_core::fftOrderForSampleRate keeps the analyser bin width and the
//      analysis-window length within bounds at every supported rate (incl.
//      192 kHz), so the low end of the spectrum is never lost. This is the guard
//      against a fixed FFT order silently degrading at high sample rates.
//   8. Detection mode. Soft (adaptive threshold) is level-independent: the same
//      resonance is cut by the same dB at 0 dB and −12 dB. Hard (absolute
//      threshold set by Depth) is level-dependent: a resonance is cut hard when
//      loud and left alone when quiet. Hard stays selective (resonance cut, not
//      broadband) and numerically safe (finite / non-increasing) at worst-case
//      Depth. Delta and the silence floor are re-checked in both modes.
//   9. reset() (issue-class E): a heavily-driven engine, once reset(), behaves
//      identically to a freshly-constructed engine on the same subsequent
//      signal -- no stale per-bin gain / ring-buffer state survives reset().
//  10. Attack/release ballistics (setTimes) are exercised with contrasting
//      fast/slow settings, not just the defaults: release recovery and attack
//      engagement are asserted to order (fast quicker than slow, meaningful
//      margin) AND, quantitatively, against an independent one-pole
//      time-constant prediction derived from the standard EMA relation
//      c = exp(-frameTime/tau) (frameTime = hop/Fs) -- not from the code
//      under test.
//  11. setRange band-limiting: suppression occurs for an in-range resonance and
//      is fully absent (output == latency-aligned input to FFT-roundtrip
//      tolerance) for an out-of-range one, at every rate.
//  12. Depth/sharpness hard step mid-stream: output stays finite and bounded,
//      and the step does not introduce a slew far beyond the signal's own
//      baseline slew (documents current behaviour; a genuine zipper here would
//      be reported, not silently patched).
//  13. Latency-preserving bypass: a from-startup-bypassed engine is bit-transparent
//      to the latency-aligned dry (out[n] == in[n-N]), so toggling bypass never
//      shifts the plugin against other tracks (PDC intact). A twin (always-active
//      vs bypass-on-then-off) confirms the crossfade stays finite, is a convex
//      blend of the two ends throughout the ramp, sits on the aligned dry while
//      fully bypassed, and returns to a bit-exact match with the active twin.
//  14. Delta-Mix identity: Mix rides the spectral gain (gEff = 1 + mix*(g-1)), so
//      the delta output is the removed signal literally, delta(m) = dry - out(m).
//      Twin normal/delta engines over mix in {0, 0.3, 0.8, 1} satisfy three gates:
//      (i) complementarity -- out_normal + out_delta reconstructs the latency-
//      aligned dry within the 1e-12 relative spec tolerance (one IEEE rounding,
//      measured <= 1 ULP); (ii) affine identity -- delta(m) == m*delta(1) +
//      (1-m)*delta(0) to 1e-12 relative (exact in real arithmetic since gEff is
//      affine in Mix and the STFT/OLA is linear; the 1e-12 absorbs FFT rounding),
//      the structural replacement for the old bit-exact scaling gate; and
//      (iii) reconstruction residual -- delta(0) = dry - dry_recon is the STFT OLA
//      residual (peak <= 1e-9 in steady state), so a Mix-ignoring full-range delta
//      leaves the whole removed signal here and fails hard.
//  15. Envelope notch (Pass A / A1): two equal tones 1/3 octave apart over a
//      noise floor -- the case where the old linear-amplitude envelope (no
//      self-excluding notch) let each peak lift the other's baseline and mask
//      the pair. Both peaks must reach the spec reduction, stay within a spec
//      distance of each other, and match an independent spec-pipeline oracle
//      computed on the engine's own last analysis frame.
//  16. Selectivity contrast (Pass A / A2): the soft-knee contrast law
//      T(s) = 1+5s dB / W(s) = 6-4s dB, on a deterministic bin-aligned comb
//      floor + tone. A hot peak (linear knee branch) must shift its reduction
//      by exactly depth*2.5 dB (s 0->0.5) and depth*5 dB (s 0->1); a weak
//      (~4 dB excess) peak's reduction must decrease monotonically with s and
//      vanish at s = 1. Engine matches the spec oracle at every s, with and
//      without the frequency smoothing.
//  17. Frequency-domain gain smoothing (Pass A / A4): on a steady single sine
//      the converged reductionDb() curve equals the spec oracle (log envelope
//      + notch -> soft-knee -> two cascaded variable-width box passes) on every
//      bin; setSmoothingWidth(0) bypasses it (matches the unsmoothed oracle);
//      with smoothing on, the adjacent-bin |delta redDb| stays under the spec
//      cap (vs the raw -48 dB single-bin cliff without smoothing).
//  18. Dual reconstruction (Pass 2A, factory_core::MultiResSuppressor): at
//      depth 0 / mix 1 the summed dual-band output equals a TEST-SIDE LR4
//      unity twin (same class / 3 kHz cutoff / rate; split->sum = allpass)
//      delayed by N_L(q), at every Quality, within the 1e-9 absolute spec
//      tolerance (a new spec value absorbing the two band STFTs'
//      reconstruction error + FMA/rounding differences; measured <= 4.5e-16).
//  18b. Phase 6 splitHzIn default-identity: prepare()'s new additive 4th
//       argument (splitHzIn, default 0.0 -> the kSplitHz constant) is BIT-exact
//       vs. the legacy 3-arg prepare() when passed the same 3000.0 Hz explicitly
//       (proving the audition-only override hook changes nothing for any
//       existing/default caller), and a genuinely different value (1500 Hz, the
//       audition pack's other checkpoint) is reflected by splitHz() and
//       measurably changes the output (non-vacuity).
//  19. Dual Delta/Mix: (a) depth-0 delta output is the two-band reconstruction
//      residual, peak <= 1e-9 (measured <= 6.7e-16); (b) complementarity --
//      out_normal + out_delta reconstructs the test-side LR4-twin dry to
//      1e-12 relative at mix in {0, 0.3, 0.8, 1} (measured 1.2e-16); (c)
//      affine identity delta(m) == m*delta(1) + (1-m)*delta(0) to 1e-12
//      relative (measured <= 4.5e-16) -- the same three gates as the single
//      engine's deltaMixIdentityTest, re-derived against the composite's
//      allpass dry reference.
//  20. Dual bypass: a from-startup-bypassed composite is bit-transparent to
//      the RAW input delayed by N_L (out[n] == in[n-N_L], exact -- the bypass
//      source is the plain delayed input, not the allpass reference), and a
//      toggle twin stays finite, convex-bounded through the ramp, sits on the
//      aligned dry while bypassed and returns to a bit-exact match.
//  21. Dual speed -- the point of the split: a tone burst scaled to each
//      path's own window (B = N_sub/4) with near-instant ballistics (the
//      per-bin gain IS each frame's target) is suppressed over a span of
//      ~ N_sub + B samples; the span is invariant to the high path's constant
//      pre-delay, so span_high/span_low directly measures the high band's 4x
//      frame speed. Oracle: span ~ N_sub + B - (threshold-edge frames)*H_sub,
//      ratio ~ (N_S+B_S)/(N_L+B_L) = 1/4 with B_sub proportional to N_sub.
//      Measured (deterministic, no rng): span_high = 1.125*N_S, span_low =
//      0.875*N_L, ratio 0.321 at all six rates; gates fixed with margin
//      (per-path span windows, span_high <= span_low/2, ratio in [0.18, 0.42]).
//  22. Dual crossover continuity: equal resonant tones just below/above the
//      3 kHz split (2.5k / 3.6k) are both suppressed >= 15 dB (measured
//      -19.6..-24.5 dB) and within 4 dB of each other (measured <= 1.9 dB) --
//      the crossover-edge detection bias of the band-limited envelopes is a
//      known simplification, bounded here rather than modelled.
//  23. Dual quality: latency table N_L(q) = 1<<clamp(O+q-1, 1, O+1) with
//      N_S(q) = N_L(q)/4 at every q; a mid-stream Normal->Fast->High switch
//      stays finite (peak-bounded) and returns to the depth-0 twin
//      reconstruction <= 1e-9 (measured 4.5e-16) once BOTH sub-engines'
//      switches settle. The two engines swap at their own frame boundaries;
//      in between, the per-sample pre-delay D = N_L(now) - N_S(now) keeps the
//      total latency pinned to the live low-band N_L, so no composite-level
//      mask is needed -- the settle window in the test counts from the last
//      request/sub-engine latency change.
//  24. Dual display merge: the composite magnitudeDb()/reductionDb() on the
//      low (display) grid equal the low engine verbatim below the split and
//      an INDEPENDENTLY re-implemented log-f linear-in-dB resample of the
//      high engine at/above it (<= 1e-9 dB), non-vacuously (the high engine
//      contributes a real cut at 8 kHz and the merged curve genuinely differs
//      from the low engine's own bins above the split).
//
// Pass A re-derivation note (engine detection rework: log-domain envelope with
// self-excluding notch, selectivity soft-knee, finite-slope Hard mode, per-bin
// reduction smoothing). Expected values re-derived with the new spec oracle:
//   - hardLevelDependenceTest: operating point depth 0.28 -> 0.6 (threshold
//     -16 dBFS -> -27.6 dBFS) and quiet scale 0.1 -> 0.06 so the two levels
//     still straddle the threshold; gates re-derived (loud <= -4.5 dB,
//     quiet >= -1.0 dB, gap >= 4.5 dB; measured loud -5.74..-7.11 dB and
//     quiet ~0.00 dB across the six rates). The old loud <= -4.0 at depth 0.28
//     assumed the removed infinite-ratio Hard cut.
//   - hardSuppressionTest: selectivity gap 15 dB -> 6.5 dB (measured
//     8.10..10.94 dB across rates). The finite Hard slope (0.85 ~ 6.7:1) plus
//     the A4 smoothing dilute a single-bin tone cut by design; the raw
//     detector's selectivity is gated smoothing-off in selectivityContrastTest.
//     f0 <= -6 dB and control >= -4.5 dB gates unchanged (still pass).
//   - All other existing gates (suppressionTest, softLevelInvarianceTest,
//     hardStabilityTest, silenceTest, rangeGatingTest,
//     depthSharpnessMidStreamTest, reconstruction/delta/bypass/reset/ballistics)
//     hold unchanged under the new engine at all six rates -- measured values
//     shift (e.g. suppression f0 -26.0 -> -22.7 dB at 48 kHz) but stay well
//     inside the existing gates, so no tolerance was touched.
//
// Pass B-1 re-derivation note (Mix/Delta output-stage rework: Mix moved from a
// time-domain dry/wet blend to the spectral gain, gEff = 1 + mix*(g-1) applied in
// processFrame; delta = dry - out). Only deltaMixIdentityTest is restructured. The
// old bit-exact scaling gate (delta(mix) == fl(mix*delta(1)), tolerance 0) no
// longer holds by construction: with Mix on the spectral gain, delta(m) picks up
// the (1-m)*delta(0) reconstruction-residual term, so it is m*delta(1) +
// (1-m)*delta(0) rather than m*delta(1). It is replaced by an affine-identity gate
// (same 1e-12 relative spec) plus a delta(0) reconstruction-residual gate (1e-9),
// which together catch the same regression class (a Mix-non-linked delta). At the
// default mix=1 (used by every other test) the spectral processing is bit-identical
// to the pre-rework engine (the mix>=1 branch applies g verbatim), so no other gate
// moves. bypassAlignment/bypassToggle/deltaTest/reconstruction hold unchanged.
//
// Pass B-2 re-derivation note (Tilt + 8x overlap + Quality). The default Normal
// quality moved from 4x to 8x overlap, so the STFT frame rate DOUBLED (hop N/4 ->
// N/8, frameRate fs/H doubled). The physical meaning of the ballistics gates (a
// time constant in ms) is unchanged; only the FRAME COUNT changes, because a frame
// is now half as long. The release/attack ballistics tests measure at the true
// engine frame stride (H = N/8) and predict with frameTime = H/Fs = 1/frameRate,
// so the one-pole check is self-consistent at the new rate. Correspondence at
// 48 kHz / order 11 (measure stride, gate = fast-vs-slow ordering + one-pole
// trajectory, tolerances UNCHANGED):
//   releaseBallisticsTest: measured per-frame recovery hops roughly double with the
//     frame rate. hopsFast 3 -> 6 (fast release 20 ms); hopsSlow stays capped at
//     measureHops (40) (slow release 400 ms doesn't recover within the window at
//     either rate). predErr stays ~0 (silence target is exactly 1.0). Ordering and
//     tolerance gates UNCHANGED.
//   attackBallisticsTest:  hopsSlow ~14 -> 29 (slow attack 300 ms); hopsFast ~0
//     (fast attack 5 ms converges within the window fill at either rate). predErr
//     unchanged (~0.008). Ordering and tolerance gates UNCHANGED.
//   depthSharpnessMidStreamTest: no frame-count expectation (a slew-bound sanity
//     gate keyed off N, not H); it holds unchanged at 8x.
// Two new tests: tiltBallisticsTest (per-bin time constants match the one-pole
// formula onePoleCoeffForMs(baseMs*s(f), frameRate) and the tilt direction: at +1
// the 8 kHz bin reacts faster, the 100 Hz bin slower) and qualityTest (per-quality
// latency table, depth=0 reconstruction, bypass bit-transparency, and a mid-stream
// Normal->Fast->High switch: finite, forced-dry during the refill hold, and depth=0
// reconstruction after each switch settles). The Pass A snapshot tests
// (envelopeNotch/selectivityContrast/gainSmoothing) keep M = 2^15 a multiple of the
// new hop N/8 at every order (11..13 -> 256/512/1024), so their frame alignment and
// steady-state values are unchanged.
//
// Pass 2A note (dual-resolution composite). Tests 18-24 gate the new
// factory_core::MultiResSuppressor (LR4 split at 3 kHz, low band at order O,
// high band at order O-2 / 4x frame rate, high path pre-delayed to N_L total).
// All existing single-engine tests are UNCHANGED -- the composite is additive.
// Its dry references are test-side twins (LinkwitzRiley is verified allpass-on-
// sum by construction: identical class/coefficients as the implementation's
// split, fed the same input, plus plain integer delays). Measured residuals
// that fixed the spec values, across all six rates: depth-0 reconstruction vs
// twin <= 4.5e-16 (spec 1e-9); delta(0) peak <= 6.7e-16 (spec 1e-9);
// complementarity <= 1.2e-16 rel and affine <= 4.5e-16 rel (spec 1e-12 each);
// burst spans 1.125*N_S / 0.875*N_L -> ratio 0.321 (spec window [0.18, 0.42]);
// crossover suppression -19.6..-24.5 dB, diff <= 1.83 dB (spec >= 15 dB /
// <= 4 dB); mid-switch settled residual <= 4.5e-16 (spec 1e-9).
//
// Phase 4 note (8-band reduction/depth-EQ + per-band width). ReductionNodes
// grew from 4 to 8 bands and gained a per-band widthOct (factory_core::
// ReductionProfile.h); three new rate-independent tests gate it directly
// (reductionProfileDbAt has no sample-rate dependence, so these run once, like
// reductionProfileTest, not per rate):
//   25. Default-identity: the 8-band profile at the factory defaults (b0..b3 as
//       before, b4..b7 off, every band's widthOct at the new default 0.50)
//       matches an INDEPENDENT v1 oracle (the pre-Phase-4 fixed-width formulas,
//       hard-coded in the test, never calling factory_core::detail::*) at every
//       swept frequency -- proving the width parameter is a no-op at its
//       default and the 8-band curve is identical to the old 4-band one.
//   26. Width sweep: for a single Bell band, the frequency where the profile
//       reaches HALF the peak's dB value scales with widthOct as an
//       independent closed-form oracle derived from the Bell spec predicts
//       (half-width in octaves proportional to widthOct), at widthOct in
//       {0.25, 0.50, 1.0, 2.0}.
//   27. 8-band superposition: with all 8 bands on at different freq/type/sens/
//       width, the combined profile equals the sum (in dB) of each band's own
//       isolated contribution -- reductionProfileDbAt is additive by
//       construction, so this is an exact-arithmetic gate.
//
// Phase 5a-1 (pre-spectrum + Listen, engine level; the plugin's own Listen
// wiring is JUCE-dependent and not headless-testable):
//   28. Pre-spectrum: magnitudePreDb() (the input spectrum, captured before
//       the per-bin gain) matches an independent Hann-FFT oracle on the
//       engine's own last analysis frame at a suppressed resonance's bin
//       (<= 1e-6 dB), while magnitudeDb() (post) sits >= 3 dB lower there; at
//       an unrelated control bin (no resonance) pre and post coincide
//       (<= 0.5 dB), since nothing was suppressed.
//   29. Listen profile: the engine-level equivalent of soloing one reduction
//       node -- a single-node profile (one Bell band's local boost, baseline
//       1.0 elsewhere, the shape the plugin's node-copy would rasterise) +
//       delta=true + mix=1.0. (a) With depth > 0 the delta output carries
//       genuine removed energy in the node's band. (b) With depth == 0 (the
//       SAME solo profile, unchanged) delta collapses to the STFT OLA
//       reconstruction residual (<= 1e-6): profile only SCALES an
//       already-nonzero post-softknee reduction (computeGains), it can never
//       manufacture suppression from nothing, so this is a non-vacuous check
//       that (a)'s effect is genuinely depth-driven through the profile shape.
//
//  30. Display smoothing (Phase 6, setDisplaySmoothingMs): a DEV/audition-only
//      one-pole temporal low-pass on the analyser snapshot dispMag/dispMagPre
//      (magnitudeDb()/magnitudePreDb()), forwarded from MultiResSuppressor to
//      both sub-engines. Twin composites (A: 0 ms/off, B: 50 ms/on), identical
//      config and input (a silence -> airband-tone step, held): (i) A and B's
//      output audio is bit-identical at EVERY sample -- the smoothing state
//      never leaks into the suppression/detection DSP or the OLA ring; (ii) a
//      few frames after the analysis window has fully cleared the onset, A's
//      unsmoothed reading has already settled to the frame's raw content while
//      B's one-pole has barely risen (read off highEngine() directly, the same
//      merge-free accessor dualSpeedTest/dualDisplayTest use, so the gate is
//      not diluted by the low/high display-grid interpolation) -- proving the
//      feature is not a silent no-op; (iii) after ~10 of B's own time
//      constants (dt = H/fs, so wall-clock tau holds regardless of hop) past
//      that same point, B relaxes onto A within a small dB tolerance.
//
// Every test runs across the full standard sample-rate matrix
// (44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz) and prepares the engine at the same
// order the plugin would pick (factory_core::fftOrderForSampleRate), so the
// gates exercise the real high-rate path.
//
#include "factory_core/FFT.h"
#include "factory_core/LinkwitzRiley.h"
#include "factory_core/MultiResSuppressor.h"
#include "factory_core/ReductionProfile.h"
#include "factory_core/ResonanceSuppressor.h"
#include "factory_core/StftResolution.h"
#include "factory_core/testing/DspInvariants.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    // The STFT order the plugin would use at this sample rate (mirrors
    // ResonanceSuppressorAudioProcessor). Tests prepare the engine with this so
    // they exercise the real high-rate path, not a fixed order.
    int orderFor (double Fs) { return factory_core::fftOrderForSampleRate (Fs, 11, 48000.0, 13); }

    // Magnitude of a real signal at frequency f (single-bin DFT over [a,b)).
    double magAt (const std::vector<double>& x, int a, int b, double f, double Fs)
    {
        cd acc {};
        const double w = 2.0 * kPi * f / Fs;
        for (int n = a; n < b; ++n) acc += x[(size_t) n] * std::exp (cd (0.0, -w * (n - a)));
        return std::abs (acc) / (b - a);
    }

    void fftTest()
    {
        std::printf ("FFT\n");
        std::mt19937 rng (1);
        std::uniform_real_distribution<double> u (-1.0, 1.0);

        for (int order : { 6, 8, 10 })
        {
            factory_core::FFT f; f.prepare (order);
            const int n = f.size();
            std::vector<cd> a ((size_t) n), b;
            for (auto& v : a) v = cd (u (rng), u (rng));
            b = a;
            f.forward (b.data());
            f.inverse (b.data());
            double e = 0.0;
            for (int i = 0; i < n; ++i) e = std::max (e, std::abs (b[(size_t) i] - a[(size_t) i]));
            if (e > 1.0e-9) fail ("FFT round-trip order " + std::to_string (order) + " err " + std::to_string (e));
        }

        // Forward vs direct DFT.
        factory_core::FFT f; f.prepare (6);
        const int n = f.size();
        std::vector<cd> a ((size_t) n), A;
        for (auto& v : a) v = cd (u (rng), u (rng));
        A = a; f.forward (A.data());
        double e = 0.0;
        for (int k = 0; k < n; ++k)
        {
            cd s {};
            for (int m = 0; m < n; ++m) s += a[(size_t) m] * std::exp (cd (0.0, -2.0 * kPi * k * m / n));
            e = std::max (e, std::abs (A[(size_t) k] - s));
        }
        if (e > 1.0e-9) fail ("FFT vs DFT err " + std::to_string (e));
        std::printf ("  ok\n");
    }

    // Render a stereo signal through a configured suppressor; return mono output.
    template <typename Cfg>
    std::vector<double> render (double Fs, const std::vector<double>& xl, const std::vector<double>& xr, Cfg cfg)
    {
        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, orderFor (Fs));
        cfg (s);
        std::vector<double> out (xl.size());
        for (size_t n = 0; n < xl.size(); ++n)
        {
            double l = xl[n], r = xr[n];
            s.process (l, r);
            out[n] = 0.5 * (l + r);
        }
        return out;
    }

    void reconstructionTest (double Fs)
    {
        std::printf ("STFT reconstruction + latency @ Fs=%.0f\n", Fs);
        const int N = 1 << orderFor (Fs); // window = latency at the rate's order
        const int M = std::max (1 << 14, 4 * N);
        std::mt19937 rng (7);
        std::uniform_real_distribution<double> u (-0.5, 0.5);
        std::vector<double> x ((size_t) M);
        for (auto& v : x) v = u (rng);

        const auto y = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (0.0); s.setMix (1.0);
        });

        factory_core::ResonanceSuppressor probe; probe.prepare (Fs, orderFor (Fs));
        if (probe.latencySamples() != N) fail ("latency != N");

        double e = 0.0;
        for (int n = 2 * N; n < M; ++n) e = std::max (e, std::abs (y[(size_t) n] - x[(size_t) (n - N)]));
        if (e > 1.0e-6) fail ("reconstruction (depth=0) err " + std::to_string (e));
        std::printf ("  maxErr=%.2e\n", e);
    }

    void deltaTest (double Fs, int mode)
    {
        std::printf ("Delta (wet + removed == dry) mode=%d @ Fs=%.0f\n", mode, Fs);
        const int N = 1 << orderFor (Fs);
        const int M = std::max (1 << 14, 4 * N);
        std::mt19937 rng (11);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 1500.0 * n / Fs);

        const auto wet = render (Fs, x, x, [mode] (factory_core::ResonanceSuppressor& s) {
            s.setMode (mode); s.setDepth (1.0); s.setMix (1.0);
        });
        const auto rem = render (Fs, x, x, [mode] (factory_core::ResonanceSuppressor& s) {
            s.setMode (mode); s.setDepth (1.0); s.setDelta (true);
        });
        double e = 0.0;
        for (int n = 2 * N; n < M; ++n) e = std::max (e, std::abs (wet[(size_t) n] + rem[(size_t) n] - x[(size_t) (n - N)]));
        if (e > 1.0e-6) fail ("wet+removed != dry err " + std::to_string (e));
        std::printf ("  maxErr=%.2e\n", e);
    }

    // Bypass must PRESERVE the reported latency (PDC): a from-startup-bypassed
    // engine is bit-transparent to the *latency-aligned* dry, out[n] == in[n-N],
    // never the un-delayed input. So toggling bypass never slides the plugin
    // against other tracks. The engine is driven fully active underneath (depth /
    // mix set) to prove the output stage — not a shortcut — carries the dry
    // through untouched. The first N samples are the ring's initial zeros. Strict
    // equality: a full bypass reads straight from the input ring (no arithmetic).
    void bypassAlignmentTest (double Fs)
    {
        std::printf ("Bypass alignment (out[n]==in[n-N], exact) @ Fs=%.0f\n", Fs);
        const int N = 1 << orderFor (Fs);
        const int M = std::max (1 << 14, 4 * N);
        std::mt19937 rng (29);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.4 * std::sin (2.0 * kPi * 1200.0 * n / Fs);

        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, orderFor (Fs));
        s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.7); // engine fully active underneath
        s.setBypassed (true);
        s.reset(); // snap the crossfade to the bypassed end, so it is transparent from n=0

        double e = 0.0;
        for (int n = 0; n < M; ++n)
        {
            double l = x[(size_t) n], r = x[(size_t) n];
            s.process (l, r);
            const double expected = (n >= N) ? x[(size_t) (n - N)] : 0.0;
            e = std::max (e, std::max (std::abs (l - expected), std::abs (r - expected)));
            if (l != expected || r != expected)
            { fail ("bypass not bit-transparent at n=" + std::to_string (n) + " Fs=" + std::to_string (Fs)); break; }
        }
        std::printf ("  maxErr=%.2e (expect 0)\n", e);
    }

    // Twin engines on identical input/config: A stays active, B is bypassed then
    // released. Bypass only gates the *output* stage, so A and B share internal
    // STFT state exactly. Therefore: (a) all finite; (b) once B's ramp reaches the
    // bypassed end, B == the latency-aligned dry until release (exact ring read);
    // (c) once the release ramp completes, B == A bit-exactly (identical arithmetic
    // on identical state); (d) throughout, B is a convex blend of A and the aligned
    // dry, so it never exceeds their pointwise max. `settle` is a generous margin
    // past the engine's 10 ms bypass ramp, so the strict windows sit clear of it.
    void bypassToggleTest (double Fs)
    {
        std::printf ("Bypass toggle (twin active vs bypass on/off) @ Fs=%.0f\n", Fs);
        const int N = 1 << orderFor (Fs);
        const double kRampSec = 0.010;                            // engine's kBypassRampSec (spec constant)
        const int settle = (int) std::ceil (kRampSec * Fs) + 16;  // generous: safely past the full ramp
        const int tOn  = 4 * N;
        const int tOff = tOn + settle + 4 * N;
        const int M    = tOff + settle + 4 * N + 256;

        std::mt19937 rng (31);
        std::normal_distribution<double> g (0.0, 0.25);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 2000.0 * n / Fs);

        auto cfg = [] (factory_core::ResonanceSuppressor& s) { s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.8); };
        factory_core::ResonanceSuppressor A, B;
        A.prepare (Fs, orderFor (Fs)); cfg (A); A.reset();
        B.prepare (Fs, orderFor (Fs)); cfg (B); B.reset();

        std::vector<double> a ((size_t) M), b ((size_t) M);
        bool boundBad = false;
        for (int n = 0; n < M; ++n)
        {
            if (n == tOn)  B.setBypassed (true);
            if (n == tOff) B.setBypassed (false);
            double la = x[(size_t) n], ra = x[(size_t) n];
            double lb = x[(size_t) n], rb = x[(size_t) n];
            A.process (la, ra);
            B.process (lb, rb);
            a[(size_t) n] = la; b[(size_t) n] = lb;

            // (d) convex-blend bound: B lies between the active output and the
            // aligned dry (holds every sample; +eps slack for the tie at the ends).
            const double dryAligned = (n >= N) ? x[(size_t) (n - N)] : 0.0;
            const double bound = std::max (std::abs (dryAligned), std::abs (la)) + 1.0e-12;
            if (! boundBad && (std::abs (lb) > bound || std::abs (rb) > bound))
            { fail ("bypass blend exceeded convex bound at n=" + std::to_string (n) + " Fs=" + std::to_string (Fs)); boundBad = true; }
        }

        // (a) finite.
        if (! factory_core::testing::allFinite (a) || ! factory_core::testing::allFinite (b))
            fail ("bypass toggle produced non-finite output at Fs=" + std::to_string (Fs));

        // (b) fully bypassed window: B == latency-aligned dry, exactly.
        for (int n = tOn + settle; n < tOff; ++n)
            if (b[(size_t) n] != x[(size_t) (n - N)])
            { fail ("bypassed B != aligned dry at n=" + std::to_string (n) + " Fs=" + std::to_string (Fs)); break; }

        // (c) after the release ramp: B == A, exactly.
        for (int n = tOff + settle; n < M; ++n)
            if (b[(size_t) n] != a[(size_t) n])
            { fail ("post-release B != A at n=" + std::to_string (n) + " Fs=" + std::to_string (Fs)); break; }

        std::printf ("  ok (N=%d settle=%d tOn=%d tOff=%d M=%d)\n", N, settle, tOn, tOff, M);
    }

    // Delta must monitor exactly what Mix removes. Mix now rides the spectral gain
    // (gEff = 1 + mix*(g-1), applied in processFrame), so the OLA ring holds the
    // Mix-scaled output and the delta output is that removed signal literally:
    // delta(m)[n] = dry[n] - out(m)[n]. Because gEff is affine in mix and the
    // STFT/OLA is linear, out(m) = (1-m)*out(0) + m*out(1); hence in exact
    // arithmetic delta(m) = m*delta(1) + (1-m)*delta(0), where delta(1) = dry -
    // full_wet (the removed signal at Mix=1) and delta(0) = dry - dry_recon (the
    // STFT reconstruction residual, ~0). Three gates on twin engines (identical
    // config/input, delta=false vs true), mix in {0, 0.3, 0.8, 1}, all six rates:
    //  (i) Complementarity: out_normal[n] + out_delta[n] reconstructs the latency-
    //      aligned dry (test-side N delay). delta = dry - out makes this one IEEE
    //      rounding from exact (2Sum non-identity); the 1e-12 relative spec
    //      tolerance absorbs that <= 1 ULP residual with margin, while an unscaled
    //      full-range delta leaves ~(1-mix)*|dry-wet| and fails hard.
    // (ii) Affine identity: |delta(m) - (m*delta(1) + (1-m)*delta(0))| <=
    //      1e-12*max(1,|dry|). This REPLACES the old bit-exact scaling gate
    //      (delta(mix) == fl(mix*delta(1)), tolerance 0): now that Mix rides the
    //      spectral gain, delta(m) carries the (1-m)*delta(0) reconstruction-
    //      residual term, so it equals m*delta(1) + (1-m)*delta(0), not m*delta(1)
    //      -- bit identity no longer holds by definition. The affine identity is
    //      exact in real arithmetic; the 1e-12 spec value absorbs the FFT rounding
    //      accumulation (~1e-15 relative). delta(1)/delta(0) are measured from
    //      dedicated twins. It catches the same class the old gate did (a Mix-non-
    //      linked delta): a delta that does not scale affinely with Mix fails here.
    // (iii) Reconstruction residual: peakAbs(delta(0)) over the steady state
    //      (n >= 2N, as reconstructionTest windows) <= 1e-9. delta(0) is the STFT
    //      OLA reconstruction residual; a Mix-ignoring full-range delta would leave
    //      the whole removed signal (~O(0.1)) here and fail hard -- this is the
    //      gate that pins the m=0 endpoint of the affine identity to "nothing
    //      removed", so (ii)+(iii) together forbid a delta that ignores Mix.
    void deltaMixIdentityTest (double Fs)
    {
        std::printf ("Delta-Mix identity (complementarity + affine + reconstruction) @ Fs=%.0f\n", Fs);
        const int N = 1 << orderFor (Fs);
        const int M = std::max (1 << 14, 4 * N);
        std::mt19937 rng (41);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 1500.0 * n / Fs);

        auto cfg = [] (factory_core::ResonanceSuppressor& s) { s.setDepth (1.2); s.setSharpness (0.5); };

        // Reference removed signals from dedicated delta twins: delta(1) = dry -
        // full_wet (the actual removed signal at Mix=1), delta(0) = dry - dry_recon
        // (the STFT reconstruction residual). Input is mono into both channels and
        // detection is stereo-linked, so L==R exactly; capture the L channel.
        auto renderDelta = [&] (double m)
        {
            factory_core::ResonanceSuppressor d;
            d.prepare (Fs, orderFor (Fs)); cfg (d); d.setDelta (true); d.setMix (m);
            std::vector<double> out ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                double l = x[(size_t) n], r = x[(size_t) n];
                d.process (l, r);
                out[(size_t) n] = l;
            }
            return out;
        };
        const auto delta1 = renderDelta (1.0);
        const auto delta0 = renderDelta (0.0);

        // Sanity: the gates below must act on a real removed signal, not silence.
        if (factory_core::testing::peakAbs (delta1) < 1.0e-3)
            fail ("delta reference carries no removed signal (vacuous gate) at Fs=" + std::to_string (Fs));

        // (iii) delta(0) is the OLA reconstruction residual in the steady state
        // (skip the first 2N of window fill-in, matching reconstructionTest).
        double reconResidual = 0.0;
        for (int n = 2 * N; n < M; ++n) reconResidual = std::max (reconResidual, std::abs (delta0[(size_t) n]));
        if (reconResidual > 1.0e-9)
            fail ("delta(0) reconstruction residual too large: " + std::to_string (reconResidual)
                  + " (spec 1e-9) at Fs=" + std::to_string (Fs));

        double maxRel = 0.0, maxAffine = 0.0;
        for (double m : { 0.0, 0.3, 0.8, 1.0 })
        {
            factory_core::ResonanceSuppressor nrm, del;
            nrm.prepare (Fs, orderFor (Fs)); cfg (nrm); nrm.setMix (m);
            del.prepare (Fs, orderFor (Fs)); cfg (del); del.setMix (m); del.setDelta (true);

            bool badSum = false, badAffine = false;
            for (int n = 0; n < M; ++n)
            {
                double ln = x[(size_t) n], rn = x[(size_t) n];
                double ld = x[(size_t) n], rd = x[(size_t) n];
                nrm.process (ln, rn);
                del.process (ld, rd);

                const double dryAligned = (n >= N) ? x[(size_t) (n - N)] : 0.0; // first N: ring zeros
                const double scale = std::max (1.0, std::abs (dryAligned));

                // (i) complementarity vs the test-side aligned dry.
                const double res = std::max (std::abs (ln + ld - dryAligned), std::abs (rn + rd - dryAligned));
                maxRel = std::max (maxRel, res / scale);
                if (! badSum && res > 1.0e-12 * scale)
                {
                    char resStr[32]; std::snprintf (resStr, sizeof (resStr), "%.2e", res);
                    fail ("normal+delta != aligned dry (res " + std::string (resStr) + ") at n=" + std::to_string (n)
                          + " mix=" + std::to_string (m) + " Fs=" + std::to_string (Fs));
                    badSum = true;
                }
                // (ii) affine identity: delta(m) == m*delta(1) + (1-m)*delta(0).
                const double affineRef = m * delta1[(size_t) n] + (1.0 - m) * delta0[(size_t) n];
                const double affRes = std::max (std::abs (ld - affineRef), std::abs (rd - affineRef));
                maxAffine = std::max (maxAffine, affRes / scale);
                if (! badAffine && affRes > 1.0e-12 * scale)
                {
                    char resStr[32]; std::snprintf (resStr, sizeof (resStr), "%.2e", affRes);
                    fail ("delta(mix) != m*delta(1)+(1-m)*delta(0) (res " + std::string (resStr) + ") at n="
                          + std::to_string (n) + " mix=" + std::to_string (m) + " Fs=" + std::to_string (Fs));
                    badAffine = true;
                }
            }
        }
        std::printf ("  maxRelResidual=%.2e affineRel=%.2e reconResidual=%.2e (spec 1e-12 / 1e-12 / 1e-9)\n",
                     maxRel, maxAffine, reconResidual);
    }

    void suppressionTest (double Fs)
    {
        std::printf ("Suppression + selectivity @ Fs=%.0f\n", Fs);
        const int M = 1 << 15;
        const double f0 = 2000.0;
        std::mt19937 rng (5);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        const auto wet = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.2); s.setSharpness (0.5);
        });

        const int a = M / 2, b = M; // steady-state window
        const double dryF0 = magAt (dry, a, b, f0, Fs), wetF0 = magAt (wet, a, b, f0, Fs);

        // Control selectivity: measure a *broadband* resonance-free region (4–10
        // kHz) rather than a single bin. A single-frequency probe is
        // noise-realisation sensitive — a random spectral peak there gets
        // legitimately reduced — which makes the gate flaky across sample rates
        // (bin alignment differs per rate). Averaging the energy over the band
        // reflects the suppressor's actual broadband behaviour and is stable.
        double dryCtrlE = 0.0, wetCtrlE = 0.0;
        for (double fc : { 4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9000.0, 10000.0 })
        {
            const double d = magAt (dry, a, b, fc, Fs), w = magAt (wet, a, b, fc, Fs);
            dryCtrlE += d * d; wetCtrlE += w * w;
        }
        const double f0Db   = 20.0 * std::log10 (wetF0 / dryF0);
        const double ctrlDb = 10.0 * std::log10 (wetCtrlE / (dryCtrlE + 1e-30));

        if (f0Db > -4.4)
            fail ("resonance at f0 not suppressed (>=4.4 dB): " + std::to_string (f0Db) + " dB");
        // The control band must be left broadly intact, and the resonance must be
        // cut far harder than the control band (the point of a resonance suppressor).
        if (ctrlDb < -4.5)
            fail ("control band over-attenuated (not selective): " + std::to_string (ctrlDb) + " dB");
        if (f0Db > ctrlDb - 15.0)
            fail ("not selective: f0 " + std::to_string (f0Db) + " dB vs control " + std::to_string (ctrlDb) + " dB");
        std::printf ("  f0 %.1f dB   control %.1f dB (broadband)\n", f0Db, ctrlDb);
    }

    void profileTest (double Fs)
    {
        std::printf ("Reduction profile (local scaling) @ Fs=%.0f\n", Fs);
        const int M = 1 << 15;
        const double f0 = 2000.0;
        std::mt19937 rng (9);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        // Profile zero around f0 -> no suppression there despite the resonance.
        const auto masked = render (Fs, x, x, [Fs, f0] (factory_core::ResonanceSuppressor& s) {
            s.setDepth (1.2);
            std::vector<double> prof ((size_t) s.numBins(), 1.0);
            const int N = 1 << factory_core::fftOrderForSampleRate (Fs, 11, 48000.0, 13);
            const int kf = (int) std::round (f0 * (double) N / Fs);
            for (int k = kf - 12; k <= kf + 12; ++k)
                if (k >= 0 && k < s.numBins()) prof[(size_t) k] = 0.0;
            s.setProfile (prof.data(), s.numBins());
        });

        const int a = M / 2, b = M;
        const double dryF0 = magAt (dry, a, b, f0, Fs), mF0 = magAt (masked, a, b, f0, Fs);
        if (mF0 < dryF0 * 0.85) fail ("profile=0 region still suppressed "
                                      + std::to_string (20.0 * std::log10 (mF0 / dryF0)) + " dB");
        std::printf ("  masked f0 %.2f dB (expect ~0)\n", 20.0 * std::log10 (mF0 / dryF0));
    }

    // ---- Phase 5a-1: pre-spectrum + Listen (engine-level) -------------------

    // Independent oracle for magnitudePreDb(): an independently instantiated
    // factory_core::FFT computes the Hann-windowed magnitude of the engine's own
    // last analysis frame (x[frameStart..frameStart+N-1]), normalised exactly
    // like the spec (max(|FFT|) / (0.5*N); here mono, so L==R==this value), in
    // dB -- mirrors the Pass A spec oracle's approach: it uses the verified FFT
    // primitive directly and never calls into ResonanceSuppressor's own
    // processFrame/dispMag machinery.
    std::vector<double> preSpectrumOracleDb (const std::vector<double>& x, int frameStart, int order)
    {
        const int N = 1 << order, half = N / 2;
        factory_core::FFT fft; fft.prepare (order);
        std::vector<cd> spec ((size_t) N);
        for (int k = 0; k < N; ++k)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * k / N);
            spec[(size_t) k] = cd (x[(size_t) (frameStart + k)] * w, 0.0);
        }
        fft.forward (spec.data());
        std::vector<double> db ((size_t) (half + 1));
        for (int k = 0; k <= half; ++k)
            db[(size_t) k] = 20.0 * std::log10 (std::abs (spec[(size_t) k]) / (0.5 * (double) N) + 1.0e-12);
        return db;
    }

    // Test (Phase 5a-1-A): magnitudePreDb() must equal the input spectrum
    // (the independent Hann-FFT oracle above) at a resonant bin, while
    // magnitudeDb() (post) sits meaningfully lower there (the resonance is
    // suppressed); at an unrelated control bin (no resonance) pre and post must
    // coincide (nothing was suppressed, so the input and output spectra agree).
    void preSpectrumTest (double Fs)
    {
        std::printf ("Pre-spectrum (magnitudePreDb == input; magnitudeDb == suppressed) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int M = 1 << 15; // multiple of the hop (N/8) at every supported rate -> exact last-frame alignment
        // Bin-aligned frequency near `target` (no scalloping loss in the dB reads below).
        auto binFreq = [&] (double target) {
            const int kf = std::max (1, (int) std::round (target * (double) N / Fs));
            return (double) kf * Fs / (double) N;
        };
        const double f0    = binFreq (3000.0);
        const double fCtrl = binFreq (9000.0); // unrelated control bin (no resonance)

        std::mt19937 rng (233);
        std::normal_distribution<double> g (0.0, 0.05);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, order);
        s.setDepth (1.2); s.setSharpness (0.5);
        for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = l; s.process (l, r); }

        const int nb = s.numBins();
        std::vector<double> preDb ((size_t) nb), postDb ((size_t) nb);
        s.magnitudePreDb (preDb.data());
        s.magnitudeDb (postDb.data());

        const auto oracle = preSpectrumOracleDb (x, M - N, order); // x[M-N..M-1] == the engine's own last frame
        const int k0   = (int) std::round (f0 * (double) N / Fs);
        const int kCtl = (int) std::round (fCtrl * (double) N / Fs);

        const double oracleErr = std::abs (preDb[(size_t) k0] - oracle[(size_t) k0]);
        if (oracleErr > 1.0e-6)
            fail ("pre-spectrum f0 bin != independent Hann-FFT oracle: " + std::to_string (preDb[(size_t) k0])
                  + " vs " + std::to_string (oracle[(size_t) k0]) + " dB (err " + std::to_string (oracleErr)
                  + ") at Fs=" + std::to_string (Fs));

        // Post (suppressed) sits meaningfully below pre at the resonance.
        const double cutDb = preDb[(size_t) k0] - postDb[(size_t) k0];
        if (cutDb < 3.0)
            fail ("post spectrum not suppressed relative to pre at f0: pre=" + std::to_string (preDb[(size_t) k0])
                  + " post=" + std::to_string (postDb[(size_t) k0]) + " dB at Fs=" + std::to_string (Fs));

        // Unrelated control bin: pre ~= post (no MEANINGFUL suppression there --
        // a quiet noise-floor bin can show a small natural fluctuation from the
        // detector's own randomness, same margin class as suppressionTest's
        // broadband control-band gate; measured <= 1.3 dB across all six rates).
        const double ctrlDiff = std::abs (preDb[(size_t) kCtl] - postDb[(size_t) kCtl]);
        if (ctrlDiff > 2.0)
            fail ("pre != post at unrelated control bin: pre=" + std::to_string (preDb[(size_t) kCtl])
                  + " post=" + std::to_string (postDb[(size_t) kCtl]) + " dB at Fs=" + std::to_string (Fs));

        std::printf ("  f0: pre=%.2f post=%.2f oracle=%.2f dB (cut %.2f dB)  ctrl: pre=%.2f post=%.2f (diff %.3f dB)\n",
                     preDb[(size_t) k0], postDb[(size_t) k0], oracle[(size_t) k0], cutDb,
                     preDb[(size_t) kCtl], postDb[(size_t) kCtl], ctrlDiff);
    }

    // Test (Phase 5a-1-B, engine-level equivalent of Listen -- the plugin's own
    // Listen wiring is JUCE-dependent and not headless-testable): a single-node
    // profile (one Bell band's local boost, baseline 1.0 elsewhere -- the exact
    // shape the plugin's node-copy, currentNodes() -> one node kept, rest off,
    // would rasterise) + delta=true + mix=1.0 must expose exactly what that one
    // node removes.
    //   (a) with the node's profile active and depth > 0, delta carries genuine
    //       removed energy in the node's band.
    //   (b) with depth == 0 (the SAME solo profile, unchanged), delta collapses
    //       to the STFT OLA reconstruction residual. ResonanceSuppressor gates
    //       ALL reduction on depth > 0.0 (computeGains: profile only SCALES an
    //       already-nonzero post-softknee value, it can never manufacture
    //       suppression from nothing), so this proves (a)'s effect is really
    //       driven by depth acting through the profile shape, not some vacuous
    //       artefact of the profile array itself -- a stronger non-vacuity
    //       check than merely disabling the node (which would still leave the
    //       engine's normal depth-driven broadband detection active).
    void listenProfileTest (double Fs)
    {
        std::printf ("Listen profile (single-node profile+delta+mix=1 == node removal) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int M = 1 << 15;
        // Bin-aligned frequency near 5 kHz (no scalloping loss in magAt below).
        const int kf0 = std::max (1, (int) std::round (5000.0 * (double) N / Fs));
        const double f0 = (double) kf0 * Fs / (double) N;

        std::mt19937 rng (211);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        // A single Bell band, on, at f0, sens large enough to hit the
        // setProfile ceiling (reductionProfileLinearAt clamps to 4.0) at its
        // centre, tapering back to the 1.0 baseline within about two octaves.
        factory_core::ReductionNodes solo;
        solo.bands[0] = { true, f0, factory_core::ReductionBandType::Bell, 24.0, 0.50 };

        auto profileFor = [&] (const factory_core::ReductionNodes& n, int nb)
        {
            std::vector<double> prof ((size_t) nb, 1.0);
            for (int k = 1; k < nb; ++k)
                prof[(size_t) k] = factory_core::reductionProfileLinearAt ((double) k * Fs / (double) N, n);
            return prof;
        };

        // (a) node on, depth > 0.
        factory_core::ResonanceSuppressor eOn;
        eOn.prepare (Fs, order);
        eOn.setDepth (1.0); eOn.setSharpness (0.5);
        { auto prof = profileFor (solo, eOn.numBins()); eOn.setProfile (prof.data(), eOn.numBins()); }
        eOn.setDelta (true); eOn.setMix (1.0);
        std::vector<double> deltaOn ((size_t) M);
        for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = l; eOn.process (l, r); deltaOn[(size_t) n] = l; }

        const int a = 2 * N, b = M;
        const double removedF0 = magAt (deltaOn, a, b, f0, Fs);
        if (removedF0 < 0.05)
            fail ("listen profile: no removed energy at the node's band: " + std::to_string (removedF0)
                  + " at Fs=" + std::to_string (Fs));

        // (b) depth == 0, SAME solo profile: delta must collapse to the
        // reconstruction residual (the same order reconstructionTest /
        // deltaMixIdentityTest's delta(0) case already gate at 1e-9; 1e-6 here
        // gives margin for the extra profile array in the loop).
        factory_core::ResonanceSuppressor eOff;
        eOff.prepare (Fs, order);
        eOff.setDepth (0.0); eOff.setSharpness (0.5);
        { auto prof = profileFor (solo, eOff.numBins()); eOff.setProfile (prof.data(), eOff.numBins()); }
        eOff.setDelta (true); eOff.setMix (1.0);
        double residual = 0.0;
        for (int n = 0; n < M; ++n)
        {
            double l = x[(size_t) n], r = l;
            eOff.process (l, r);
            if (n >= a) residual = std::max (residual, std::abs (l));
        }
        if (residual > 1.0e-6)
            fail ("listen profile: depth=0 delta did not collapse to reconstruction residual: "
                  + std::to_string (residual) + " at Fs=" + std::to_string (Fs));

        std::printf ("  removedF0=%.4f (depth=1, on)   residual=%.2e (depth=0, spec <=1e-6)\n", removedF0, residual);
    }

    void stereoLinkTest (double Fs)
    {
        std::printf ("Stereo link (L==R) @ Fs=%.0f\n", Fs);
        const int M = 1 << 14;
        std::mt19937 rng (3);
        std::normal_distribution<double> g (0.0, 0.2);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 3000.0 * n / Fs);

        factory_core::ResonanceSuppressor s; s.prepare (Fs, orderFor (Fs));
        s.setDepth (1.2); s.setStereoLink (true);
        double e = 0.0;
        for (int n = 0; n < M; ++n)
        {
            double l = x[(size_t) n], r = x[(size_t) n];
            s.process (l, r);
            e = std::max (e, std::abs (l - r));
        }
        if (e > 1.0e-9) fail ("linked L/R diverged err " + std::to_string (e));
        std::printf ("  maxLRdiff=%.2e\n", e);
    }

    // Regression guard for issue #24: a near-silent source (e.g. a synth's idle
    // output at ~-100 dBFS) that still has spectral peaks must NOT trigger any
    // gain reduction. A purely relative peak-vs-envelope detector would paint a
    // phantom reduction "curtain" on it; the absolute floor must keep the engine
    // idle so silence stays silent on the display and in the audio.
    void silenceTest (double Fs, int mode)
    {
        std::printf ("Silence floor (no reduction on near-silent input) mode=%d @ Fs=%.0f\n", mode, Fs);
        const int M = 1 << 15;
        const double amp = 1.0e-5; // ~ -100 dBFS peaks — inaudible
        std::mt19937 rng (13);
        std::normal_distribution<double> g (0.0, amp * 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = g (rng)
                          + amp * std::sin (2.0 * kPi * 400.0 * n / Fs)
                          + amp * std::sin (2.0 * kPi * 800.0 * n / Fs);

        factory_core::ResonanceSuppressor s; s.prepare (Fs, orderFor (Fs));
        s.setMode (mode); s.setDepth (1.5); s.setSharpness (0.5);
        for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = l; s.process (l, r); }

        const double* red = s.reductionDb();
        double worst = 0.0; // most negative reduction across all bins
        for (int k = 0; k < s.numBins(); ++k) worst = std::min (worst, red[(size_t) k]);
        if (worst < -0.5)
            fail ("phantom reduction on near-silent input: " + std::to_string (worst)
                  + " dB (mode " + std::to_string (mode) + ") at Fs=" + std::to_string (Fs));
        std::printf ("  worstReduction=%.3f dB (expect ~0)\n", worst);
    }

    // Regression guard for the 192 kHz analyser bug: a fixed FFT order makes the
    // bin width (fs/N) and window length (N/fs) drift with the sample rate. The
    // order chosen by fftOrderForSampleRate must keep both within bounds at every
    // rate, so the analyser always has a data point near 20 Hz and the
    // suppressor's detection window stays ~constant in time.
    void resolutionTest (double Fs)
    {
        std::printf ("Analyser resolution invariants @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);

        factory_core::ResonanceSuppressor s; s.prepare (Fs, order);
        const double binHz = s.binToHz (1);                 // lowest non-DC analyser bin
        const double winMs = 1000.0 * s.latencySamples() / Fs;

        // Bin 1 must reach the bottom of the audible band so a 20 Hz feature is
        // representable; otherwise the analyser's low end goes blank (the bug).
        if (binHz > 25.0)
            fail ("bin width too coarse: " + std::to_string (binHz) + " Hz at Fs=" + std::to_string (Fs));
        // Window length (and thus reduction behaviour) must stay ~constant in time.
        if (winMs < 30.0 || winMs > 60.0)
            fail ("window length out of range: " + std::to_string (winMs) + " ms at Fs=" + std::to_string (Fs));

        std::printf ("  order=%d binHz=%.2f winMs=%.1f\n", order, binHz, winMs);
    }

    // Bin-aligned tone frequency near `f0` so windowed magnitude has no scalloping
    // loss — the Hard-mode tests key off absolute dBFS, so the tone must land on a
    // bin for the level threshold to be exercised precisely.
    double alignedFreq (double Fs, double f0)
    {
        const int N = 1 << orderFor (Fs);
        const int kf = std::max (1, (int) std::round (f0 * (double) N / Fs));
        return (double) kf * Fs / (double) N;
    }

    // Measure the steady-state reduction (dB) of a resonance at `f0` for a given
    // mode/depth and input scale, via an independent single-bin DFT (magAt).
    double resonanceReductionDb (double Fs, double f0, int mode, double depth, double scale)
    {
        const int M = 1 << 15;
        std::mt19937 rng (5);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = scale * (g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs));

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        const auto wet = render (Fs, x, x, [mode, depth] (factory_core::ResonanceSuppressor& s) {
            s.setMode (mode); s.setDepth (depth); s.setSharpness (0.5);
        });
        const int a = M / 2, b = M;
        return 20.0 * std::log10 (magAt (wet, a, b, f0, Fs) / magAt (dry, a, b, f0, Fs));
    }

    // ---- Pass A spec oracle -------------------------------------------------
    // Independent implementation of the detection -> reduction spec (A1..A4),
    // written from the spec formulas -- it never calls into ResonanceSuppressor.
    // factory_core::FFT is used for the analysis only (independently verified by
    // fftTest above, like the rest of this file). The oracle evaluates ONE
    // analysis frame; the tests below feed the engine a signal whose length M is
    // a multiple of the hop H (so the engine's LAST frame is exactly
    // x[M-N .. M-1]) and use near-instant ballistics, making the engine's
    // reductionDb() equal to that frame's target (the ballistics themselves are
    // gated separately by release/attackBallisticsTest).

    struct SpecDetector
    {
        int    mode        = 0;          // 0 = Soft, 1 = Hard
        double depth       = 1.0;
        double selectivity = 0.5;        // engine default
        double sharpOct    = 0.5;        // engine default envelope half-width
        double smoothOct   = 1.0 / 12.0; // engine default reduction smoothing
    };

    // Spec A2 soft-knee excess: 0 below T-W/2, quadratic spline through the
    // knee, x-T above T+W/2.
    double specKneeOver (double x, double T, double W)
    {
        if (x - T <= -0.5 * W) return 0.0;
        if (x - T >=  0.5 * W) return x - T;
        const double t = x - T + 0.5 * W;
        return t * t / (2.0 * W);
    }

    // Per-bin reduction target (dB) of one Hann analysis frame
    // x[frameStart .. frameStart+N-1], per spec A1 (log-mean envelope with
    // self-excluding notch), A2/A3 (soft-knee contrast / finite Hard slope) and
    // A4 (two cascaded variable-width box passes on redDb).
    std::vector<double> specReductionOracle (const std::vector<double>& x, int frameStart,
                                             double Fs, int order, const SpecDetector& p)
    {
        const int N = 1 << order, half = N / 2;
        factory_core::FFT fft; fft.prepare (order);
        std::vector<cd> spec ((size_t) N);
        for (int k = 0; k < N; ++k)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * k / N); // Hann
            spec[(size_t) k] = cd (x[(size_t) (frameStart + k)] * w, 0.0);
        }
        fft.forward (spec.data());

        // A1: log magnitude, prefix sum, log-mean envelope with the central
        // notch [k/nf, k*nf] (nf = wf^(1/4)) excluded; whole-window fallback
        // when the window is tiny or the notch leaves fewer than 4 bins.
        std::vector<double> L ((size_t) (half + 1)), pfx ((size_t) (half + 2)), env ((size_t) (half + 1));
        pfx[0] = 0.0;
        for (int k = 0; k <= half; ++k)
        {
            L[(size_t) k] = std::log (std::abs (spec[(size_t) k]) + 1.0e-12);
            pfx[(size_t) (k + 1)] = pfx[(size_t) k] + L[(size_t) k];
        }
        const double wf = std::pow (2.0, p.sharpOct), nf = std::pow (wf, 0.25);
        for (int k = 0; k <= half; ++k)
        {
            const int lo  = std::clamp ((int) std::floor (k / wf), 0, half);
            const int hi  = std::clamp ((int) std::ceil  (k * wf), lo, half);
            const int loN = std::clamp ((int) std::floor (k / nf), lo, hi);
            const int hiN = std::clamp ((int) std::ceil  (k * nf), loN, hi);
            const int  wc = hi - lo + 1, nc = hiN - loN + 1;
            const double ws = pfx[(size_t) (hi + 1)] - pfx[(size_t) lo];
            if (wc <= 6 || wc - nc < 4)
                env[(size_t) k] = ws / (double) wc;
            else
                env[(size_t) k] = (ws - (pfx[(size_t) (hiN + 1)] - pfx[(size_t) loN])) / (double) (wc - nc);
        }

        // A2/A3: excess -> reduction. Spec constants: floor -80 dBFS (full-scale
        // sine bin = N/4), T(s) = 1+5s, W(s) = 6-4s, Hard threshold sweep
        // -6..-60 dBFS over depth 0..1.5, Hard slope 0.85 through a 6 dB knee,
        // -48 dB clamp, default range 20 Hz .. 20 kHz.
        const double floorMag = 0.25 * (double) N * std::pow (10.0, -80.0 / 20.0);
        const double T = 1.0 + 5.0 * p.selectivity, W = 6.0 - 4.0 * p.selectivity;
        const int lowBin  = std::clamp ((int) std::round (20.0    * N / Fs), 1, half);
        const int highBin = std::clamp ((int) std::round (20000.0 * N / Fs), 1, half);
        std::vector<double> red ((size_t) (half + 1), 0.0);
        for (int k = 0; k <= half; ++k)
        {
            const double mk = std::abs (spec[(size_t) k]);
            if (k < lowBin || k > highBin || p.depth <= 0.0 || mk <= floorMag) continue;
            const double exDb = (L[(size_t) k] - env[(size_t) k]) * (20.0 / std::log (10.0));
            double rd = 0.0;
            if (p.mode == 0)
                rd = -p.depth * specKneeOver (exDb, T, W);
            else
            {
                const double magDb = 20.0 * std::log10 ((mk + 1.0e-12) / (0.25 * (double) N));
                const double d     = std::clamp (p.depth / 1.5, 0.0, 1.0);
                const double thr   = -6.0 + d * (-60.0 + 6.0);
                if (specKneeOver (exDb, T, W) > 0.0)
                    rd = -0.85 * specKneeOver (magDb - thr, 0.0, 6.0);
            }
            red[(size_t) k] = std::max (rd, -48.0);
        }

        // A4: two cascaded variable-width box averages on redDb, half-width
        // h_k = max(1, round(k * (2^(oct/2) - 1))), edges clamped to [0, half]
        // and renormalised by the actual bin count. oct = 0 bypasses.
        if (p.smoothOct > 0.0)
        {
            const double fac = std::pow (2.0, 0.5 * p.smoothOct) - 1.0;
            for (int pass = 0; pass < 2; ++pass)
            {
                pfx[0] = 0.0;
                for (int k = 0; k <= half; ++k) pfx[(size_t) (k + 1)] = pfx[(size_t) k] + red[(size_t) k];
                for (int k = 0; k <= half; ++k)
                {
                    const int h  = std::max (1, (int) std::lround (k * fac));
                    const int lo = std::max (0, k - h), hi = std::min (half, k + h);
                    red[(size_t) k] = (pfx[(size_t) (hi + 1)] - pfx[(size_t) lo]) / (double) (hi - lo + 1);
                }
            }
        }
        return red;
    }

    // Drive the engine over x (mono -> both channels) with near-instant
    // ballistics and return the converged per-bin reductionDb() snapshot. x.size()
    // must be a multiple of the hop (N/4) so the last frame is x[M-N .. M-1].
    std::vector<double> engineReductionSnapshot (const std::vector<double>& x, double Fs,
                                                 int order, const SpecDetector& p)
    {
        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, order);
        s.setMode (p.mode); s.setDepth (p.depth); s.setSelectivity (p.selectivity);
        s.setSharpness (p.sharpOct); s.setSmoothingWidth (p.smoothOct);
        s.setTimes (0.01, 0.01); // coeff underflows to 0: gain == last frame's target
        for (size_t n = 0; n < x.size(); ++n) { double l = x[n], r = x[n]; s.process (l, r); }
        const double* rd = s.reductionDb();
        return std::vector<double> (rd, rd + s.numBins());
    }

    // Deterministic floor for the selectivity test: bin-aligned sines with
    // golden-ratio-scrambled fixed phases (no seed, no run-to-run variance; a
    // time shift by any hop leaves every bin magnitude unchanged, so all frames
    // are identical and the steady state is exact).
    template <typename AmpFn>
    std::vector<double> combSignal (int order, int M, AmpFn amp)
    {
        const int N = 1 << order;
        std::vector<double> x ((size_t) M, 0.0);
        for (int k = 1; k <= N / 2; ++k)
        {
            const double a = amp (k);
            if (a <= 0.0) continue;
            const double ph = 2.0 * kPi * std::fmod (0.6180339887498949 * k, 1.0);
            const double w  = 2.0 * kPi * k / (double) N; // bin-aligned (k * Fs / N Hz)
            for (int n = 0; n < M; ++n) x[(size_t) n] += a * std::sin (w * n + ph);
        }
        return x;
    }

    // A test signal length that is a multiple of the hop N/4 at every standard
    // order (11..13 -> hop 512..2048) and >= 4N at the top order.
    constexpr int kSnapshotLen = 1 << 15;

    // Pass A / A1: the self-excluding log-envelope must detect two equal
    // resonances 1/3 octave apart over a noise floor. Each peak sits inside the
    // other's envelope window, so the old linear-amplitude mean (no notch) let
    // the pair lift each other's baseline and mask both: an old-spec oracle
    // (linear mean, no notch, hard 3 dB gate, same smoothing) measures only
    // -5.25..-6.44 dB here across rates and would fail the >= 8 dB gate below.
    // The new spec detects -10.77..-14.61 dB (calibrated across all six rates;
    // engine == oracle to < 1e-14 dB). Spec values, fixed from the oracle:
    // both peaks <= -8 dB, |difference| <= 4 dB, engine-vs-oracle <= 0.5 dB.
    // The noise sigma scales with sqrt(N/2048) so the bin-domain tone/noise
    // ratio (tone bin ~ N, noise bin ~ sigma*sqrt(N)) is rate-invariant.
    void envelopeNotchTest (double Fs)
    {
        std::printf ("Envelope notch (two peaks 1/3 oct apart) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs), N = 1 << order, M = kSnapshotLen;
        const double f1 = alignedFreq (Fs, 2000.0);
        const double f2 = alignedFreq (Fs, 2000.0 * std::pow (2.0, 1.0 / 3.0));
        const int k1 = (int) std::round (f1 * N / Fs), k2 = (int) std::round (f2 * N / Fs);

        const double sigma = 0.05 * std::sqrt ((double) N / 2048.0);
        std::mt19937 rng (19);
        std::normal_distribution<double> g (0.0, sigma);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = g (rng) + 0.4 * std::sin (2.0 * kPi * f1 * n / Fs)
                                    + 0.4 * std::sin (2.0 * kPi * f2 * n / Fs);

        SpecDetector p; p.depth = 0.8;
        const auto eng = engineReductionSnapshot (x, Fs, order, p);
        const auto orc = specReductionOracle (x, M - N, Fs, order, p);

        for (int k : { k1, k2 })
            if (std::abs (eng[(size_t) k] - orc[(size_t) k]) > 0.5)
                fail ("envelope notch: engine != spec oracle at bin " + std::to_string (k)
                      + ": eng " + std::to_string (eng[(size_t) k]) + " orc " + std::to_string (orc[(size_t) k])
                      + " at Fs=" + std::to_string (Fs));
        if (eng[(size_t) k1] > -8.0 || eng[(size_t) k2] > -8.0)
            fail ("envelope notch: mutual masking (a peak under-detected, want <= -8 dB): k1 "
                  + std::to_string (eng[(size_t) k1]) + " k2 " + std::to_string (eng[(size_t) k2])
                  + " at Fs=" + std::to_string (Fs));
        if (std::abs (eng[(size_t) k1] - eng[(size_t) k2]) > 4.0)
            fail ("envelope notch: equal peaks treated unequally (> 4 dB apart): k1 "
                  + std::to_string (eng[(size_t) k1]) + " k2 " + std::to_string (eng[(size_t) k2])
                  + " at Fs=" + std::to_string (Fs));
        std::printf ("  red k1=%.2f dB k2=%.2f dB (spec: both <= -8, diff <= 4)\n",
                     eng[(size_t) k1], eng[(size_t) k2]);
    }

    // Pass A / A2: the Selectivity contrast law T(s) = 1+5s dB / W(s) = 6-4s dB,
    // on a deterministic comb floor (bins [kf/1.6, kf*1.6], amp 0.003) plus a
    // tone at kf ~ 2 kHz. Engine must match the spec oracle at s in {0, 0.5, 1}
    // both with the default smoothing and with smoothing off (tolerance
    // max(0.05 dB, 3% of the oracle value) -- the spec "a few percent" bound;
    // measured agreement < 1e-14 dB). Formula-independent invariants, smoothing
    // off so no neighbour bins mix in:
    //  - HOT peak (20x floor -> excess ~21.9 dB, linear knee branch, unclamped):
    //    reduction shifts by exactly depth*2.5 dB from s=0 to 0.5 and depth*5 dB
    //    from s=0 to 1 (T moves 1 -> 3.5 -> 6 dB) -- calibrated 2.5000 / 5.0000.
    //  - WEAK peak (1x floor on top of it -> excess 3.95 dB, inside the knee):
    //    reduction decreases monotonically in s (calibrated -2.949 / -0.750 /
    //    0.000 at depth 1) and is fully gated at s=1 (excess < T(1)-W(1)/2 = 5).
    void selectivityContrastTest (double Fs)
    {
        std::printf ("Selectivity contrast (soft-knee T/W law) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs), N = 1 << order, M = kSnapshotLen;
        const int kf = (int) std::round (2000.0 * N / Fs);
        const int jlo = (int) std::ceil (kf / 1.6), jhi = (int) std::floor (kf * 1.6);
        const double b = 0.003;

        auto redAt = [&] (const std::vector<double>& x, double sel, double smoothOct, double& orcOut)
        {
            SpecDetector p; p.depth = 1.0; p.selectivity = sel; p.smoothOct = smoothOct;
            const auto eng = engineReductionSnapshot (x, Fs, order, p);
            const auto orc = specReductionOracle (x, M - N, Fs, order, p);
            orcOut = orc[(size_t) kf];
            return eng[(size_t) kf];
        };
        auto checkOracle = [&] (double eng, double orc, const char* what, double sel)
        {
            if (std::abs (eng - orc) > std::max (0.05, 0.03 * std::abs (orc)))
                fail ("selectivity: engine != spec oracle (" + std::string (what) + ", s="
                      + std::to_string (sel) + "): eng " + std::to_string (eng) + " orc "
                      + std::to_string (orc) + " at Fs=" + std::to_string (Fs));
        };

        // HOT: tone 20x the floor on top of it (bin amp 21b, same phase) ->
        // linear knee branch at every s.
        {
            const auto x = combSignal (order, M, [&] (int k) {
                if (k < jlo || k > jhi) return 0.0;
                return (k == kf) ? 21.0 * b : b;
            });
            double e[3], o[3];
            int i = 0;
            for (double sel : { 0.0, 0.5, 1.0 })
            {
                e[i] = redAt (x, sel, 0.0, o[i]);              // smoothing off
                checkOracle (e[i], o[i], "hot/off", sel);
                double oSm = 0.0;
                const double eSm = redAt (x, sel, 1.0 / 12.0, oSm); // default smoothing
                checkOracle (eSm, oSm, "hot/smooth", sel);
                ++i;
            }
            if (e[0] > -15.0 || e[0] < -40.0)
                fail ("selectivity: hot peak not in the unclamped linear branch: red(s=0) "
                      + std::to_string (e[0]) + " at Fs=" + std::to_string (Fs));
            if (std::abs ((e[1] - e[0]) - 2.5) > 0.1)
                fail ("selectivity: T(s) sweep s0->s0.5 != depth*2.5 dB: "
                      + std::to_string (e[1] - e[0]) + " at Fs=" + std::to_string (Fs));
            if (std::abs ((e[2] - e[0]) - 5.0) > 0.1)
                fail ("selectivity: T(s) sweep s0->s1 != depth*5 dB: "
                      + std::to_string (e[2] - e[0]) + " at Fs=" + std::to_string (Fs));
            std::printf ("  hot  red(s)=(%.2f, %.2f, %.2f) dB (deltas %.4f / %.4f, expect 2.5 / 5)\n",
                         e[0], e[1], e[2], e[1] - e[0], e[2] - e[0]);
        }

        // WEAK: tone equal to the floor on top of it -> excess ~3.95 dB.
        {
            const auto x = combSignal (order, M, [&] (int k) {
                if (k < jlo || k > jhi) return 0.0;
                return (k == kf) ? 2.0 * b : b;
            });
            double e[3], o[3];
            int i = 0;
            for (double sel : { 0.0, 0.5, 1.0 })
            {
                e[i] = redAt (x, sel, 0.0, o[i]);
                checkOracle (e[i], o[i], "weak/off", sel);
                double oSm = 0.0;
                const double eSm = redAt (x, sel, 1.0 / 12.0, oSm);
                checkOracle (eSm, oSm, "weak/smooth", sel);
                ++i;
            }
            // Monotone decreasing reduction with s (calibrated margins 2.20 and
            // 0.75 dB at depth 1); fully gated at max selectivity.
            if (e[0] > e[1] - 1.5)
                fail ("selectivity: weak peak reduction not decreasing s0->s0.5: "
                      + std::to_string (e[0]) + " vs " + std::to_string (e[1]) + " at Fs=" + std::to_string (Fs));
            if (e[1] > e[2] - 0.4)
                fail ("selectivity: weak peak reduction not decreasing s0.5->s1: "
                      + std::to_string (e[1]) + " vs " + std::to_string (e[2]) + " at Fs=" + std::to_string (Fs));
            if (e[2] < -0.2)
                fail ("selectivity: weak (~4 dB excess) peak not gated at s=1: "
                      + std::to_string (e[2]) + " at Fs=" + std::to_string (Fs));
            std::printf ("  weak red(s)=(%.3f, %.3f, %.3f) dB (monotone, ~0 at s=1)\n", e[0], e[1], e[2]);
        }
    }

    // Pass A / A4: frequency-domain gain smoothing. A steady bin-aligned sine
    // puts a 3-bin -48 dB block into the raw reduction target; the converged
    // engine curve must equal the spec oracle on EVERY bin (spec tolerance
    // 0.5 dB; measured < 2e-15), with the default smoothing and, via
    // setSmoothingWidth(0), without it. With smoothing on, the adjacent-bin
    // step must stay under the 7 dB spec cap (calibrated max 5.76 dB across
    // rates); without it the raw -48 dB single-bin cliff (48 dB step) must be
    // present -- proving the cap gate is not vacuous.
    void gainSmoothingTest (double Fs)
    {
        std::printf ("Gain smoothing (2-pass box vs oracle) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs), N = 1 << order, half = N / 2, M = kSnapshotLen;
        const double f0 = alignedFreq (Fs, 2000.0);
        const int kf = (int) std::round (f0 * N / Fs);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        SpecDetector sm;  sm.depth = 1.2;              // default smoothing 1/12 oct
        SpecDetector raw = sm; raw.smoothOct = 0.0;    // smoothing bypassed
        const auto engS = engineReductionSnapshot (x, Fs, order, sm);
        const auto orcS = specReductionOracle (x, M - N, Fs, order, sm);
        const auto engR = engineReductionSnapshot (x, Fs, order, raw);
        const auto orcR = specReductionOracle (x, M - N, Fs, order, raw);

        double errS = 0.0, errR = 0.0;
        for (int k = 0; k <= half; ++k)
        {
            errS = std::max (errS, std::abs (engS[(size_t) k] - orcS[(size_t) k]));
            errR = std::max (errR, std::abs (engR[(size_t) k] - orcR[(size_t) k]));
        }
        if (errS > 0.5)
            fail ("gain smoothing: engine != spec oracle (smoothed) by " + std::to_string (errS)
                  + " dB at Fs=" + std::to_string (Fs));
        if (errR > 0.5)
            fail ("gain smoothing: setSmoothingWidth(0) != unsmoothed oracle by " + std::to_string (errR)
                  + " dB at Fs=" + std::to_string (Fs));

        auto maxAdj = [half] (const std::vector<double>& v)
        {
            double m = 0.0;
            for (int k = 1; k <= half; ++k) m = std::max (m, std::abs (v[(size_t) k] - v[(size_t) (k - 1)]));
            return m;
        };
        const double adjS = maxAdj (engS), adjR = maxAdj (engR);
        if (adjS > 7.0)
            fail ("gain smoothing: adjacent-bin step above the 7 dB spec cap: " + std::to_string (adjS)
                  + " dB at Fs=" + std::to_string (Fs));
        if (adjR < 40.0)
            fail ("gain smoothing: raw single-bin cliff missing (cap gate would be vacuous): "
                  + std::to_string (adjR) + " dB at Fs=" + std::to_string (Fs));
        if (engS[(size_t) kf] > -10.0)
            fail ("gain smoothing: cut did not survive smoothing at kf: " + std::to_string (engS[(size_t) kf])
                  + " dB at Fs=" + std::to_string (Fs));
        std::printf ("  allBinErr sm=%.2e raw=%.2e  maxAdj sm=%.2f raw=%.2f  red[kf]=%.2f\n",
                     errS, errR, adjS, adjR, engS[(size_t) kf]);
    }

    // Soft mode uses an adaptive threshold, so its reduction is invariant to input
    // level: the SAME resonance is cut by the SAME dB whether the signal is at
    // 0 dB or −12 dB. This is the defining property of the frequency-dependent
    // (Soft) mode — assert it directly, with no oracle (issue #34 style: the
    // property itself is the gate, measured through the real DSP).
    void softLevelInvarianceTest (double Fs)
    {
        std::printf ("Soft mode level invariance @ Fs=%.0f\n", Fs);
        const double f0 = alignedFreq (Fs, 2000.0);
        const double loud = resonanceReductionDb (Fs, f0, 0, 1.2, 1.0);   // hot
        const double quiet = resonanceReductionDb (Fs, f0, 0, 1.2, 0.25);  // −12 dB
        if (loud > -4.0)
            fail ("Soft: resonance not suppressed at 0 dB: " + std::to_string (loud) + " dB");
        if (std::abs (loud - quiet) > 0.5)
            fail ("Soft mode not level-invariant: loud " + std::to_string (loud)
                  + " dB vs quiet " + std::to_string (quiet) + " dB at Fs=" + std::to_string (Fs));
        std::printf ("  redF0 loud=%.2f dB  quiet=%.2f dB (expect ~equal)\n", loud, quiet);
    }

    // Hard mode reacts to absolute level: Depth sets an absolute threshold, so the
    // same resonance is cut hard when it sits above the threshold (loud) and left
    // essentially alone when it drops below it (quiet). This is the defining
    // property of the level-dependent (Hard) mode. Depth 0.6 puts the threshold
    // near −27.6 dBFS, between the loud (−6 dBFS) and quiet (−30.5 dBFS)
    // resonance. Pass A re-derivation: the Hard cut is now finite-slope
    // (0.85 * 6 dB-knee of the excess over the threshold, ~6.7:1) and the
    // reduction is smoothed across frequency, so a single-bin tone's observable
    // cut is intentionally shallower than the old infinite-ratio spec; operating
    // point and gates re-derived with the new spec oracle (measured across all
    // six rates: loud −5.74..−7.11 dB, quiet −0.00..−0.001 dB).
    void hardLevelDependenceTest (double Fs)
    {
        std::printf ("Hard mode level dependence @ Fs=%.0f\n", Fs);
        const double f0 = alignedFreq (Fs, 2000.0);
        const double loud  = resonanceReductionDb (Fs, f0, 1, 0.6, 1.0);   // −6 dBFS peak
        const double quiet = resonanceReductionDb (Fs, f0, 1, 0.6, 0.06);  // −30.5 dBFS peak
        if (loud > -4.5)
            fail ("Hard: loud resonance not suppressed: " + std::to_string (loud) + " dB");
        if (quiet < -1.0)
            fail ("Hard: quiet resonance should be nearly untouched: " + std::to_string (quiet) + " dB");
        if (quiet - loud < 4.5)
            fail ("Hard mode not level-dependent: loud " + std::to_string (loud)
                  + " dB vs quiet " + std::to_string (quiet) + " dB at Fs=" + std::to_string (Fs));
        std::printf ("  redF0 loud=%.2f dB  quiet=%.2f dB (expect loud << quiet)\n", loud, quiet);
    }

    // Hard mode must still be a *resonance* suppressor, not a broadband gate: a hot
    // resonance is cut hard while a resonance-free control band is left broadly
    // intact (selectivity), measured with an independent DFT. Pass A
    // re-derivation: the finite Hard slope + the A4 frequency smoothing dilute a
    // single-bin tone's cut by design, so the f0-vs-control selectivity gap spec
    // moves 15 dB -> 6.5 dB (measured 8.10..10.94 dB across the six rates); the
    // raw detector's selectivity is gated smoothing-off in
    // selectivityContrastTest. The f0 and control gates are unchanged.
    void hardSuppressionTest (double Fs)
    {
        std::printf ("Hard suppression + selectivity @ Fs=%.0f\n", Fs);
        const int M = 1 << 15;
        const double f0 = alignedFreq (Fs, 2000.0);
        std::mt19937 rng (5);
        std::normal_distribution<double> g (0.0, 0.1);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
        const auto wet = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) {
            s.setMode (1); s.setDepth (0.8); s.setSharpness (0.5);
        });

        const int a = M / 2, b = M;
        const double f0Db = 20.0 * std::log10 (magAt (wet, a, b, f0, Fs) / magAt (dry, a, b, f0, Fs));
        double dryCtrlE = 0.0, wetCtrlE = 0.0;
        for (double fc : { 4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9000.0, 10000.0 })
        {
            const double d = magAt (dry, a, b, fc, Fs), w = magAt (wet, a, b, fc, Fs);
            dryCtrlE += d * d; wetCtrlE += w * w;
        }
        const double ctrlDb = 10.0 * std::log10 (wetCtrlE / (dryCtrlE + 1e-30));
        if (f0Db > -6.0)
            fail ("Hard: resonance at f0 not suppressed (>=6 dB): " + std::to_string (f0Db) + " dB");
        if (ctrlDb < -4.5)
            fail ("Hard: control band over-attenuated (not selective): " + std::to_string (ctrlDb) + " dB");
        if (f0Db > ctrlDb - 6.5)
            fail ("Hard: not selective: f0 " + std::to_string (f0Db) + " dB vs control " + std::to_string (ctrlDb) + " dB");
        std::printf ("  f0 %.1f dB   control %.1f dB (broadband)\n", f0Db, ctrlDb);
    }

    // Hard mode is feed-forward (no feedback loop), but assert it anyway at the
    // worst-case Depth across all rates: the impulse-response tail must not grow
    // (loop gain < 1) and a long hot hold must stay finite and bounded (no NaN /
    // runaway), per the regression-policy numeric-safety invariants.
    void hardStabilityTest (double Fs)
    {
        std::printf ("Hard mode stability (finite / non-increasing) @ Fs=%.0f\n", Fs);
        factory_core::ResonanceSuppressor s; s.prepare (Fs, orderFor (Fs));
        s.setMode (1); s.setDepth (1.5); s.setSharpness (0.5); s.setMix (1.0);
        auto proc = [&s] (double in) { double l = in, r = in; s.process (l, r); return l; };
        if (! factory_core::testing::impulseResponseNonIncreasing (proc, Fs, 4.0, 0.25, 1.05))
            fail ("Hard mode impulse response grew (loop gain >= 1) at Fs=" + std::to_string (Fs));

        // Long hot noise hold: output must stay finite and bounded (a suppressor
        // only attenuates, so a realistic ceiling — not a 1e6 not-NaN tolerance).
        factory_core::ResonanceSuppressor s2; s2.prepare (Fs, orderFor (Fs));
        s2.setMode (1); s2.setDepth (1.5); s2.setSharpness (0.5); s2.setMix (1.0);
        const int M = (int) (2.0 * Fs);
        std::mt19937 rng (23);
        std::normal_distribution<double> g (0.0, 0.5);
        std::vector<double> y ((size_t) M);
        for (int n = 0; n < M; ++n) { double l = g (rng), r = l; s2.process (l, r); y[(size_t) n] = l; }
        if (! factory_core::testing::allFinite (y))
            fail ("Hard mode produced non-finite output at Fs=" + std::to_string (Fs));
        if (factory_core::testing::peakAbs (y) > 4.0)
            fail ("Hard mode output exceeded realistic bound: "
                  + std::to_string (factory_core::testing::peakAbs (y)) + " at Fs=" + std::to_string (Fs));

        // Resolution-follows-rate holds for the mode path too (independent of mode).
        if (! factory_core::testing::resolutionFollowsSampleRate (Fs, 25.0, 0.030))
            fail ("resolution out of range at Fs=" + std::to_string (Fs));
        std::printf ("  ok (peak=%.3f)\n", factory_core::testing::peakAbs (y));
    }

    // Regression guard for issue-class E (#30/#39/#41 style state-reset leaks):
    // drive the engine hard (dirty ring buffers, non-trivial idx/hop phase,
    // heavily-adapted per-bin gain state), call reset(), then feed an identical
    // subsequent signal into both the dirtied-then-reset engine and a freshly
    // constructed+prepared+configured one. If reset() truly clears all
    // processing state (ring buffers, gainL/gainR, idx, hop), the two must
    // produce bit-for-bit-close output on the same signal.
    void resetStateTest (double Fs)
    {
        std::printf ("reset() clears stale state @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const double f0 = alignedFreq (Fs, 2000.0);

        auto configure = [] (factory_core::ResonanceSuppressor& s)
        {
            s.setDepth (1.4); s.setSharpness (0.3); s.setTimes (5.0, 30.0);
        };

        factory_core::ResonanceSuppressor dirty;
        dirty.prepare (Fs, order);
        configure (dirty);
        {
            std::mt19937 rng (101);
            std::normal_distribution<double> g (0.0, 0.2);
            // Not a multiple of the hop size: leaves idx/hop mid-cycle, and long
            // enough to fully adapt the per-bin gain state away from unity.
            const int dirtyLen = (int) (0.7 * Fs) + 137;
            for (int n = 0; n < dirtyLen; ++n)
            {
                double l = g (rng) + 0.6 * std::sin (2.0 * kPi * f0 * n / Fs);
                double r = l;
                dirty.process (l, r);
            }
        }
        dirty.reset();

        factory_core::ResonanceSuppressor fresh;
        fresh.prepare (Fs, order);
        configure (fresh);

        const int M = 1 << 15;
        std::mt19937 rng (303);
        std::normal_distribution<double> g (0.0, 0.15);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        double maxErr = 0.0;
        for (int n = 0; n < M; ++n)
        {
            double lA = x[(size_t) n], rA = x[(size_t) n];
            double lB = x[(size_t) n], rB = x[(size_t) n];
            dirty.process (lA, rA);
            fresh.process (lB, rB);
            maxErr = std::max ({ maxErr, std::abs (lA - lB), std::abs (rA - rB) });
        }
        if (maxErr > 1.0e-9)
            fail ("reset() left stale state: dirtied-then-reset engine diverges from a fresh one by "
                  + std::to_string (maxErr) + " at Fs=" + std::to_string (Fs));
        std::printf ("  maxErr(dirty-vs-fresh after reset)=%.2e\n", maxErr);
    }

    // Regression guard for issue-class F (ballistics never varied from
    // defaults): release recovery, measured via the same per-bin gain the GUI
    // reads (reductionDb()), must (a) be ordered -- a fast release recovers to
    // near-unity gain in fewer hops than a slow one, with a meaningful margin --
    // and (b) quantitatively track an independent one-pole prediction. After the
    // resonant tone is replaced by true silence and the analysis window is fully
    // flushed of the old tone (>= N/H hops), every subsequent frame's target is
    // exactly 1.0 (mag below the absolute floor), so the smoother is a clean
    // one-pole recurrence g[m] = c^m*(g0-1) + 1 with c = exp(-frameTime/tau),
    // frameTime = hop/Fs, tau = releaseMs/1000 -- the standard EMA/one-pole
    // time-constant relation, not a value read out of computeGains().
    void releaseBallisticsTest (double Fs)
    {
        std::printf ("Release ballistics (fast vs slow, one-pole check) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int H = N / 8; // Normal quality = 8x overlap (hop N/8); B2-2 doubled the frame rate
        const double f0 = alignedFreq (Fs, 2000.0);
        const int kf = (int) std::round (f0 * (double) N / Fs);
        const double atkMs = 2.0; // fast & common to both runs: fully converged before switch-off

        auto measure = [&] (double relMs)
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, order);
            s.setDepth (1.3); s.setSharpness (0.5); s.setTimes (atkMs, relMs);

            std::mt19937 rng (17);
            std::normal_distribution<double> g (0.0, 0.05);
            const int settleSamples = (int) (0.5 * Fs);
            for (int n = 0; n < settleSamples; ++n)
            {
                double l = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);
                double r = l;
                s.process (l, r);
            }

            // Sanity: the tone was genuinely suppressed before the switch-off
            // (this is what the "recovery" below recovers from). Checked here,
            // before flush, because a fast release can already claw back part
            // of that reduction during the flush hops themselves.
            const double gBeforeSwitch = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);
            if (gBeforeSwitch > 0.6)
                fail ("release test: resonance not meaningfully suppressed before release (relMs=" + std::to_string (relMs)
                      + ") g=" + std::to_string (gBeforeSwitch) + " at Fs=" + std::to_string (Fs));

            // Switch to true silence; flush the analysis window of the old tone
            // (window = N samples = N/H hops) before trusting target == 1.0.
            const int flushHops = N / H + 2;
            for (int h = 0; h < flushHops; ++h)
                for (int n = 0; n < H; ++n) { double l = 0.0, r = 0.0; s.process (l, r); }

            const double g0 = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);

            const int measureHops = 40;
            std::vector<double> traj ((size_t) measureHops);
            for (int h = 0; h < measureHops; ++h)
            {
                for (int n = 0; n < H; ++n) { double l = 0.0, r = 0.0; s.process (l, r); }
                traj[(size_t) h] = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);
            }
            return std::pair<double, std::vector<double>> { g0, traj };
        };

        const double fastRelMs = 20.0, slowRelMs = 400.0;
        const auto [g0Fast, trajFast] = measure (fastRelMs);
        const auto [g0Slow, trajSlow] = measure (slowRelMs);

        auto hopsToRecover = [] (const std::vector<double>& traj, double thresh)
        {
            for (size_t i = 0; i < traj.size(); ++i) if (traj[i] >= thresh) return (int) i;
            return (int) traj.size();
        };
        const double recoverThresh = 0.9; // gain back within ~1 dB of unity
        const int hopsFast = hopsToRecover (trajFast, recoverThresh);
        const int hopsSlow = hopsToRecover (trajSlow, recoverThresh);
        if (! (hopsFast < hopsSlow))
            fail ("fast release should recover sooner than slow: hopsFast=" + std::to_string (hopsFast)
                  + " hopsSlow=" + std::to_string (hopsSlow) + " at Fs=" + std::to_string (Fs));
        if (hopsSlow - hopsFast < 3)
            fail ("release ordering margin too small: hopsFast=" + std::to_string (hopsFast)
                  + " hopsSlow=" + std::to_string (hopsSlow) + " at Fs=" + std::to_string (Fs));

        auto predict = [&] (double relMs, double g0, int hops)
        {
            const double tau = relMs * 1.0e-3;
            const double frameTime = (double) H / Fs;
            const double c = std::exp (-frameTime / tau);
            std::vector<double> p ((size_t) hops);
            double g = g0;
            for (int i = 0; i < hops; ++i) { g = c * g + (1.0 - c) * 1.0; p[(size_t) i] = g; }
            return p;
        };
        const auto predFast = predict (fastRelMs, g0Fast, (int) trajFast.size());
        const auto predSlow = predict (slowRelMs, g0Slow, (int) trajSlow.size());

        double errFast = 0.0, errSlow = 0.0;
        for (size_t i = 0; i < trajFast.size(); ++i) errFast = std::max (errFast, std::abs (trajFast[i] - predFast[i]));
        for (size_t i = 0; i < trajSlow.size(); ++i) errSlow = std::max (errSlow, std::abs (trajSlow[i] - predSlow[i]));
        const double tol = 0.05;
        if (errFast > tol) fail ("fast release trajectory vs one-pole prediction err " + std::to_string (errFast)
                                  + " at Fs=" + std::to_string (Fs));
        if (errSlow > tol) fail ("slow release trajectory vs one-pole prediction err " + std::to_string (errSlow)
                                  + " at Fs=" + std::to_string (Fs));
        std::printf ("  hopsFast=%d hopsSlow=%d  predErrFast=%.3f predErrSlow=%.3f\n",
                     hopsFast, hopsSlow, errFast, errSlow);
    }

    // Same idea for attack: a fast attack must reach the (shared) steady-state
    // reduction target sooner than a slow one, with a meaningful margin, and the
    // post-ramp-in tail is quantitatively checked against the same one-pole
    // relation. Because the STFT's own magnitude estimate ramps in over the
    // first N/H hops regardless of attack speed (physical window-fill, not
    // ballistics), the run first "primes" the window with the resonant tone for
    // that many hops (both runs use identical rng/signal so this ramp-in is
    // byte-identical either way and cannot bias the ordering check), then reads
    // the gain at that point as the one-pole recursion's initial condition and
    // predicts the remaining approach toward an independently-measured
    // steady-state target (obtained from a separate, very-fast-attack reference
    // run so it does not depend on the attackMs values under test).
    void attackBallisticsTest (double Fs)
    {
        std::printf ("Attack ballistics (fast vs slow, one-pole check) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int H = N / 8; // Normal quality = 8x overlap (hop N/8); B2-2 doubled the frame rate
        const double f0 = alignedFreq (Fs, 2000.0);
        const int kf = (int) std::round (f0 * (double) N / Fs);
        const double relMs = 50.0; // fixed release; irrelevant while the attack branch is active
        const int primeHops = N / H + 2;

        auto driveTone = [&] (factory_core::ResonanceSuppressor& s, std::mt19937& rng, int& n, int samples)
        {
            std::normal_distribution<double> g (0.0, 0.05);
            for (int i = 0; i < samples; ++i, ++n)
            {
                double l = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);
                double r = l;
                s.process (l, r);
            }
        };

        double targetSteady;
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, order);
            s.setDepth (1.3); s.setSharpness (0.5); s.setTimes (0.5, relMs);
            std::mt19937 rng (17);
            int n = 0;
            driveTone (s, rng, n, (int) (1.0 * Fs));
            targetSteady = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);
        }

        auto measure = [&] (double atkMs)
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, order);
            s.setDepth (1.3); s.setSharpness (0.5); s.setTimes (atkMs, relMs);

            std::mt19937 rng (17);
            int n = 0;
            for (int h = 0; h < primeHops; ++h) driveTone (s, rng, n, H);
            const double g0 = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);

            const int measureHops = 60;
            std::vector<double> traj ((size_t) measureHops);
            for (int h = 0; h < measureHops; ++h)
            {
                driveTone (s, rng, n, H);
                traj[(size_t) h] = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);
            }
            return std::pair<double, std::vector<double>> { g0, traj };
        };

        const double fastAtkMs = 5.0, slowAtkMs = 300.0;
        const auto [g0Fast, trajFast] = measure (fastAtkMs);
        const auto [g0Slow, trajSlow] = measure (slowAtkMs);

        if (trajFast.back() > 0.6 || trajSlow.back() > 0.6)
            fail ("attack test: resonance not meaningfully suppressed by end of window at Fs=" + std::to_string (Fs));

        // "Reaches target reduction sooner": first hop at/below the halfway
        // point between untouched (1.0) and the shared steady-state target.
        const double halfway = 0.5 * (1.0 + targetSteady);
        auto hopsToReach = [] (const std::vector<double>& traj, double thresh)
        {
            for (size_t i = 0; i < traj.size(); ++i) if (traj[i] <= thresh) return (int) i;
            return (int) traj.size();
        };
        const int hopsFast = hopsToReach (trajFast, halfway);
        const int hopsSlow = hopsToReach (trajSlow, halfway);
        if (! (hopsFast < hopsSlow))
            fail ("fast attack should reach target reduction sooner than slow: hopsFast=" + std::to_string (hopsFast)
                  + " hopsSlow=" + std::to_string (hopsSlow) + " at Fs=" + std::to_string (Fs));
        if (hopsSlow - hopsFast < 5)
            fail ("attack ordering margin too small: hopsFast=" + std::to_string (hopsFast)
                  + " hopsSlow=" + std::to_string (hopsSlow) + " at Fs=" + std::to_string (Fs));

        auto predict = [&] (double atkMs, double g0, int hops)
        {
            const double tau = atkMs * 1.0e-3;
            const double frameTime = (double) H / Fs;
            const double c = std::exp (-frameTime / tau);
            std::vector<double> p ((size_t) hops);
            double g = g0;
            for (int i = 0; i < hops; ++i) { g = c * g + (1.0 - c) * targetSteady; p[(size_t) i] = g; }
            return p;
        };
        const auto predFast = predict (fastAtkMs, g0Fast, (int) trajFast.size());
        const auto predSlow = predict (slowAtkMs, g0Slow, (int) trajSlow.size());

        double errFast = 0.0, errSlow = 0.0;
        for (size_t i = 0; i < trajFast.size(); ++i) errFast = std::max (errFast, std::abs (trajFast[i] - predFast[i]));
        for (size_t i = 0; i < trajSlow.size(); ++i) errSlow = std::max (errSlow, std::abs (trajSlow[i] - predSlow[i]));
        // Looser than the release check: unlike silence, the tone+noise signal
        // still has small hop-to-hop magnitude jitter after priming.
        const double tol = 0.10;
        if (errFast > tol) fail ("fast attack trajectory vs one-pole prediction err " + std::to_string (errFast)
                                  + " at Fs=" + std::to_string (Fs));
        if (errSlow > tol) fail ("slow attack trajectory vs one-pole prediction err " + std::to_string (errSlow)
                                  + " at Fs=" + std::to_string (Fs));
        std::printf ("  targetSteady=%.3f hopsFast=%d hopsSlow=%d  predErrFast=%.3f predErrSlow=%.3f\n",
                     targetSteady, hopsFast, hopsSlow, errFast, errSlow);
    }

    // Regression guard: setRange() is exercised with a narrow band (the
    // processor otherwise hardcodes 20-20000 Hz). (a) A resonance inside the
    // configured range is still suppressed; (b) a resonance outside it produces
    // NO suppression at all -- with an isolated single tone outside the range
    // (every bin, including any Hann-window leakage bins, sits well clear of
    // [lowBin, highBin]), the whole output must match the latency-aligned input
    // to the same FFT-roundtrip tolerance as the depth=0 reconstruction test.
    void rangeGatingTest (double Fs)
    {
        std::printf ("Range gating (setRange) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const double lowHz = 1000.0, highHz = 3000.0;
        const double insideF0  = alignedFreq (Fs, 1800.0);
        const double outsideF0 = alignedFreq (Fs, 6000.0);

        // (a) In-range resonance is still suppressed.
        {
            const int M = 1 << 15;
            std::mt19937 rng (51);
            std::normal_distribution<double> g (0.0, 0.1);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * insideF0 * n / Fs);

            const auto dry = render (Fs, x, x, [] (factory_core::ResonanceSuppressor& s) { s.setDepth (0.0); });
            const auto wet = render (Fs, x, x, [lowHz, highHz] (factory_core::ResonanceSuppressor& s) {
                s.setRange (lowHz, highHz); s.setDepth (1.2); s.setSharpness (0.5);
            });
            const int a = M / 2, b = M;
            const double dB = 20.0 * std::log10 (magAt (wet, a, b, insideF0, Fs) / magAt (dry, a, b, insideF0, Fs));
            if (dB > -4.4)
                fail ("in-range resonance not suppressed with setRange active: " + std::to_string (dB)
                      + " dB at Fs=" + std::to_string (Fs));
            std::printf ("  inside-range %.1f dB (expect strong cut)\n", dB);
        }

        // (b) Out-of-range resonance: identity within reconstruction tolerance.
        {
            const int N = 1 << order;
            const int M = std::max (1 << 14, 4 * N);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = 0.5 * std::sin (2.0 * kPi * outsideF0 * n / Fs);

            const auto wet = render (Fs, x, x, [lowHz, highHz] (factory_core::ResonanceSuppressor& s) {
                s.setRange (lowHz, highHz); s.setDepth (1.4); s.setSharpness (0.3); s.setMix (1.0);
            });

            double e = 0.0;
            for (int n = 2 * N; n < M; ++n) e = std::max (e, std::abs (wet[(size_t) n] - x[(size_t) (n - N)]));
            if (e > 1.0e-6)
                fail ("out-of-range content modified despite setRange: err " + std::to_string (e)
                      + " at Fs=" + std::to_string (Fs));
            std::printf ("  outside-range maxErr=%.2e (expect ~identity)\n", e);
        }
    }

    // Documents current behaviour for a hard mid-stream depth/sharpness step
    // (both are applied with no smoothing of their own -- only the resulting
    // per-bin gain ramps via attack/release): output must stay finite and
    // within a realistic peak bound, and the step must not introduce a
    // sample-to-sample slew far beyond the signal's own steady-state slew. This
    // is a coarse sanity/documentation gate, not a tight spec -- if it ever
    // finds a genuine zipper, that is a core bug to report, not to paper over
    // here.
    void depthSharpnessMidStreamTest (double Fs)
    {
        std::printf ("Depth/sharpness hard step mid-stream (no runaway) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int M = std::max (1 << 16, 8 * N);
        const double f0 = alignedFreq (Fs, 2000.0);

        std::mt19937 rng (61);
        std::normal_distribution<double> g (0.0, 0.15);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

        factory_core::ResonanceSuppressor s;
        s.prepare (Fs, order);
        s.setDepth (0.1); s.setSharpness (0.3);

        std::vector<double> y ((size_t) M);
        const int switchAt = M / 2;
        for (int n = 0; n < M; ++n)
        {
            if (n == switchAt) { s.setDepth (1.5); s.setSharpness (1.5); }
            double l = x[(size_t) n], r = x[(size_t) n];
            s.process (l, r);
            y[(size_t) n] = 0.5 * (l + r);
        }

        if (! factory_core::testing::allFinite (y))
            fail ("depth/sharpness step produced non-finite output at Fs=" + std::to_string (Fs));
        const double peak = factory_core::testing::peakAbs (y);
        if (peak > 3.0)
            fail ("depth/sharpness step produced unrealistic peak " + std::to_string (peak)
                  + " at Fs=" + std::to_string (Fs));

        auto maxSlew = [&] (int a, int b)
        {
            double m = 0.0;
            for (int n = a + 1; n < b; ++n) m = std::max (m, std::abs (y[(size_t) n] - y[(size_t) (n - 1)]));
            return m;
        };
        const double baseline   = maxSlew (N, switchAt - 2 * N);
        const double transition = maxSlew (switchAt - N, std::min (M, switchAt + 3 * N));
        const double bound = std::max (0.05, baseline * 8.0); // generous: documents behaviour, not a tight spec
        if (transition > bound)
            fail ("depth/sharpness step introduced a slew beyond a realistic bound: baseline="
                  + std::to_string (baseline) + " transition=" + std::to_string (transition)
                  + " at Fs=" + std::to_string (Fs));
        std::printf ("  peak=%.3f baselineSlew=%.4f transitionSlew=%.4f (bound=%.4f)\n",
                     peak, baseline, transition, bound);
    }

    // Pass B-2 / B2-1: per-bin ballistics with frequency Tilt. Tilt scales each
    // bin's time constant by s(f) = (f/1kHz)^(-Tilt*log10 4) about a 1 kHz pivot, so
    // Tilt +1 makes highs 4x faster and lows 4x slower. Measured on the RELEASE
    // (tone -> silence, so once the analysis window is flushed the target is exactly
    // 1.0 and the smoother is a clean one-pole g[m] = c^m*(g0-1)+1). The oracle is
    // the spec formula onePoleCoeffForMs(clamp(baseMs*s(f_k), 0.05, 5000), frameRate)
    // computed test-side from the bin's exact frequency f_k = k*Fs/N -- it never
    // reads the engine's coefficient array. Two gates per (freq, tilt): (i) the
    // measured recovery trajectory matches the one-pole prediction (predErr <= tol),
    // and (ii) direction -- at Tilt +1 the 8 kHz bin recovers in FEWER hops than at
    // Tilt 0 and the 100 Hz bin in MORE (mirror at Tilt -1). baseRelMs is slow enough
    // that every tilted time constant stays longer than the N/H = 8-hop window fill,
    // so the flushed one-pole is clean.
    void tiltBallisticsTest (double Fs)
    {
        std::printf ("Tilt ballistics (per-bin time constants) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs), N = 1 << order, H = N / 8; // Normal 8x
        const double baseAtkMs = 5.0;    // fast attack: converges well before the switch-off
        const double baseRelMs = 200.0;  // slow-ish release: the quantity actually measured
        const int flushHops = N / H + 2; // flush the tone out of the window before target == 1
        const double kExp = 0.60205999132796239; // log10(4)
        auto sOf = [kExp] (double f, double t) { return std::pow (f / 1000.0, -t * kExp); };

        struct Meas { double gBefore, g0; std::vector<double> traj; };
        auto measureRelease = [&] (double freqHz, double tilt) -> Meas
        {
            const double f0 = alignedFreq (Fs, freqHz);
            const int kf = (int) std::round (f0 * (double) N / Fs);
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, order);
            s.setDepth (1.0); s.setSharpness (0.5); s.setTilt (tilt); s.setTimes (baseAtkMs, baseRelMs);

            std::mt19937 rng (17);
            std::normal_distribution<double> g (0.0, 0.05);
            const int settleSamples = (int) (0.4 * Fs);
            for (int n = 0; n < settleSamples; ++n)
            {
                double l = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs), r = l;
                s.process (l, r);
            }
            const double gBefore = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);

            for (int h = 0; h < flushHops; ++h)
                for (int n = 0; n < H; ++n) { double l = 0.0, r = 0.0; s.process (l, r); }
            const double g0 = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);

            const int measureHops = 100;
            std::vector<double> traj ((size_t) measureHops);
            for (int h = 0; h < measureHops; ++h)
            {
                for (int n = 0; n < H; ++n) { double l = 0.0, r = 0.0; s.process (l, r); }
                traj[(size_t) h] = std::pow (10.0, s.reductionDb()[(size_t) kf] / 20.0);
            }
            return { gBefore, g0, traj };
        };

        auto predict = [&] (double freqHz, double tilt, double g0, int hops)
        {
            // Independent one-pole oracle: c = exp(-frameTime/tau) with the spec tilt
            // scale tau = clamp(baseMs*s(f_k), 0.05, 5000) ms, computed test-side from
            // the bin's exact frequency (never reads the engine's coefficient array).
            const double f0 = alignedFreq (Fs, freqHz);
            const double tauSec = std::clamp (baseRelMs * sOf (f0, tilt), 0.05, 5000.0) * 1.0e-3;
            const double frameTime = (double) H / Fs; // = 1/frameRate
            const double c = std::exp (-frameTime / tauSec);
            std::vector<double> p ((size_t) hops);
            double gg = g0;
            for (int i = 0; i < hops; ++i) { gg = c * gg + (1.0 - c) * 1.0; p[(size_t) i] = gg; }
            return p;
        };
        auto hopsToRecover = [] (const std::vector<double>& t, double thr)
        {
            for (size_t i = 0; i < t.size(); ++i) if (t[i] >= thr) return (int) i;
            return (int) t.size();
        };

        struct Dir { int hopsM, hops0, hopsP; };
        auto runFreq = [&] (double freqHz) -> Dir
        {
            int hops[3]; int ti = 0;
            for (double tilt : { -1.0, 0.0, 1.0 })
            {
                const Meas m = measureRelease (freqHz, tilt);
                if (m.gBefore > 0.6)
                    fail ("tilt: resonance not suppressed before release (freq=" + std::to_string (freqHz)
                          + " tilt=" + std::to_string (tilt) + ") g=" + std::to_string (m.gBefore)
                          + " at Fs=" + std::to_string (Fs));
                const auto pred = predict (freqHz, tilt, m.g0, (int) m.traj.size());
                double err = 0.0;
                for (size_t i = 0; i < m.traj.size(); ++i) err = std::max (err, std::abs (m.traj[i] - pred[i]));
                if (err > 0.05)
                    fail ("tilt: release trajectory vs one-pole oracle err " + std::to_string (err)
                          + " (freq=" + std::to_string (freqHz) + " tilt=" + std::to_string (tilt)
                          + ") at Fs=" + std::to_string (Fs));
                hops[ti++] = hopsToRecover (m.traj, 0.9);
            }
            return { hops[0], hops[1], hops[2] };
        };

        const Dir hi = runFreq (8000.0);
        const Dir lo = runFreq (100.0);

        // Direction: Tilt +1 speeds highs (fewer recovery hops) and slows lows (more);
        // Tilt -1 mirrors. Each with a >= 2-hop margin so frame quantisation can't flip it.
        if (! (hi.hopsP + 2 <= hi.hops0))
            fail ("tilt: 8 kHz not faster at +1 (hopsP=" + std::to_string (hi.hopsP) + " hops0="
                  + std::to_string (hi.hops0) + ") at Fs=" + std::to_string (Fs));
        if (! (hi.hopsM >= hi.hops0 + 2))
            fail ("tilt: 8 kHz not slower at -1 (hopsM=" + std::to_string (hi.hopsM) + " hops0="
                  + std::to_string (hi.hops0) + ") at Fs=" + std::to_string (Fs));
        if (! (lo.hopsP >= lo.hops0 + 2))
            fail ("tilt: 100 Hz not slower at +1 (hopsP=" + std::to_string (lo.hopsP) + " hops0="
                  + std::to_string (lo.hops0) + ") at Fs=" + std::to_string (Fs));
        if (! (lo.hopsM + 2 <= lo.hops0))
            fail ("tilt: 100 Hz not faster at -1 (hopsM=" + std::to_string (lo.hopsM) + " hops0="
                  + std::to_string (lo.hops0) + ") at Fs=" + std::to_string (Fs));

        std::printf ("  8k hops(-1,0,+1)=(%d,%d,%d)  100 hops(-1,0,+1)=(%d,%d,%d)\n",
                     hi.hopsM, hi.hops0, hi.hopsP, lo.hopsM, lo.hops0, lo.hopsP);
    }

    // Pass B-2 / B2-2: Quality (Fast/Normal/High) + 8x overlap + latency-changing
    // switch. order_q = normalOrder + (q-1), overlap_q = 4 (Fast) else 8, so latency
    // N_q = 1<<order_q and hop H_q = N_q/overlap_q. Per quality: (a) latencySamples()
    // and hopSamples() equal the spec table; (b) depth=0 reconstruction to the
    // latency-aligned dry (<= 1e-9); (c) bypass is bit-transparent to the aligned dry.
    // Then a mid-stream Normal->Fast->High switch: (d) all samples finite, the N_q
    // refill hold outputs the aligned dry bit-exactly (checked with depth on, so wet
    // != dry makes the hold observable), and with depth=0 the reconstruction returns
    // to <= 1e-9 once each switch settles.
    void qualityTest (double Fs)
    {
        std::printf ("Quality switch (Fast/Normal/High, 8x overlap) @ Fs=%.0f\n", Fs);
        const int normalOrder = orderFor (Fs);
        const int maxOrder = normalOrder + 1; // engine default maxOrder
        auto orderForQ = [&] (int q) { return std::clamp (normalOrder + (q - 1), 1, maxOrder); };
        auto Nq = [&] (int q) { return 1 << orderForQ (q); };
        auto Hq = [&] (int q) { return Nq (q) / ((q == 0) ? 4 : 8); };

        // (a) latency/hop table + (b) depth=0 reconstruction, per quality.
        for (int q = 0; q < 3; ++q)
        {
            const int nq = Nq (q), hq = Hq (q);
            const int M = std::max (1 << 15, 8 * nq);
            std::mt19937 rng (71);
            std::uniform_real_distribution<double> u (-0.5, 0.5);
            std::vector<double> x ((size_t) M);
            for (auto& v : x) v = u (rng);

            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, normalOrder);
            s.setQuality (q); s.setDepth (0.0); s.setMix (1.0);
            std::vector<double> y ((size_t) M);
            for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = x[(size_t) n]; s.process (l, r); y[(size_t) n] = l; }

            if (s.latencySamples() != nq)
                fail ("quality " + std::to_string (q) + ": latency " + std::to_string (s.latencySamples())
                      + " != " + std::to_string (nq) + " at Fs=" + std::to_string (Fs));
            if (s.hopSamples() != hq)
                fail ("quality " + std::to_string (q) + ": hop " + std::to_string (s.hopSamples())
                      + " != " + std::to_string (hq) + " at Fs=" + std::to_string (Fs));

            double e = 0.0;
            for (int n = M / 2; n < M; ++n) e = std::max (e, std::abs (y[(size_t) n] - x[(size_t) (n - nq)]));
            if (e > 1.0e-9)
                fail ("quality " + std::to_string (q) + ": depth0 reconstruction err " + std::to_string (e)
                      + " at Fs=" + std::to_string (Fs));
            std::printf ("  q=%d N=%d H=%d reconErr=%.2e\n", q, nq, hq, e);
        }

        // (c) bypass bit-transparency per quality (engine active underneath).
        for (int q = 0; q < 3; ++q)
        {
            const int nq = Nq (q);
            const int M = std::max (1 << 15, 8 * nq);
            std::mt19937 rng (83);
            std::normal_distribution<double> g (0.0, 0.3);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.4 * std::sin (2.0 * kPi * 1200.0 * n / Fs);

            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, normalOrder);
            s.setQuality (q); s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.7); s.setBypassed (true);
            std::vector<double> y ((size_t) M);
            for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = x[(size_t) n]; s.process (l, r); y[(size_t) n] = l; }

            // Tail (past the bypass ramp + switch + refill hold): full bypass reads
            // the input ring with no arithmetic, so it is bit-exact to the aligned dry.
            for (int n = M / 2; n < M; ++n)
                if (y[(size_t) n] != x[(size_t) (n - nq)])
                { fail ("quality " + std::to_string (q) + ": bypass not bit-transparent at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); break; }
        }

        // (d) mid-stream Normal -> Fast -> High switch. settle covers the N_q refill
        // hold (N_q <= bufLen) + the 10 ms ramp; segments are wide enough to leave a
        // large checked region after it (asserted non-vacuous below).
        const int bufLen = 1 << maxOrder;
        const int settle = bufLen + (int) std::ceil (0.010 * Fs) + 256;
        const int t1 = 2 * bufLen, t2 = 6 * bufLen, M = 10 * bufLen;
        std::mt19937 rng (97);
        std::normal_distribution<double> g (0.0, 0.25);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 2000.0 * n / Fs);

        // Pass 1 (depth on): finite everywhere; forced-dry (bit-exact) during each hold.
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, normalOrder);
            s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.9);
            std::vector<double> y ((size_t) M);
            int prevLat = s.latencySamples(), holdRem = 0, holdNq = 0;
            int switches = 0, holdChecked = 0;
            bool holdBad = false;
            for (int n = 0; n < M; ++n)
            {
                if (n == t1) s.setQuality (0);
                if (n == t2) s.setQuality (2);
                double l = x[(size_t) n], r = x[(size_t) n];
                s.process (l, r);
                y[(size_t) n] = l;
                const int lat = s.latencySamples();
                if (lat != prevLat) { holdRem = lat; holdNq = lat; prevLat = lat; ++switches; } // switch sample (excluded)
                else if (holdRem > 0)
                {
                    if (! holdBad && (l != x[(size_t) (n - holdNq)] || r != x[(size_t) (n - holdNq)]))
                    { fail ("quality mid-switch: refill hold not aligned dry at n=" + std::to_string (n)
                            + " Fs=" + std::to_string (Fs)); holdBad = true; }
                    ++holdChecked;
                    --holdRem;
                }
            }
            if (! factory_core::testing::allFinite (y))
                fail ("quality mid-switch produced non-finite output at Fs=" + std::to_string (Fs));
            if (switches != 2)                 // both Normal->Fast and Fast->High must apply
                fail ("quality mid-switch: expected 2 latency changes, saw " + std::to_string (switches)
                      + " at Fs=" + std::to_string (Fs));
            if (holdChecked <= bufLen)         // guard: the forced-dry hold check must not be vacuous
                fail ("quality mid-switch: hold check vacuous (holdChecked=" + std::to_string (holdChecked)
                      + ") at Fs=" + std::to_string (Fs));
        }

        // Pass 2 (depth=0): reconstruction returns to <= 1e-9 once each switch settles.
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, normalOrder);
            s.setDepth (0.0); s.setMix (1.0);
            std::vector<double> y ((size_t) M), lat ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                if (n == t1) s.setQuality (0);
                if (n == t2) s.setQuality (2);
                double l = x[(size_t) n], r = x[(size_t) n];
                s.process (l, r);
                y[(size_t) n] = l; lat[(size_t) n] = (double) s.latencySamples();
            }
            int curLat = (int) lat[0], lastChange = 0, checked = 0;
            double e = 0.0;
            for (int n = 0; n < M; ++n)
            {
                if ((int) lat[(size_t) n] != curLat) { curLat = (int) lat[(size_t) n]; lastChange = n; }
                if (n - lastChange > settle && n >= curLat + settle)
                {
                    e = std::max (e, std::abs (y[(size_t) n] - x[(size_t) (n - curLat)]));
                    ++checked;
                }
            }
            if (checked < bufLen) // guard: the settled-region check must not be vacuous
                fail ("quality mid-switch: depth0 check vacuous (checked=" + std::to_string (checked)
                      + ") at Fs=" + std::to_string (Fs));
            if (e > 1.0e-9)
                fail ("quality mid-switch: depth0 reconstruction across switches err " + std::to_string (e)
                      + " at Fs=" + std::to_string (Fs));
            std::printf ("  mid-switch reconErr=%.2e checked=%d (t1=%d t2=%d M=%d)\n", e, checked, t1, t2, M);
        }
    }

    // ---- Pass 2A: dual-resolution composite (MultiResSuppressor) -------------
    // The composite splits the input with an LR4 at 3 kHz (spec constant,
    // mirrored test-side below), runs the low band at order O and the high band
    // at order O-2 (N_S = N_L/4, so the high frame rate is 4x), pre-delays the
    // high band by N_L - N_S and sums. Every dry reference here is built
    // test-side: an LR4 "unity twin" (same class / cutoff / rate as the
    // implementation's split; for LR4 low+high is an allpass) plus plain
    // integer delays -- never the implementation's own delay rings.

    std::vector<double> lr4AllpassRef (const std::vector<double>& x, double Fs)
    {
        factory_core::LinkwitzRiley twin;
        twin.setCutoff (3000.0, Fs); // kSplitHz spec value
        std::vector<double> ap (x.size());
        for (size_t n = 0; n < x.size(); ++n) ap[n] = twin.allpass (x[n]);
        return ap;
    }

    // Spec latency table: N_L(q) = 1 << clamp(order + q - 1, 1, order + 1)
    // (default maxOrder = order + 1); the high band runs at N_S(q) = N_L(q)/4.
    int dualLatencyForQ (int order, int q)
    {
        return 1 << std::clamp (order + (q - 1), 1, order + 1);
    }

    // 18. depth=0 / mix=1: the summed dual-band output equals the test-side LR4
    // twin allpass delayed by N_L(q), at every Quality, within the 1e-9 absolute
    // spec tolerance (new spec value: absorbs the two band STFTs' reconstruction
    // error plus FMA/rounding differences across compilers; measured <= 4.5e-16
    // across all rates and qualities).
    void dualReconstructionTest (double Fs)
    {
        std::printf ("Dual reconstruction (LR4 twin + N_L delay, all quality) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        double worst = 0.0;
        for (int q = 0; q < 3; ++q)
        {
            const int nq = dualLatencyForQ (order, q);
            const int M = std::max (1 << 15, 8 * nq);
            std::mt19937 rng (7);
            std::uniform_real_distribution<double> u (-0.5, 0.5);
            std::vector<double> x ((size_t) M);
            for (auto& v : x) v = u (rng);
            const auto ap = lr4AllpassRef (x, Fs);

            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setQuality (q); m.setDepth (0.0); m.setMix (1.0);
            std::vector<double> y ((size_t) M);
            for (int n = 0; n < M; ++n)
            { double l = x[(size_t) n], r = x[(size_t) n]; m.process (l, r); y[(size_t) n] = l; }

            if (m.latencySamples() != nq)
                fail ("dual recon: latency " + std::to_string (m.latencySamples()) + " != "
                      + std::to_string (nq) + " (q=" + std::to_string (q) + ") at Fs=" + std::to_string (Fs));
            double e = 0.0;
            for (int n = M / 2; n < M; ++n) e = std::max (e, std::abs (y[(size_t) n] - ap[(size_t) (n - nq)]));
            if (e > 1.0e-9)
                fail ("dual recon: depth0 err vs LR4 twin " + std::to_string (e) + " (q=" + std::to_string (q)
                      + ") at Fs=" + std::to_string (Fs));
            worst = std::max (worst, e);
        }
        std::printf ("  maxErr=%.2e (spec 1e-9, all q)\n", worst);
    }

    // 18b. Phase 6 splitHzIn default-identity: prepare()'s additive audition hook
    // (splitHzIn, default 0.0 -> kSplitHz) must be bit-identical to the pre-hook
    // 3-arg prepare() call for every existing/default caller. Two composites --
    // one via the old 3-arg prepare(), one via the new 4-arg prepare() passed the
    // SAME 3000.0 Hz explicitly -- must produce EXACTLY the same output sample
    // for sample (not just within a tolerance), proving the hook is additive-only.
    // A third instance with a genuinely different splitHzIn (1500 Hz, the other
    // audition checkpoint value) is included only to prove the override actually
    // takes effect (splitHz() reports it, and depth=0 output measurably diverges
    // from the two 3 kHz instances) -- so this isn't a vacuous "always 3000" check.
    void dualSplitHzDefaultIdentityTest (double Fs)
    {
        std::printf ("Dual splitHzIn default-identity (Phase 6 audition hook) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int M = std::max (1 << 15, 8 * (1 << order));
        std::mt19937 rng (53);
        std::uniform_real_distribution<double> u (-0.5, 0.5);
        std::vector<double> x ((size_t) M);
        for (auto& v : x) v = u (rng);

        auto renderWithSplit = [&] (bool useDefaultOverload, double splitHzIn)
        {
            factory_core::MultiResSuppressor m;
            if (useDefaultOverload) m.prepare (Fs, order);                 // old 3-arg call site
            else                    m.prepare (Fs, order, 0, splitHzIn);   // new 4-arg call site
            m.setDepth (1.2); m.setSharpness (0.5); // engine fully active (not just depth=0 passthrough)
            std::vector<double> y ((size_t) M);
            for (int n = 0; n < M; ++n)
            { double l = x[(size_t) n], r = x[(size_t) n]; m.process (l, r); y[(size_t) n] = l; }
            return y;
        };

        if (std::abs (factory_core::MultiResSuppressor {}.splitHz() - 3000.0) > 0.0)
            fail ("default-constructed splitHz() != 3000.0 (spec constant) at Fs=" + std::to_string (Fs));

        const auto yOld        = renderWithSplit (true, 0.0);
        const auto yNewDefault = renderWithSplit (false, 3000.0);
        double diff = 0.0;
        for (int n = 0; n < M; ++n) diff = std::max (diff, std::abs (yOld[(size_t) n] - yNewDefault[(size_t) n]));
        if (diff != 0.0)
            fail ("splitHzIn=3000.0 (explicit) != legacy 3-arg prepare() (bit-exact expected), diff="
                  + std::to_string (diff) + " at Fs=" + std::to_string (Fs));

        // Non-vacuity: an actually-different split (1.5 kHz) must both report
        // differently and measurably change the depth-engaged output.
        factory_core::MultiResSuppressor probe;
        probe.prepare (Fs, order, 0, 1500.0);
        if (std::abs (probe.splitHz() - 1500.0) > 1.0e-9)
            fail ("splitHzIn=1500.0 not reflected by splitHz(): got " + std::to_string (probe.splitHz())
                  + " at Fs=" + std::to_string (Fs));
        const auto y1500 = renderWithSplit (false, 1500.0);
        double divergence = 0.0;
        for (int n = 0; n < M; ++n) divergence = std::max (divergence, std::abs (y1500[(size_t) n] - yNewDefault[(size_t) n]));
        if (divergence < 1.0e-6)
            fail ("splitHzIn=1500.0 did not measurably change the output vs 3000.0 (vacuous override) at Fs="
                  + std::to_string (Fs));

        std::printf ("  bitExactVsLegacy=%.2e (expect 0)  divergenceAt1500Hz=%.2e (expect > 1e-6)\n", diff, divergence);
    }

    // 19. Dual Delta/Mix identity, the composite version of deltaMixIdentityTest.
    // The composite's dry reference is allpass(in) delayed N_L (its own LR4 sum
    // ring); the test rebuilds it independently (twin LR4 + plain delay). Gates:
    //  (a) depth-0 delta = the two-band reconstruction residual, peak <= 1e-9
    //      over the steady state (n >= 2N);
    //  (b) complementarity: out_normal + out_delta == twin dry to 1e-12 relative
    //      at mix in {0, 0.3, 0.8, 1} (delta = dryRef - out is one IEEE rounding
    //      from exact; the twin dry is bit-identical arithmetic to the ring);
    //  (c) affine identity: delta(m) == m*delta(1) + (1-m)*delta(0) to 1e-12
    //      relative (gEff is affine in Mix per band and the band sum is linear).
    // Measured across rates: (a) <= 6.7e-16, (b) <= 1.2e-16, (c) <= 4.5e-16.
    void dualDeltaTest (double Fs)
    {
        std::printf ("Dual delta (residual + complementarity + affine) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;
        const int M = std::max (1 << 14, 4 * N);
        std::mt19937 rng (41);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 1500.0 * n / Fs);
        const auto ap = lr4AllpassRef (x, Fs);

        auto cfg = [] (factory_core::MultiResSuppressor& s) { s.setDepth (1.2); s.setSharpness (0.5); };

        auto renderDelta = [&] (double m)
        {
            factory_core::MultiResSuppressor d;
            d.prepare (Fs, order); cfg (d); d.setDelta (true); d.setMix (m);
            std::vector<double> out ((size_t) M);
            for (int n = 0; n < M; ++n)
            { double l = x[(size_t) n], r = x[(size_t) n]; d.process (l, r); out[(size_t) n] = l; }
            return out;
        };
        const auto delta1 = renderDelta (1.0);
        const auto delta0 = renderDelta (0.0);

        if (factory_core::testing::peakAbs (delta1) < 1.0e-3)
            fail ("dual delta reference carries no removed signal (vacuous gate) at Fs=" + std::to_string (Fs));

        // (a) depth-0 delta residual over the steady state.
        double recon0 = 0.0;
        for (int n = 2 * N; n < M; ++n) recon0 = std::max (recon0, std::abs (delta0[(size_t) n]));
        if (recon0 > 1.0e-9)
            fail ("dual delta(0) reconstruction residual too large: " + std::to_string (recon0)
                  + " (spec 1e-9) at Fs=" + std::to_string (Fs));

        double maxRel = 0.0, maxAffine = 0.0;
        for (double m : { 0.0, 0.3, 0.8, 1.0 })
        {
            factory_core::MultiResSuppressor nrm, del;
            nrm.prepare (Fs, order); cfg (nrm); nrm.setMix (m);
            del.prepare (Fs, order); cfg (del); del.setMix (m); del.setDelta (true);

            bool badSum = false, badAffine = false;
            for (int n = 0; n < M; ++n)
            {
                double ln = x[(size_t) n], rn = x[(size_t) n];
                double ld = x[(size_t) n], rd = x[(size_t) n];
                nrm.process (ln, rn);
                del.process (ld, rd);

                const double dryRef = (n >= N) ? ap[(size_t) (n - N)] : 0.0; // first N: ring zeros
                const double scale = std::max (1.0, std::abs (dryRef));

                // (b) complementarity vs the test-side twin dry.
                const double res = std::max (std::abs (ln + ld - dryRef), std::abs (rn + rd - dryRef));
                maxRel = std::max (maxRel, res / scale);
                if (! badSum && res > 1.0e-12 * scale)
                {
                    fail ("dual normal+delta != twin dry at n=" + std::to_string (n) + " mix="
                          + std::to_string (m) + " Fs=" + std::to_string (Fs));
                    badSum = true;
                }
                // (c) affine identity.
                const double affineRef = m * delta1[(size_t) n] + (1.0 - m) * delta0[(size_t) n];
                const double affRes = std::max (std::abs (ld - affineRef), std::abs (rd - affineRef));
                maxAffine = std::max (maxAffine, affRes / scale);
                if (! badAffine && affRes > 1.0e-12 * scale)
                {
                    fail ("dual delta(mix) != m*delta(1)+(1-m)*delta(0) at n=" + std::to_string (n)
                          + " mix=" + std::to_string (m) + " Fs=" + std::to_string (Fs));
                    badAffine = true;
                }
            }
        }
        std::printf ("  recon0=%.2e complementarityRel=%.2e affineRel=%.2e (spec 1e-9 / 1e-12 / 1e-12)\n",
                     recon0, maxRel, maxAffine);
    }

    // 20. Dual bypass. Full bypass reads the raw-input delay ring with no
    // arithmetic, so a from-startup-bypassed composite is bit-transparent to
    // in[n-N_L] (NOT the allpass reference -- bypass must be the untouched
    // input). The toggle twin mirrors bypassToggleTest: bypass only gates the
    // composite output stage (both sub-engines and all rings keep running), so
    // after the release ramp the twin returns to a bit-exact match.
    void dualBypassTest (double Fs)
    {
        std::printf ("Dual bypass (bit transparency + toggle twin) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;

        // (i) bit transparency from startup, engine fully active underneath.
        {
            const int M = std::max (1 << 14, 4 * N);
            std::mt19937 rng (29);
            std::normal_distribution<double> g (0.0, 0.3);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.4 * std::sin (2.0 * kPi * 1200.0 * n / Fs);

            factory_core::MultiResSuppressor s;
            s.prepare (Fs, order);
            s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.7);
            s.setBypassed (true);
            s.reset(); // snap the crossfade to the bypassed end

            for (int n = 0; n < M; ++n)
            {
                double l = x[(size_t) n], r = x[(size_t) n];
                s.process (l, r);
                const double expected = (n >= N) ? x[(size_t) (n - N)] : 0.0;
                if (l != expected || r != expected)
                { fail ("dual bypass not bit-transparent at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); break; }
            }
        }

        // (ii) toggle twin: A active, B bypassed then released.
        {
            const double kRampSec = 0.010;                           // engine's bypass ramp (spec constant)
            const int settle = (int) std::ceil (kRampSec * Fs) + 16; // safely past the full ramp
            const int tOn  = 4 * N;
            const int tOff = tOn + settle + 4 * N;
            const int M    = tOff + settle + 4 * N + 256;

            std::mt19937 rng (31);
            std::normal_distribution<double> g (0.0, 0.25);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 2000.0 * n / Fs);

            auto cfg = [] (factory_core::MultiResSuppressor& s)
            { s.setDepth (1.2); s.setSharpness (0.5); s.setMix (0.8); };
            factory_core::MultiResSuppressor A, B;
            A.prepare (Fs, order); cfg (A); A.reset();
            B.prepare (Fs, order); cfg (B); B.reset();

            std::vector<double> a ((size_t) M), b ((size_t) M);
            bool boundBad = false;
            for (int n = 0; n < M; ++n)
            {
                if (n == tOn)  B.setBypassed (true);
                if (n == tOff) B.setBypassed (false);
                double la = x[(size_t) n], ra = x[(size_t) n];
                double lb = x[(size_t) n], rb = x[(size_t) n];
                A.process (la, ra);
                B.process (lb, rb);
                a[(size_t) n] = la; b[(size_t) n] = lb;

                // Convex-blend bound: B lies between the active output and the
                // aligned raw dry (+eps slack for ties at the ends).
                const double dryAligned = (n >= N) ? x[(size_t) (n - N)] : 0.0;
                const double bound = std::max (std::abs (dryAligned), std::abs (la)) + 1.0e-12;
                if (! boundBad && (std::abs (lb) > bound || std::abs (rb) > bound))
                { fail ("dual bypass blend exceeded convex bound at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); boundBad = true; }
            }

            if (! factory_core::testing::allFinite (a) || ! factory_core::testing::allFinite (b))
                fail ("dual bypass toggle produced non-finite output at Fs=" + std::to_string (Fs));

            for (int n = tOn + settle; n < tOff; ++n)
                if (b[(size_t) n] != x[(size_t) (n - N)])
                { fail ("dual bypassed B != aligned dry at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); break; }

            for (int n = tOff + settle; n < M; ++n)
                if (b[(size_t) n] != a[(size_t) n])
                { fail ("dual post-release B != A at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); break; }

            std::printf ("  ok (N=%d settle=%d tOn=%d tOff=%d M=%d)\n", N, settle, tOn, tOff, M);
        }
    }

    // 21. Dual speed -- the property the split exists for. A tone burst scaled
    // to each path's own window (B = N_sub/4) is fed over silence with
    // near-instant ballistics (setTimes(0.01, 0.01): the one-pole coefficient
    // underflows to 0, so the per-bin gain IS each frame's target and no
    // release tau contaminates the measurement). The burst is "visible" to a
    // window of length N_sub for N_sub + B samples, so the suppressed span of
    // the sub-engine's gain at the tone bin is ~ N_sub + B minus a few
    // threshold-edge frames (windows barely overlapping the burst fall under
    // the detection knee), quantised to the sub-engine hop H_sub = N_sub/8.
    // The span is INVARIANT to the high path's constant pre-delay (a delay
    // shifts first/last equally), so span_high/span_low measures the high
    // band's 4x frame speed directly: with B_sub proportional to N_sub the
    // oracle ratio is (N_S + B_S)/(N_L + B_L) = N_S/N_L = 1/4. Everything is
    // deterministic (no rng). Measured at all six rates: span_high exactly
    // 1.125*N_S, span_low exactly 0.875*N_L (the 500 Hz tone sheds more edge
    // frames -- its detection margin is slimmer), ratio 0.3214, worst in-burst
    // reduction <= -16 dB. Gates fixed with margin: minRed <= -10 dB both;
    // span_high in [0.75*N_S, 1.5*N_S]; span_low in [0.6*N_L, 1.5*N_L];
    // direction span_high <= span_low/2; ratio in [0.18, 0.42]. A high band
    // secretly running at the low resolution would give ratio ~1.25 and a
    // one-order-short window ~0.64 -- both fail hard.
    void dualSpeedTest (double Fs)
    {
        std::printf ("Dual speed (burst span, high band 4x frames) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int NL = 1 << order, NS = NL / 4;

        struct Span { int first = -1, last = -1; double minRed = 0.0; };
        auto burstSpan = [&] (bool isHigh, double toneHz) -> Span
        {
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setDepth (1.2); m.setSharpness (0.5); m.setTimes (0.01, 0.01);

            const auto& sub = isHigh ? m.highEngine() : m.lowEngine();
            const int Nsub = sub.latencySamples();
            const int B = Nsub / 4;
            const int start = 4 * NL;
            const int M = start + B + 6 * NL;
            const int kSub = std::max (1, (int) std::round (toneHz * (double) Nsub / Fs));
            const double f0 = (double) kSub * Fs / (double) Nsub; // aligned on the sub grid

            Span sp;
            for (int n = 0; n < M; ++n)
            {
                const bool on = (n >= start && n < start + B);
                double l = on ? 0.5 * std::sin (2.0 * kPi * f0 * (n - start) / Fs) : 0.0;
                double r = l;
                m.process (l, r);
                const double red = sub.reductionDb()[(size_t) kSub];
                if (red < -6.0) { if (sp.first < 0) sp.first = n; sp.last = n; }
                sp.minRed = std::min (sp.minRed, red);
            }
            return sp;
        };

        const Span hi = burstSpan (true, 8000.0);
        const Span lo = burstSpan (false, 500.0);
        if (hi.minRed > -10.0 || lo.minRed > -10.0 || hi.first < 0 || lo.first < 0)
        {
            fail ("dual speed: burst not meaningfully suppressed (minRedHi=" + std::to_string (hi.minRed)
                  + " minRedLo=" + std::to_string (lo.minRed) + ") at Fs=" + std::to_string (Fs));
            return;
        }
        const int spanHi = hi.last - hi.first + 1;
        const int spanLo = lo.last - lo.first + 1;
        const double ratio = (double) spanHi / (double) spanLo;

        if (spanHi < (3 * NS) / 4 || spanHi > (3 * NS) / 2)
            fail ("dual speed: high span " + std::to_string (spanHi) + " outside [0.75*N_S, 1.5*N_S] (N_S="
                  + std::to_string (NS) + ") at Fs=" + std::to_string (Fs));
        if (spanLo < (3 * NL) / 5 || spanLo > (3 * NL) / 2)
            fail ("dual speed: low span " + std::to_string (spanLo) + " outside [0.6*N_L, 1.5*N_L] (N_L="
                  + std::to_string (NL) + ") at Fs=" + std::to_string (Fs));
        if (! (2 * spanHi <= spanLo))
            fail ("dual speed: high span not < half the low span (spanHi=" + std::to_string (spanHi)
                  + " spanLo=" + std::to_string (spanLo) + ") at Fs=" + std::to_string (Fs));
        if (ratio < 0.18 || ratio > 0.42)
            fail ("dual speed: span ratio " + std::to_string (ratio) + " outside [0.18, 0.42] at Fs="
                  + std::to_string (Fs));
        std::printf ("  spanHi=%d (N_S=%d) spanLo=%d (N_L=%d) ratio=%.3f minRed=(%.1f, %.1f) dB\n",
                     spanHi, NS, spanLo, NL, ratio, hi.minRed, lo.minRed);
    }

    // 22. Dual crossover continuity: the same resonant tone+noise recipe just
    // below (2.5 kHz) and just above (3.6 kHz) the 3 kHz split must be
    // suppressed on both sides of the crossover and by a comparable amount.
    // Near the split each band's detector sees an LR4-rolled-off copy of the
    // tone and a rolled-off noise floor, so the excess -- and Soft-mode cut --
    // survives, but a small edge bias remains (a known simplification: the
    // envelopes are band-limited, not full-band). Measured across the six
    // rates: 2.5k -19.6..-23.2 dB, 3.6k -20.0..-24.5 dB, |diff| <= 1.83 dB.
    // Spec gates with margin: both <= -15 dB, |diff| <= 4 dB.
    void dualCrossoverTest (double Fs)
    {
        std::printf ("Dual crossover (2.5k vs 3.6k continuity) @ Fs=%.0f\n", Fs);
        auto suppress = [&] (double f0)
        {
            const int M = 1 << 15;
            std::mt19937 rng (5);
            std::normal_distribution<double> g (0.0, 0.1);
            std::vector<double> x ((size_t) M);
            for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * f0 * n / Fs);

            auto run = [&] (double depth)
            {
                factory_core::MultiResSuppressor m;
                m.prepare (Fs, orderFor (Fs));
                m.setDepth (depth); m.setSharpness (0.5);
                std::vector<double> y ((size_t) M);
                for (int n = 0; n < M; ++n)
                { double l = x[(size_t) n], r = x[(size_t) n]; m.process (l, r); y[(size_t) n] = 0.5 * (l + r); }
                return y;
            };
            const auto dry = run (0.0);
            const auto wet = run (1.2);
            const int a = M / 2, b = M;
            return 20.0 * std::log10 (magAt (wet, a, b, f0, Fs) / magAt (dry, a, b, f0, Fs));
        };

        const double dBelow = suppress (alignedFreq (Fs, 2500.0));
        const double dAbove = suppress (alignedFreq (Fs, 3600.0));
        if (dBelow > -15.0)
            fail ("dual crossover: below-split resonance under-suppressed: " + std::to_string (dBelow)
                  + " dB at Fs=" + std::to_string (Fs));
        if (dAbove > -15.0)
            fail ("dual crossover: above-split resonance under-suppressed: " + std::to_string (dAbove)
                  + " dB at Fs=" + std::to_string (Fs));
        if (std::abs (dBelow - dAbove) > 4.0)
            fail ("dual crossover: suppression discontinuity across the split: below "
                  + std::to_string (dBelow) + " dB vs above " + std::to_string (dAbove)
                  + " dB at Fs=" + std::to_string (Fs));
        std::printf ("  2.5k %.2f dB  3.6k %.2f dB  |diff| %.2f dB (spec <= -15 / <= 4)\n",
                     dBelow, dAbove, std::abs (dBelow - dAbove));
    }

    // 23. Dual quality: (a) the latency table and the structural N_S = N_L/4 at
    // every Quality (depth-0 reconstruction per quality is gated in
    // dualReconstructionTest); (b) a mid-stream Normal->Fast->High switch stays
    // finite and peak-bounded with depth engaged, and with depth 0 the output
    // returns to the twin reconstruction (<= 1e-9, measured 4.5e-16) once the
    // switches settle. The two sub-engines swap at their OWN frame boundaries
    // (up to a low hop apart); between and shortly after those boundaries the
    // high pre-delay ring briefly carries mixed-delay history, so the settle
    // window counts from the LAST event among {request, low latency change,
    // high latency change} -- checking inside that window would gate the
    // documented (finite, bounded) transient, not a regression.
    void dualQualityTest (double Fs)
    {
        std::printf ("Dual quality (latency table + mid-stream switch) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int bufLen = 1 << (order + 1);

        // (a) latency table + band ratio, per quality (switch applies at the
        // first frame boundary; a short zero-feed is enough to latch it).
        for (int q = 0; q < 3; ++q)
        {
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setQuality (q);
            for (int n = 0; n < 2 * bufLen; ++n) { double l = 0.0, r = 0.0; m.process (l, r); }
            const int nq = dualLatencyForQ (order, q);
            if (m.latencySamples() != nq)
                fail ("dual quality " + std::to_string (q) + ": latency " + std::to_string (m.latencySamples())
                      + " != " + std::to_string (nq) + " at Fs=" + std::to_string (Fs));
            if (m.highEngine().latencySamples() != nq / 4)
                fail ("dual quality " + std::to_string (q) + ": N_S " + std::to_string (m.highEngine().latencySamples())
                      + " != N_L/4 = " + std::to_string (nq / 4) + " at Fs=" + std::to_string (Fs));
        }

        const int settle = bufLen + (int) std::ceil (0.010 * Fs) + 512;
        const int t1 = 2 * bufLen, t2 = 6 * bufLen, M = 10 * bufLen;
        std::mt19937 rng (97);
        std::normal_distribution<double> g (0.0, 0.25);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n) x[(size_t) n] = g (rng) + 0.5 * std::sin (2.0 * kPi * 2000.0 * n / Fs);
        const auto ap = lr4AllpassRef (x, Fs);

        // (b1) depth engaged: finite everywhere, realistic peak bound through
        // both switches (the composite applies no output mask -- the per-band
        // holds and the live pre-delay keep it aligned and bounded).
        {
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setDepth (1.2); m.setSharpness (0.5); m.setMix (0.9);
            std::vector<double> y ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                if (n == t1) m.setQuality (0);
                if (n == t2) m.setQuality (2);
                double l = x[(size_t) n], r = x[(size_t) n];
                m.process (l, r);
                y[(size_t) n] = l;
            }
            if (! factory_core::testing::allFinite (y))
                fail ("dual quality mid-switch produced non-finite output at Fs=" + std::to_string (Fs));
            const double peak = factory_core::testing::peakAbs (y);
            if (peak > 4.0)
                fail ("dual quality mid-switch peak " + std::to_string (peak)
                      + " beyond realistic bound at Fs=" + std::to_string (Fs));
        }

        // (b2) depth 0: settled reconstruction across the switches.
        {
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setDepth (0.0); m.setMix (1.0);
            std::vector<double> y ((size_t) M);
            std::vector<int> latL ((size_t) M), latS ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                if (n == t1) m.setQuality (0);
                if (n == t2) m.setQuality (2);
                double l = x[(size_t) n], r = x[(size_t) n];
                m.process (l, r);
                y[(size_t) n]    = l;
                latL[(size_t) n] = m.lowEngine().latencySamples();
                latS[(size_t) n] = m.highEngine().latencySamples();
            }

            int lastEvent = 0, curL = latL[0], curS = latS[0], checked = 0;
            double e = 0.0;
            for (int n = 0; n < M; ++n)
            {
                if (n == t1 || n == t2) lastEvent = n;
                if (latL[(size_t) n] != curL) { curL = latL[(size_t) n]; lastEvent = n; }
                if (latS[(size_t) n] != curS) { curS = latS[(size_t) n]; lastEvent = n; }
                if (n - lastEvent > settle && n >= curL + settle)
                {
                    e = std::max (e, std::abs (y[(size_t) n] - ap[(size_t) (n - curL)]));
                    ++checked;
                }
            }
            if (checked < bufLen) // guard: the settled-region check must not be vacuous
                fail ("dual quality mid-switch: depth0 check vacuous (checked=" + std::to_string (checked)
                      + ") at Fs=" + std::to_string (Fs));
            if (e > 1.0e-9)
                fail ("dual quality mid-switch: settled depth0 residual " + std::to_string (e)
                      + " at Fs=" + std::to_string (Fs));
            std::printf ("  mid-switch settledErr=%.2e checked=%d (t1=%d t2=%d M=%d)\n", e, checked, t1, t2, M);
        }
    }

    // 24. Dual display merge. The composite reports the LOW engine's grid;
    // below the split its curves are the low engine's verbatim (same snapshot,
    // asserted exact), at/above the split they are the high engine's values
    // resampled by linear interpolation in dB along log-frequency -- rebuilt
    // here independently (own bin->Hz maths, log2-based interpolation weights)
    // and compared to 1e-9 dB. Non-vacuity: the 8 kHz resonance produces a
    // real merged cut (<= -1 dB) and the merged magnitude genuinely differs
    // from the low engine's own above-split bins (the two windows see
    // different noise averages).
    void dualDisplayTest (double Fs)
    {
        std::printf ("Dual display merge (low grid + log-f dB interp) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int NL = 1 << order, NS = NL / 4;
        const int M = 1 << 15;

        // 8 kHz aligned on the HIGH grid (hence also on the low grid: N_L = 4*N_S).
        const int k8s = (int) std::round (8000.0 * (double) NS / Fs);
        const double f8 = (double) k8s * Fs / (double) NS;
        const double f1 = alignedFreq (Fs, 1000.0);

        std::mt19937 rng (131);
        std::normal_distribution<double> g (0.0, 0.15);
        std::vector<double> x ((size_t) M);
        for (int n = 0; n < M; ++n)
            x[(size_t) n] = g (rng) + 0.4 * std::sin (2.0 * kPi * f8 * n / Fs)
                                    + 0.4 * std::sin (2.0 * kPi * f1 * n / Fs);

        factory_core::MultiResSuppressor m;
        m.prepare (Fs, order);
        m.setDepth (1.2); m.setSharpness (0.5);
        for (int n = 0; n < M; ++n) { double l = x[(size_t) n], r = x[(size_t) n]; m.process (l, r); }

        const int nbLow  = m.numBins();
        const int nbHigh = m.highEngine().numBins();
        std::vector<double> mag ((size_t) nbLow), red ((size_t) nbLow);
        m.magnitudeDb (mag.data());
        m.reductionDb (red.data());

        // Sub-engine snapshots (the engine is idle now, so these are the same
        // frames the composite merged).
        std::vector<double> lowMag ((size_t) nbLow), highMag ((size_t) nbHigh);
        m.lowEngine().magnitudeDb (lowMag.data());
        m.highEngine().magnitudeDb (highMag.data());
        const double* lowRed  = m.lowEngine().reductionDb();
        const double* highRed = m.highEngine().reductionDb();

        // Independent merge oracle: own bin->Hz maths and log2-based weights.
        auto mergeOracle = [&] (const double* lowArr, const double* highArr, int k) -> double
        {
            const double f = (double) k * Fs / (double) NL;
            if (f < 3000.0) return lowArr[k];
            const double binHzHigh = Fs / (double) NS;
            int j0 = (int) std::floor (f / binHzHigh);
            j0 = std::clamp (j0, 1, nbHigh - 1);
            const int j1 = std::min (j0 + 1, nbHigh - 1);
            if (j1 == j0) return highArr[j0];
            const double lf  = std::log2 (f);
            const double lf0 = std::log2 ((double) j0 * binHzHigh);
            const double lf1 = std::log2 ((double) j1 * binHzHigh);
            const double t   = (lf - lf0) / (lf1 - lf0);
            return highArr[j0] + t * (highArr[j1] - highArr[j0]);
        };

        double worstMag = 0.0, worstRed = 0.0, maxAboveDiff = 0.0;
        bool exactBad = false;
        for (int k = 0; k < nbLow; ++k)
        {
            const double f = (double) k * Fs / (double) NL;
            const double em = mergeOracle (lowMag.data(), highMag.data(), k);
            const double er = mergeOracle (lowRed, highRed, k);
            worstMag = std::max (worstMag, std::abs (mag[(size_t) k] - em));
            worstRed = std::max (worstRed, std::abs (red[(size_t) k] - er));
            if (f < 3000.0)
            {
                // Below the split the merge must be the low engine verbatim.
                if (! exactBad && (mag[(size_t) k] != lowMag[(size_t) k] || red[(size_t) k] != lowRed[k]))
                { fail ("dual display: below-split bin " + std::to_string (k)
                        + " != low engine value at Fs=" + std::to_string (Fs)); exactBad = true; }
            }
            else
                maxAboveDiff = std::max (maxAboveDiff, std::abs (mag[(size_t) k] - lowMag[(size_t) k]));
        }
        if (worstMag > 1.0e-9)
            fail ("dual display: magnitude merge != oracle by " + std::to_string (worstMag)
                  + " dB at Fs=" + std::to_string (Fs));
        if (worstRed > 1.0e-9)
            fail ("dual display: reduction merge != oracle by " + std::to_string (worstRed)
                  + " dB at Fs=" + std::to_string (Fs));

        // Non-vacuity: the 8 kHz cut shows up in the merged curve, and the
        // merged magnitude above the split is not just the low engine's bins.
        const int k8l = 4 * k8s; // same frequency on the low grid
        if (red[(size_t) k8l] > -1.0)
            fail ("dual display: merged reduction at 8 kHz missing (" + std::to_string (red[(size_t) k8l])
                  + " dB) at Fs=" + std::to_string (Fs));
        if (maxAboveDiff < 0.1)
            fail ("dual display: merge indistinguishable from the low engine above the split (vacuous) at Fs="
                  + std::to_string (Fs));
        std::printf ("  oracleErr mag=%.2e red=%.2e  red[8k]=%.2f dB  maxAboveDiff=%.2f dB\n",
                     worstMag, worstRed, red[(size_t) k8l], maxAboveDiff);
    }

    // ---- Phase 6: display-only temporal smoothing (audition/DEV) ------------

    // 30. Display smoothing (setDisplaySmoothingMs) must be perfectly
    // audio-transparent: it only low-passes the analyser snapshot that
    // magnitudeDb()/magnitudePreDb() report (dispMag/dispMagPre in
    // ResonanceSuppressor, forwarded to both MultiResSuppressor sub-engines),
    // and must never alter the suppression/detection DSP or the output audio.
    // By construction (ResonanceSuppressor::processFrame): the raw per-frame
    // value r = max(mag0*g0Eff, mag1*g1Eff)/(0.5*N) is computed from mag[]/
    // gain[], which are populated BEFORE the dispSmoothMs branch and never
    // read back from dispMag/dispMagState -- so r is bit-identical whether or
    // not smoothing is on; only what gets WRITTEN to dispMag differs (r itself
    // vs. a running one-pole of r). Twin composites (A: off/0 ms, B: on/50 ms),
    // identical prepare/depth/sharpness/near-instant ballistics and identical
    // input, gate three formula-independent invariants:
    //  (i)   Audio transparency: A and B's output audio is bit-identical (exact
    //        equality) at EVERY sample of a run spanning a silence->tone onset
    //        AND the ensuing steady state.
    //  (ii)  Non-vacuity (the feature does something), measured RELATIVE to the
    //        tone's actual arrival so it never depends on the exact delay
    //        arithmetic: read directly off the HIGH sub-engine (bypassing the
    //        low/high display-grid merge, the same accessor dualSpeedTest/
    //        dualDisplayTest use) at an airband tone bin-aligned on its own
    //        grid, with near-instant ballistics (setTimes(0.01, 0.01), the same
    //        "gain == this frame's target" trick engineReductionSnapshot uses)
    //        so the per-bin gain carries no ballistics-timescale memory of its
    //        own. Scan forward for the FIRST sample the UNSMOOTHED A's
    //        magnitudeDb() at that bin rises clearly off the analyser floor
    //        (> kArrivalDb). A and B share frame timing exactly (identical
    //        input/config; display smoothing changes only WHAT is written to
    //        dispMag, not WHEN), so at that same sample B's one-pole reflects
    //        the same history -- still dominated by the long near-silent stretch
    //        before the tone reaches the high band -- and must sit measurably
    //        BELOW A. A separate gate asserts A does arrive within a generous
    //        budget, so the check can never be silently skipped.
    //  (iii) Steady-state convergence: ~10 time constants past guaranteed
    //        arrival, B's one-pole has relaxed onto the (now-constant) raw
    //        value and must match A within a small dB tolerance.
    // Timing (spec constants, independent of the engine internals): the tone
    // reaches the HIGH sub-engine only after the high-band pre-delay
    // D = N_L - N_S (MultiResSuppressor's highDelay ring) plus the high engine's
    // own window fill N_S -- ~(N_L + N_S)/H_S frames after onset. Invariant (ii)
    // is arrival-relative, so this figure only sizes the "A must arrive" budget
    // (arrivalBudgetFrames = ceil((N_L+N_S)/H_S) + 2*fillFrames, fillFrames =
    // ceil(N_S/H_S) = 8 at the default 8x Normal quality). n63Frames =
    // kDispSmoothMs*(Fs/H_S)/1000 is the one-pole's frame-domain time constant
    // (H_S/Fs is the high engine's frame interval, so this is exactly the
    // coefficient's tau in frames -- see setDisplaySmoothingMs's own doc).
    // Across 44.1-192 kHz H_S/Fs stays narrow (the STFT order tracks Fs so the
    // window length in ms is ~constant -- see resolutionTest), so n63Frames
    // stays roughly in [27, 53]. At A's first arrival B is many tau behind
    // (still integrating mostly floor), so the A-vs-B gap is tens of dB and
    // kLagEpsilonDb (4 dB) has huge margin. Check (iii) waits ceil(10*n63Frames)
    // frames past the arrival budget (retention c^n ~ e^-10 ~= 4.5e-5 in the
    // linear domain, an estimated ~4e-4 dB residual), so kConvergeTolDb
    // (0.05 dB) has ~100x margin.
    void displaySmoothingIsAudioTransparentTest (double Fs)
    {
        std::printf ("Display smoothing (audio-transparent, lag-then-converge) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int NL = 1 << order;

        const double kDispSmoothMs  = 50.0;  // audition tau under test
        const double kArrivalDb     = -60.0; // invariant (ii): "A risen off the floor" threshold (floor ~-240, steady ~-36 dB)
        const double kLagEpsilonDb  = 4.0;   // invariant (ii) margin (B typically tens of dB lower at A's first arrival)
        const double kConvergeTolDb = 0.05;  // invariant (iii) tolerance (estimated residual ~4e-4 dB)

        // Near-instant ballistics (matches engineReductionSnapshot's trick):
        // the per-bin gain coefficient underflows to 0, so gain == this
        // frame's own target with no cross-frame ballistics memory -- the only
        // remaining time-domain effect on the raw r sequence is the analysis
        // window itself filling with the tone, isolating (ii)/(iii) to the
        // display smoothing alone.
        auto cfg = [] (factory_core::MultiResSuppressor& s)
        { s.setDepth (1.2); s.setSharpness (0.5); s.setTimes (0.01, 0.01); };

        factory_core::MultiResSuppressor A, B; // A: smoothing off, B: smoothing on
        A.prepare (Fs, order); cfg (A); A.setDisplaySmoothingMs (0.0);
        B.prepare (Fs, order); cfg (B); B.setDisplaySmoothingMs (kDispSmoothMs);

        const int NS = A.highEngine().latencySamples();
        const int HS = A.highEngine().hopSamples();

        // Airband tone bin-aligned on the HIGH engine's own FFT grid (no
        // scalloping loss), same technique as dualSpeedTest/dualDisplayTest.
        const int kSub = std::max (1, (int) std::round (8000.0 * (double) NS / Fs));
        const double f0 = (double) kSub * Fs / (double) NS;

        // Onset aligned to a whole H_S hop (cosmetic; the arrival scan below is
        // index-based, not tied to an absolute frame count -- see (ii)).
        const int onset = ((4 * NL + HS - 1) / HS) * HS;

        const int fillFrames = (NS + HS - 1) / HS; // hops spanning one high-engine analysis window (== 8, Normal quality)
        const double n63Frames = (kDispSmoothMs * 1.0e-3) * (Fs / (double) HS); // one-pole tau, in high-engine frames

        // The tone reaches the HIGH sub-engine only after the high-band pre-delay
        // D = N_L - N_S (MultiResSuppressor's highDelay ring) PLUS the high
        // engine's own window fill N_S -- ~(N_L + N_S)/H_S frames after onset.
        // Invariant (ii) is measured RELATIVE to A's actual first arrival (found
        // in the loop), so it never depends on this figure; the budget below only
        // guards "A must arrive" so the check can never be silently skipped, and
        // the convergence point (iii) sits ~10 tau AFTER guaranteed arrival.
        const int arrivalBudgetFrames = (NL + NS + HS - 1) / HS + 2 * fillFrames;
        const int checkFrames3        = arrivalBudgetFrames + (int) std::ceil (10.0 * n63Frames);
        const int arrivalBudgetSample = onset + arrivalBudgetFrames * HS;
        const int checkSample3        = onset + checkFrames3 * HS;
        const int M = checkSample3;

        std::vector<double> x ((size_t) M, 0.0);
        for (int n = onset; n < M; ++n) x[(size_t) n] = 0.5 * std::sin (2.0 * kPi * f0 * (n - onset) / Fs);

        const int nbHigh = A.highEngine().numBins();
        std::vector<double> magA ((size_t) nbHigh), magB ((size_t) nbHigh);
        double snap2A = 0.0, snap2B = 0.0, snap3A = 0.0, snap3B = 0.0;
        int arriveSample = -1;
        bool audioBad = false;

        for (int n = 0; n < M; ++n)
        {
            double la = x[(size_t) n], ra = x[(size_t) n];
            double lb = x[(size_t) n], rb = x[(size_t) n];
            A.process (la, ra);
            B.process (lb, rb);

            // (i) Audio transparency: exact equality, every sample.
            if (! audioBad && (la != lb || ra != rb))
            {
                fail ("display smoothing altered the output audio at n=" + std::to_string (n)
                      + " (dL=" + std::to_string (la - lb) + " dR=" + std::to_string (ra - rb)
                      + ") at Fs=" + std::to_string (Fs));
                audioBad = true;
            }

            // (ii) Arrival-relative lag: capture the FIRST sample the unsmoothed
            // A rises clearly off the analyser floor at the test bin. A and B
            // share frame timing exactly (identical input/config; display
            // smoothing changes only WHAT is written to dispMag, not WHEN), so
            // B's value at this same sample is its one-pole over the same history
            // -- still dominated by the long near-silent stretch before the tone
            // reaches the high band, hence far lower than A.
            if (arriveSample < 0 && n >= onset)
            {
                A.highEngine().magnitudeDb (magA.data());
                if (magA[(size_t) kSub] > kArrivalDb)
                {
                    B.highEngine().magnitudeDb (magB.data());
                    snap2A = magA[(size_t) kSub];
                    snap2B = magB[(size_t) kSub];
                    arriveSample = n;
                }
            }

            if (n == checkSample3 - 1)
            {
                A.highEngine().magnitudeDb (magA.data());
                B.highEngine().magnitudeDb (magB.data());
                snap3A = magA[(size_t) kSub]; snap3B = magB[(size_t) kSub];
            }
        }

        // Non-vacuity: the tone must actually register (well above the
        // analyser floor) and be genuinely suppressed (depth=1.2 active), or
        // the gates below would pass trivially on near-silence.
        if (snap3A < -40.0)
            fail ("display smoothing: steady-state tone magnitude implausibly low ("
                  + std::to_string (snap3A) + " dB) -- test signal not registering at Fs=" + std::to_string (Fs));
        const double redAtSteady = A.highEngine().reductionDb()[(size_t) kSub];
        if (redAtSteady > -3.0)
            fail ("display smoothing: no meaningful suppression active at the test bin ("
                  + std::to_string (redAtSteady) + " dB) -- invariant (i) would be vacuous at Fs=" + std::to_string (Fs));

        // (ii) A must arrive within the budget (so the check is never silently
        // skipped) and, at that first-arrival sample, B must sit measurably below it.
        if (arriveSample < 0 || arriveSample > arrivalBudgetSample)
            fail ("display smoothing: unsmoothed A never rose off the floor (> "
                  + std::to_string (kArrivalDb) + " dB) within the arrival budget ("
                  + std::to_string (arrivalBudgetFrames) + " frames): arriveSample="
                  + std::to_string (arriveSample) + " budgetSample=" + std::to_string (arrivalBudgetSample)
                  + " at Fs=" + std::to_string (Fs));
        else if (snap2B > snap2A - kLagEpsilonDb)
            fail ("display smoothing: no measurable lag at A's first arrival (sample "
                  + std::to_string (arriveSample) + "): A=" + std::to_string (snap2A) + " dB B="
                  + std::to_string (snap2B) + " dB (want B <= A-" + std::to_string (kLagEpsilonDb)
                  + ") at Fs=" + std::to_string (Fs));

        // (iii) B must converge to A in the steady state.
        if (std::abs (snap3B - snap3A) > kConvergeTolDb)
            fail ("display smoothing: did not converge to the unsmoothed value after " + std::to_string (checkFrames3)
                  + " frames: A=" + std::to_string (snap3A) + " dB B=" + std::to_string (snap3B)
                  + " dB (tol " + std::to_string (kConvergeTolDb) + ") at Fs=" + std::to_string (Fs));

        std::printf ("  arrive@%d (budget %d) n63Frames=%.1f  lag(A=%.2f,B=%.2f,gap=%.2f dB)  converge(A=%.3f,B=%.3f,err=%.2e dB)\n",
                     arriveSample, arrivalBudgetSample, n63Frames, snap2A, snap2B, snap2A - snap2B,
                     snap3A, snap3B, std::abs (snap3A - snap3B));
    }

    // ---- Pass 3A: routing (continuous link, external detector, M/S + sidechain) ---

    // linkBlendTest: continuous stereo link on the single engine. L carries a
    // resonance at f0 over a floor common to both channels; R carries only the
    // floor (a flat comb: no excess at f0, so per-channel R is uncut). Sweeping the
    // link amount lambda in {0, 0.5, 1} (link on) blends R's reduction at f0 between
    // its per-channel value (lambda 0: ~0, R has no resonance there) and the linked
    // value (lambda 1: R gets L's cut to preserve the stereo image). The blend is a
    // log-domain lerp, so R's reduction IN dB is exactly affine in lambda; the
    // midpoint identity red_R(0.5) == 0.5*(red_R(0)+red_R(1)) is an independent
    // oracle (each endpoint measured through the real DSP, no formula from the
    // engine). A deterministic bin-aligned comb + smoothing-off makes R's f0 a clean
    // carrier, so the measured reduction is the true applied gain at every rate. And
    // lambda 1 applies identical L/R gains (symmetric probe: L==R in -> L==R out).
    void linkBlendTest (double Fs)
    {
        std::printf ("Link blend (continuous stereo link, affine identity) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs), N = 1 << order, M = kSnapshotLen;
        const int kf = (int) std::round (2000.0 * N / Fs);
        const int jlo = (int) std::ceil (kf / 1.6), jhi = (int) std::floor (kf * 1.6);
        const double b = 0.02, big = 0.12; // floor line / L's resonance at kf (excess ~15 dB)
        const double f0 = (double) kf * Fs / (double) N;

        // R = flat comb floor (kf line == neighbours == b); L = same floor + a
        // strong resonance at kf. The non-kf lines share amplitude AND phase, so the
        // floor is common; L differs only by (big-b) at kf.
        const auto xr = combSignal (order, M, [&] (int k) { return (k >= jlo && k <= jhi) ? b : 0.0; });
        const auto xl = combSignal (order, M, [&] (int k) { if (k < jlo || k > jhi) return 0.0; return (k == kf) ? big : b; });

        auto renderR = [&] (double lambda, double depth)
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, orderFor (Fs));
            s.setStereoLink (true); s.setLinkAmount (lambda);
            s.setDepth (depth); s.setSharpness (0.5); s.setSmoothingWidth (0.0); // clean single-bin read
            std::vector<double> outR ((size_t) M);
            for (int n = 0; n < M; ++n) { double l = xl[(size_t) n], r = xr[(size_t) n]; s.process (l, r); outR[(size_t) n] = r; }
            return outR;
        };
        const auto dryR = renderR (1.0, 0.0); // depth 0: gain 1 (link irrelevant)
        const auto wR0  = renderR (0.0, 1.0);
        const auto wR5  = renderR (0.5, 1.0);
        const auto wR1  = renderR (1.0, 1.0);
        const int a = M / 2, b2 = M;          // window length M/2 is an integer multiple of N (clean bins)
        const double dRef = magAt (dryR, a, b2, f0, Fs);
        const double r0 = 20.0 * std::log10 (magAt (wR0, a, b2, f0, Fs) / dRef);
        const double r5 = 20.0 * std::log10 (magAt (wR5, a, b2, f0, Fs) / dRef);
        const double r1 = 20.0 * std::log10 (magAt (wR1, a, b2, f0, Fs) / dRef);

        const double mid = 0.5 * (r0 + r1);
        if (std::abs (r5 - mid) > 0.5)
            fail ("link blend: R reduction not affine in lambda: r0=" + std::to_string (r0)
                  + " r5=" + std::to_string (r5) + " r1=" + std::to_string (r1)
                  + " mid=" + std::to_string (mid) + " at Fs=" + std::to_string (Fs));
        if (r1 > -6.0)
            fail ("link blend: lambda=1 did not cut R at f0 (image not linked): " + std::to_string (r1)
                  + " dB at Fs=" + std::to_string (Fs));
        if (r0 < -1.0)
            fail ("link blend: lambda=0 cut R without a resonance there: " + std::to_string (r0)
                  + " dB at Fs=" + std::to_string (Fs));

        // lambda=1 applies identical L/R gains (symmetric probe -> exact L==R out).
        {
            factory_core::ResonanceSuppressor s;
            s.prepare (Fs, orderFor (Fs));
            s.setStereoLink (true); s.setLinkAmount (1.0); s.setDepth (1.0); s.setSharpness (0.5);
            double e = 0.0;
            for (int n = 0; n < M; ++n)
            { double v = xl[(size_t) n]; double l = v, r = v; s.process (l, r); e = std::max (e, std::abs (l - r)); }
            if (e > 1.0e-9)
                fail ("link blend: lambda=1 L/R gains diverged err " + std::to_string (e) + " at Fs=" + std::to_string (Fs));
        }
        std::printf ("  redR(0,0.5,1)=(%.2f, %.2f, %.2f) dB  mid=%.2f (affine +/-0.5)\n", r0, r5, r1, mid);
    }

    // msRoundtripTest: M/S mode on the composite. (a) At depth 0 the output equals
    // a TEST-SIDE oracle (encode M/S -> LR4 twin allpass per component -> decode,
    // delayed N_L) to 1e-9 -- the encode/decode wraps the split cleanly. (b) M/S
    // bypass is still bit-transparent to the raw input delayed N_L. (c) A resonance
    // placed purely in M (l==r tone) is cut while the S component (l-r noise) is
    // preserved within +/-0.5 dB (unlinked + high selectivity so the S floor is
    // untouched) -- M and S are processed independently.
    void msRoundtripTest (double Fs)
    {
        std::printf ("M/S roundtrip (encode/split/decode + separation) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int NL = 1 << order;

        // (a) depth-0 reconstruction vs the M/S allpass twin oracle.
        {
            const int M = std::max (1 << 15, 8 * NL);
            std::mt19937 rng (7);
            std::uniform_real_distribution<double> u (-0.5, 0.5);
            std::vector<double> xl ((size_t) M), xr ((size_t) M);
            for (int n = 0; n < M; ++n) { xl[(size_t) n] = u (rng); xr[(size_t) n] = u (rng); }

            // Oracle: encode -> LR4 twin allpass on M and S separately -> decode.
            factory_core::LinkwitzRiley twM, twS;
            twM.setCutoff (3000.0, Fs); twS.setCutoff (3000.0, Fs);
            std::vector<double> oL ((size_t) M), oR ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                const double mid = 0.5 * (xl[(size_t) n] + xr[(size_t) n]);
                const double sid = 0.5 * (xl[(size_t) n] - xr[(size_t) n]);
                const double aM = twM.allpass (mid), aS = twS.allpass (sid);
                oL[(size_t) n] = aM + aS; oR[(size_t) n] = aM - aS;
            }

            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setChannelMode (1); m.setDepth (0.0); m.setMix (1.0);
            std::vector<double> yL ((size_t) M), yR ((size_t) M);
            for (int n = 0; n < M; ++n)
            { double l = xl[(size_t) n], r = xr[(size_t) n]; m.process (l, r); yL[(size_t) n] = l; yR[(size_t) n] = r; }

            double e = 0.0;
            for (int n = M / 2; n < M; ++n)
                e = std::max (e, std::max (std::abs (yL[(size_t) n] - oL[(size_t) (n - NL)]),
                                           std::abs (yR[(size_t) n] - oR[(size_t) (n - NL)])));
            if (e > 1.0e-9)
                fail ("M/S depth0 reconstruction vs twin oracle err " + std::to_string (e) + " at Fs=" + std::to_string (Fs));
            std::printf ("  depth0 reconErr=%.2e (spec 1e-9)\n", e);
        }

        // (b) M/S bypass bit-transparency to the raw input delayed N_L.
        {
            const int M = std::max (1 << 14, 4 * NL);
            std::mt19937 rng (29);
            std::normal_distribution<double> g (0.0, 0.3);
            std::vector<double> xl ((size_t) M), xr ((size_t) M);
            for (int n = 0; n < M; ++n) { xl[(size_t) n] = g (rng); xr[(size_t) n] = g (rng); }

            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setChannelMode (1); m.setDepth (1.2); m.setSharpness (0.5); m.setMix (0.7);
            m.setBypassed (true); m.reset();
            for (int n = 0; n < M; ++n)
            {
                double l = xl[(size_t) n], r = xr[(size_t) n];
                m.process (l, r);
                const double el = (n >= NL) ? xl[(size_t) (n - NL)] : 0.0;
                const double er = (n >= NL) ? xr[(size_t) (n - NL)] : 0.0;
                if (l != el || r != er)
                { fail ("M/S bypass not bit-transparent at n=" + std::to_string (n) + " Fs=" + std::to_string (Fs)); break; }
            }
        }

        // (c) M/S separation: resonance in M cut, S (noise) preserved.
        {
            const int M = 1 << 15;
            const double f0 = alignedFreq (Fs, 2000.0);
            std::mt19937 rng (53);
            std::normal_distribution<double> g (0.0, 0.1);
            std::vector<double> xl ((size_t) M), xr ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                const double tone = 0.5 * std::sin (2.0 * kPi * f0 * n / Fs); // in M (l==r part)
                const double ns   = g (rng);                                  // in S (l-r part)
                xl[(size_t) n] = tone + ns; xr[(size_t) n] = tone - ns;
            }

            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setChannelMode (1); m.setStereoLink (false);
            // Unlinked so M/S are processed independently; high selectivity + modest
            // depth so the broadband S floor is not nibbled (only M's tone is a peak).
            m.setDepth (0.5); m.setSharpness (0.5); m.setSelectivity (1.0);
            std::vector<double> yL ((size_t) M), yR ((size_t) M);
            for (int n = 0; n < M; ++n)
            { double l = xl[(size_t) n], r = xr[(size_t) n]; m.process (l, r); yL[(size_t) n] = l; yR[(size_t) n] = r; }

            const int a = M / 2, b = M;
            double sIn = 0.0, sOut = 0.0;
            for (int n = a; n < b; ++n)
            {
                const double si = 0.5 * (xl[(size_t) (n - NL)] - xr[(size_t) (n - NL)]);
                const double so = 0.5 * (yL[(size_t) n] - yR[(size_t) n]);
                sIn += si * si; sOut += so * so;
            }
            const double sDb = 10.0 * std::log10 (sOut / (sIn + 1e-30));
            if (std::abs (sDb) > 0.5)
                fail ("M/S: S component not preserved: " + std::to_string (sDb) + " dB at Fs=" + std::to_string (Fs));

            std::vector<double> mIn ((size_t) M), mOut ((size_t) M);
            for (int n = 0; n < M; ++n)
            { mIn[(size_t) n] = 0.5 * (xl[(size_t) n] + xr[(size_t) n]); mOut[(size_t) n] = 0.5 * (yL[(size_t) n] + yR[(size_t) n]); }
            const double mDb = 20.0 * std::log10 (magAt (mOut, a, b, f0, Fs) / magAt (mIn, a, b, f0, Fs));
            if (mDb > -6.0)
                fail ("M/S: M resonance not cut: " + std::to_string (mDb) + " dB at Fs=" + std::to_string (Fs));
            std::printf ("  S preserved %.3f dB (+/-0.5)  M cut %.2f dB (<= -6)\n", sDb, mDb);
        }
    }

    // sidechainTest: external-detector (sidechain) routing on the composite.
    // (a) With the sidechain OFF, feeding a different SC signal to the 4-arg
    // process is BIT-IDENTICAL to the 2-arg form (the SC only feeds rings, never
    // read). (b) With it ON, a strong SC tone notches the main (broadband) output
    // at that frequency -- on the low side (2 kHz) and the high side (6 kHz), keyed
    // off the correct sub-engine -- and the notch vanishes when the SC is silent.
    // (c) Toggling the sidechain stays finite and peak-bounded.
    void sidechainTest (double Fs)
    {
        std::printf ("Sidechain (external detector routing) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int N = 1 << order;

        // (a) sidechain OFF: 4-arg with a different SC == 2-arg twin, bit-exact.
        {
            const int M = std::max (1 << 14, 4 * N);
            std::mt19937 rng (61);
            std::normal_distribution<double> g (0.0, 0.3);
            std::vector<double> xm ((size_t) M), xs ((size_t) M);
            for (int n = 0; n < M; ++n) { xm[(size_t) n] = g (rng); xs[(size_t) n] = g (rng); }

            factory_core::MultiResSuppressor A, B;
            A.prepare (Fs, order); A.setDepth (1.2); A.setSharpness (0.5);
            B.prepare (Fs, order); B.setDepth (1.2); B.setSharpness (0.5); // sidechain OFF (default)
            double e = 0.0;
            for (int n = 0; n < M; ++n)
            {
                double la = xm[(size_t) n], ra = xm[(size_t) n];
                double lb = xm[(size_t) n], rb = xm[(size_t) n];
                A.process (la, ra);                                 // 2-arg
                B.process (lb, rb, xs[(size_t) n], xs[(size_t) n]); // 4-arg, different SC
                e = std::max (e, std::max (std::abs (la - lb), std::abs (ra - rb)));
            }
            if (e > 0.0)
                fail ("sidechain OFF: 4-arg SC feed changed the output (err " + std::to_string (e)
                      + ") at Fs=" + std::to_string (Fs));
        }

        // (b) sidechain ON: SC tone notches the main output at its frequency.
        auto suppressionAt = [&] (double fSc)
        {
            const int M = 1 << 15;
            std::mt19937 rng (67);
            std::normal_distribution<double> g (0.0, 0.1);
            std::vector<double> main ((size_t) M), sc ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                main[(size_t) n] = g (rng);                                   // broadband floor
                sc[(size_t) n]   = 0.3 * std::sin (2.0 * kPi * fSc * n / Fs)  // strong SC tone
                                 + 0.03 * g (rng);
            }
            auto run = [&] (bool scActive)
            {
                factory_core::MultiResSuppressor m;
                m.prepare (Fs, order);
                m.setSidechain (true); m.setDepth (1.2); m.setSharpness (0.5);
                std::vector<double> y ((size_t) M);
                for (int n = 0; n < M; ++n)
                {
                    double l = main[(size_t) n], r = main[(size_t) n];
                    const double s = scActive ? sc[(size_t) n] : 0.0;
                    m.process (l, r, s, s);
                    y[(size_t) n] = 0.5 * (l + r);
                }
                return y;
            };
            const auto on  = run (true);
            const auto off = run (false);
            const int a = M / 2, b = M;
            return 20.0 * std::log10 (magAt (on, a, b, fSc, Fs) / magAt (off, a, b, fSc, Fs));
        };
        const double loDb = suppressionAt (alignedFreq (Fs, 2000.0));
        const double hiDb = suppressionAt (alignedFreq (Fs, 6000.0));
        if (loDb > -6.0)
            fail ("sidechain: low-side SC tone did not suppress the main: " + std::to_string (loDb) + " dB at Fs=" + std::to_string (Fs));
        if (hiDb > -6.0)
            fail ("sidechain: high-side SC tone did not suppress the main: " + std::to_string (hiDb) + " dB at Fs=" + std::to_string (Fs));

        // (c) on/off transition: finite + peak-bounded.
        {
            const int M = 1 << 15;
            std::mt19937 rng (73);
            std::normal_distribution<double> g (0.0, 0.2);
            std::vector<double> main ((size_t) M), sc ((size_t) M);
            for (int n = 0; n < M; ++n)
            {
                main[(size_t) n] = g (rng) + 0.3 * std::sin (2.0 * kPi * 2000.0 * n / Fs);
                sc[(size_t) n]   = 0.4 * std::sin (2.0 * kPi * 2000.0 * n / Fs);
            }
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order); m.setDepth (1.2); m.setSharpness (0.5);
            std::vector<double> y ((size_t) M);
            const double inPeak = factory_core::testing::peakAbs (main);
            for (int n = 0; n < M; ++n)
            {
                if (n == M / 3)     m.setSidechain (true);
                if (n == 2 * M / 3) m.setSidechain (false);
                double l = main[(size_t) n], r = main[(size_t) n];
                m.process (l, r, sc[(size_t) n], sc[(size_t) n]);
                y[(size_t) n] = l;
            }
            if (! factory_core::testing::allFinite (y))
                fail ("sidechain toggle produced non-finite output at Fs=" + std::to_string (Fs));
            if (factory_core::testing::peakAbs (y) > 1.5 * inPeak)
                fail ("sidechain toggle exceeded 1.5x input peak: " + std::to_string (factory_core::testing::peakAbs (y))
                      + " vs " + std::to_string (inPeak) + " at Fs=" + std::to_string (Fs));
        }
        std::printf ("  suppression lo(2k)=%.1f dB hi(6k)=%.1f dB (spec <= -6)\n", loDb, hiDb);
    }

    // scListenTest: monitoring the raw sidechain. With scListen on the output is
    // EXACTLY the SC input delayed by N_L (integer delay, bit-exact ring read), it
    // outranks Delta, and in M/S mode it still returns the RAW (undecoded) SC.
    void scListenTest (double Fs)
    {
        std::printf ("SC listen (raw sidechain, N_L delay, exact) @ Fs=%.0f\n", Fs);
        const int order = orderFor (Fs);
        const int NL = 1 << order;
        const int M = std::max (1 << 14, 4 * NL);
        std::mt19937 rng (89);
        std::normal_distribution<double> g (0.0, 0.3);
        std::vector<double> ml ((size_t) M), mr ((size_t) M), sl ((size_t) M), sr ((size_t) M);
        for (int n = 0; n < M; ++n)
        {
            ml[(size_t) n] = g (rng); mr[(size_t) n] = g (rng);
            sl[(size_t) n] = g (rng); sr[(size_t) n] = g (rng);
        }

        auto checkExact = [&] (int chMode, bool delta, const char* what)
        {
            factory_core::MultiResSuppressor m;
            m.prepare (Fs, order);
            m.setChannelMode (chMode); m.setScListen (true); m.setDelta (delta);
            m.setDepth (1.2); m.setSharpness (0.5);
            for (int n = 0; n < M; ++n)
            {
                double l = ml[(size_t) n], r = mr[(size_t) n];
                m.process (l, r, sl[(size_t) n], sr[(size_t) n]);
                const double el = (n >= NL) ? sl[(size_t) (n - NL)] : 0.0;
                const double er = (n >= NL) ? sr[(size_t) (n - NL)] : 0.0;
                if (l != el || r != er)
                { fail (std::string ("scListen ") + what + ": out != raw SC delayed N_L at n=" + std::to_string (n)
                        + " Fs=" + std::to_string (Fs)); return; }
            }
        };
        checkExact (0, false, "stereo");
        checkExact (0, true,  "stereo+delta"); // scListen outranks Delta
        checkExact (1, false, "M/S-raw");      // undecoded even in M/S
        std::printf ("  ok (out == SC[n-%d], exact: stereo / delta / M/S)\n", NL);
    }

    // ---- Reduction-profile (soothe-style depth EQ) shape invariants ----------
    // Independent, oracle-free checks of the per-frequency sensitivity curve
    // (factory_core::ReductionProfile), the single source of truth shared by the
    // audio rasteriser and the editor. Rate-independent (the shape is a function
    // of frequency only); reductionDefaultTest additionally rasterises the
    // factory config across the full rate matrix.
    using BT = factory_core::ReductionBandType;
    factory_core::ReductionNodes oneBand (double f, BT type, double sens)
    {
        factory_core::ReductionNodes n; n.bands[0] = { true, f, type, sens }; return n;
    }
    factory_core::ReductionNodes oneCut (bool low, double f, double slope)
    {
        factory_core::ReductionNodes n; (low ? n.lowCut : n.highCut) = { true, f, slope }; return n;
    }
    factory_core::ReductionNodes defaultNodes()
    {
        factory_core::ReductionNodes n;
        n.lowCut  = { true, 450.0,   24.0 };
        n.highCut = { true, 16000.0, 24.0 };
        n.bands[0] = { true, 991.0,  BT::Bell, 0.0 };
        n.bands[1] = { true, 2500.0, BT::Bell, 0.0 };
        n.bands[2] = { true, 5000.0, BT::Bell, 6.0 };
        n.bands[3] = { true, 8000.0, BT::Bell, 0.0 };
        return n;
    }

    void reductionProfileTest()
    {
        std::printf ("Reduction-profile shapes\n");
        auto db = [] (double f, const factory_core::ReductionNodes& n)
                  { return factory_core::reductionProfileDbAt (f, n); };
        auto near = [] (double a, double b, double tol, const char* what)
                    { if (std::abs (a - b) > tol) fail (std::string (what) + ": " + std::to_string (a)); };

        // Bell: peak == sens at f0, ~0 two octaves away.
        { auto n = oneBand (1000.0, BT::Bell, 6.0);
          near (db (1000.0, n), 6.0, 0.02, "bell peak != sens");
          if (std::abs (db (4000.0, n)) > 0.3) fail ("bell not local (2 oct)"); }

        // Low/High shelf: full sens on one side, ~0 on the other, half at corner.
        { auto n = oneBand (1000.0, BT::LowShelf, 6.0);
          if (db (100.0, n) < 5.5)  fail ("low shelf not full below");
          if (db (10000.0, n) > 0.5) fail ("low shelf not zero above");
          near (db (1000.0, n), 3.0, 0.4, "low shelf corner != sens/2"); }
        { auto n = oneBand (1000.0, BT::HighShelf, 6.0);
          if (db (10000.0, n) < 5.5) fail ("high shelf not full above");
          if (db (100.0, n) > 0.5)   fail ("high shelf not zero below"); }

        // Cuts are rounded (Butterworth): exactly −3 dB at the corner for every
        // slope, monotone, ~0 deep in the pass-band, and the stop-band asymptotes
        // to slopeDbPerOct measured over a deep octave (away from the knee).
        for (double slope : { 6.0, 12.0, 24.0, 48.0 })
        {
            auto lo = oneCut (true, 1000.0, slope);
            near (db (1000.0, lo), -3.0103, 0.02, "low cut -3 dB at corner");
            if (db (8000.0, lo) < -0.25)  fail ("low cut acts deep in pass-band");     // 3 oct above ~0
            near (db (62.5, lo) - db (125.0, lo), -slope, slope * 0.06, "low cut asymptotic slope");
            if (! (db (500.0, lo) < db (1000.0, lo))) fail ("low cut not monotone");

            auto hi = oneCut (false, 1000.0, slope);
            near (db (1000.0, hi), -3.0103, 0.02, "high cut -3 dB at corner");
            if (db (125.0, hi) < -0.25)   fail ("high cut acts deep in pass-band");
            near (db (16000.0, hi) - db (8000.0, hi), -slope, slope * 0.06, "high cut asymptotic slope");
            if (! (db (2000.0, hi) < db (1000.0, hi))) fail ("high cut not monotone");
        }

        // Band shelf: flat-topped bump == sens at centre. Band reject: dip.
        { auto n = oneBand (1000.0, BT::BandShelf, 6.0);
          near (db (1000.0, n), 6.0, 0.3, "band shelf centre");
          if (std::abs (db (8000.0, n)) > 0.5) fail ("band shelf not local"); }
        { auto n = oneBand (1000.0, BT::BandReject, 6.0);
          near (db (1000.0, n), -6.0, 0.3, "band reject centre"); }

        // Tilt: ±sens by two octaves, monotone through f0.
        { auto n = oneBand (1000.0, BT::Tilt, 12.0);
          near (db (4000.0, n),  12.0, 0.5, "tilt high");
          near (db (250.0,  n), -12.0, 0.5, "tilt low");
          near (db (1000.0, n),   0.0, 0.02, "tilt pivot");
          if (! (db (2000.0, n) > db (500.0, n))) fail ("tilt not monotone"); }
        std::printf ("  ok\n");
    }

    // ---- Phase 4: 8-band width EQ -------------------------------------------
    // Independent v1 (pre-Phase-4) oracle: the fixed-width formulas exactly as
    // they existed before ReductionNodes gained widthOct, hard-coded here (NOT
    // calling factory_core::detail::* -- that would just be the implementation
    // checking itself). Only cuts + bands[0..3] exist in this model, matching
    // what the plugin shipped through v1.5.0.
    namespace v1
    {
        constexpr double kBellSigma  = 0.35;
        constexpr double kShelfWidth = 0.50;
        constexpr double kBandHalf   = 0.50;
        constexpr double kBandEdge   = 0.35;
        constexpr double kTiltSpan   = 2.0 * 0.69314718055994530942;

        double bandTop (double x)
        {
            const double num = std::tanh ((x + kBandHalf) / kBandEdge) - std::tanh ((x - kBandHalf) / kBandEdge);
            const double den = 2.0 * std::tanh (kBandHalf / kBandEdge);
            return num / den;
        }
        double bandDb (BT type, double x, double sensDb)
        {
            switch (type)
            {
                case BT::Bell:       { const double t = x / kBellSigma; return sensDb * std::exp (-0.5 * t * t); }
                case BT::LowShelf:   return sensDb * 0.5 * (1.0 - std::tanh (x / kShelfWidth));
                case BT::HighShelf:  return sensDb * 0.5 * (1.0 + std::tanh (x / kShelfWidth));
                case BT::BandShelf:  return sensDb * bandTop (x);
                case BT::BandReject: return -sensDb * bandTop (x);
                case BT::Tilt:       return sensDb * std::clamp (x / kTiltSpan, -1.0, 1.0);
            }
            return 0.0;
        }
        double cutDb (double f, double fc, double slopeDbPerOct, bool lowCut)
        {
            const double order = std::max (0.5, slopeDbPerOct / 6.0);
            const double ratio = lowCut ? (fc / std::max (1.0e-6, f)) : (f / std::max (1.0e-6, fc));
            return -10.0 * std::log10 (1.0 + std::pow (ratio, 2.0 * order));
        }
        // v1 only ever had 4 bands: bands[4..7] (if present/on) are deliberately
        // ignored, mirroring the type that existed pre-Phase-4.
        double reductionProfileDbAt (double f, const factory_core::ReductionNodes& n)
        {
            const double lf = std::log (std::max (1.0e-6, f));
            double db = 0.0;
            if (n.lowCut.on)  db += cutDb (f, n.lowCut.freqHz,  n.lowCut.slopeDbPerOct,  true);
            if (n.highCut.on) db += cutDb (f, n.highCut.freqHz, n.highCut.slopeDbPerOct, false);
            for (int b = 0; b < 4; ++b)
            {
                const auto& bd = n.bands[(size_t) b];
                if (bd.on) db += bandDb (bd.type, lf - std::log (std::max (1.0e-6, bd.freqHz)), bd.sensDb);
            }
            return db;
        }
    }

    // Test 25: at the factory defaults (b0..b3 as v1, b4..b7 off, every widthOct
    // == 0.50) the new 8-band width-scaled implementation must reproduce the old
    // fixed-width v1 oracle at every frequency -- bit-identical in exact
    // arithmetic (w = widthOct/kWidthRef == 1.0 exactly, and x*1.0 == x), so a
    // small relative tolerance only absorbs incidental compiler/libm variance
    // between the header's call site and this independently-written oracle.
    void reductionDefaultIdentityTest()
    {
        std::printf ("Reduction default-identity (8-band width==0.50 vs independent v1 oracle)\n");
        const auto n = defaultNodes(); // b0..b3 == v1 factory defaults; b4..b7 default-constructed (off)
        double maxAbs = 0.0, maxRel = 0.0;
        for (int i = 0; i <= 1000; ++i)
        {
            const double t = (double) i / 1000.0;
            const double f = 20.0 * std::pow (1000.0, t); // 20 Hz .. 20 kHz, log-spaced
            const double got = factory_core::reductionProfileDbAt (f, n);
            const double expected = v1::reductionProfileDbAt (f, n);
            const double diff = std::abs (got - expected);
            const double tol  = 1.0e-12 * std::max (1.0, std::abs (expected));
            maxAbs = std::max (maxAbs, diff);
            maxRel = std::max (maxRel, diff / std::max (1.0, std::abs (expected)));
            if (diff > tol)
                fail ("default profile diverged from v1 oracle at f=" + std::to_string (f)
                      + " got=" + std::to_string (got) + " expected=" + std::to_string (expected));
        }
        std::printf ("  maxAbsDiff=%.3e maxRelDiff=%.3e (spec: bit-identical or <= 1e-12 rel)\n", maxAbs, maxRel);
    }

    // Test 26: width sweep. For a single Bell band (bandDb = sensDb *
    // exp(-0.5*(x/sigma)^2), sigma = kBellSigma*widthOct/kWidthRef, x = ln(f/f0)),
    // the profile reaches HALF the peak's dB value (sensDb/2) at x_half =
    // sigma*sqrt(2 ln 2) -- an independent closed-form derivation from the Bell
    // spec, not read from the implementation. The half-width in octaves,
    // x_half/ln2, is therefore proportional to widthOct. Measure the actual
    // half-value frequency via bisection on the real reductionProfileDbAt (the
    // function the engine/editor evaluate -- the "z-domain", i.e. the domain
    // that actually runs, for this non-filter EQ curve) and compare to the
    // spec ratio.
    void widthSweepTest()
    {
        std::printf ("Width sweep (Bell half-value width proportional to widthOct)\n");
        constexpr double kBellSigmaV1 = 0.35, kWidthRefV1 = 0.50, kLn2 = 0.69314718055994530942;
        const double f0 = 1000.0, sens = -12.0; // negative peak; half value = -6 dB

        for (double widthOct : { 0.25, 0.50, 1.0, 2.0 })
        {
            factory_core::ReductionNodes n;
            n.bands[0] = { true, f0, BT::Bell, sens, widthOct };

            // Bisect in log-frequency for the root of db(f) == sens/2 above f0
            // (db is monotone from sens at f0 toward 0 as f grows away from it).
            double lo = f0, hi = f0 * 64.0;
            for (int it = 0; it < 60; ++it)
            {
                const double mid = std::sqrt (lo * hi);
                const double d = factory_core::reductionProfileDbAt (mid, n);
                if (d < sens * 0.5) lo = mid; else hi = mid;
            }
            const double fHalf = std::sqrt (lo * hi);
            const double measuredHalfWidthOct = std::log (fHalf / f0) / kLn2;

            const double specSigma = kBellSigmaV1 * (widthOct / kWidthRefV1);
            const double specHalfWidthOct = specSigma * std::sqrt (2.0 * kLn2) / kLn2;

            const double relErr = std::abs (measuredHalfWidthOct - specHalfWidthOct) / specHalfWidthOct;
            if (relErr > 0.03) // a few percent, fixed (bisection converges to ~2^-60; headroom is the oracle itself)
                fail ("width=" + std::to_string (widthOct) + " half-width " + std::to_string (measuredHalfWidthOct)
                      + " oct vs spec " + std::to_string (specHalfWidthOct) + " oct (relErr " + std::to_string (relErr) + ")");
            std::printf ("  width=%.2f oct: measured=%.4f oct  spec=%.4f oct  relErr=%.4f\n",
                         widthOct, measuredHalfWidthOct, specHalfWidthOct, relErr);
        }
    }

    // Test 27: 8-band superposition. reductionProfileDbAt accumulates each
    // enabled band's bandDb() into a single running sum, so with all 8 bands on
    // (different freq/type/sens/width, no cuts) the combined curve must equal
    // the sum of each band's OWN isolated contribution at every frequency --
    // an exact-arithmetic gate (same accumulation order either way).
    void eightBandSuperpositionTest()
    {
        std::printf ("8-band superposition (combined == sum of individual contributions)\n");
        const double freqs [8] = { 80.0, 200.0, 500.0, 1200.0, 3000.0, 6000.0, 9000.0, 15000.0 };
        const double senss [8] = { 4.0, -6.0, 8.0, -3.0, 10.0, -8.0, 5.0, -2.0 };
        const BT     types [8] = { BT::Bell, BT::LowShelf, BT::HighShelf, BT::BandShelf,
                                   BT::BandReject, BT::Tilt, BT::Bell, BT::LowShelf };
        const double widths[8] = { 0.25, 0.50, 1.0, 2.0, 0.30, 0.75, 1.5, 0.10 };

        factory_core::ReductionNodes n; // all cuts off -- isolate the band superposition
        for (int b = 0; b < 8; ++b)
            n.bands[(size_t) b] = { true, freqs[b], types[b], senss[b], widths[b] };

        double maxErr = 0.0;
        for (int i = 0; i <= 400; ++i)
        {
            const double t = (double) i / 400.0;
            const double f = 20.0 * std::pow (1000.0, t); // 20 Hz .. 20 kHz, log-spaced

            double sumIndividual = 0.0;
            for (int b = 0; b < 8; ++b)
            {
                factory_core::ReductionNodes one; // all bands off by default except b
                one.bands[(size_t) b] = n.bands[(size_t) b];
                sumIndividual += factory_core::reductionProfileDbAt (f, one);
            }
            const double combined = factory_core::reductionProfileDbAt (f, n);
            maxErr = std::max (maxErr, std::abs (combined - sumIndividual));
        }
        if (maxErr > 1.0e-9) fail ("8-band superposition mismatch, maxErr=" + std::to_string (maxErr));
        std::printf ("  maxErr=%.2e (expect ~0, additive by construction)\n", maxErr);
    }

    void reductionDefaultTest (double Fs)
    {
        std::printf ("Reduction default profile (rasterise) @ Fs=%.0f\n", Fs);
        const int N = 1 << orderFor (Fs);
        const int bins = N / 2 + 1;
        const auto n = defaultNodes();

        std::vector<double> prof ((size_t) bins);
        for (int k = 0; k < bins; ++k)
        {
            const double f = (double) k * Fs / N;
            prof[(size_t) k] = (k == 0) ? 1.0 : factory_core::reductionProfileLinearAt (f, n);
        }
        if (! factory_core::testing::allFinite (prof)) fail ("profile not finite");
        for (double v : prof) if (v < 0.0 || v > 4.0) fail ("profile out of [0,4]: " + std::to_string (v));

        auto P = [&n] (double f) { return factory_core::reductionProfileLinearAt (f, n); };
        if (P (5000.0) < 1.5)                       fail ("5k emphasis (+6 dB) missing");
        if (P (1000.0) < 0.9 || P (1000.0) > 1.1)   fail ("1k not ~unity");
        if (P (100.0)  > 0.1)                        fail ("low cut not applied at 100 Hz");
        if (P (20000.0) > 0.6)                       fail ("high cut not applied near 20 kHz");
        if (! (P (5000.0) > P (1000.0)))            fail ("band3 should exceed 1 kHz");
        if (! factory_core::testing::resolutionFollowsSampleRate (Fs, 25.0, 0.030))
            fail ("resolution out of range at Fs=" + std::to_string (Fs));
        std::printf ("  P(100)=%.3f P(1k)=%.3f P(5k)=%.3f P(20k)=%.3f\n",
                     P (100.0), P (1000.0), P (5000.0), P (20000.0));
    }
}

int main (int argc, char** argv)
{
    // Full standard sample-rate matrix, up to 192 kHz. A single rate may be
    // passed on the command line (CTest registers one case per rate).
    std::vector<double> rates;
    if (argc > 1) rates.push_back (std::atof (argv[1]));
    else          rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    fftTest();
    reductionProfileTest();
    reductionDefaultIdentityTest();
    widthSweepTest();
    eightBandSuperpositionTest();
    for (double Fs : rates)
    {
        reconstructionTest (Fs);
        deltaTest (Fs, 0);   // Soft
        deltaTest (Fs, 1);   // Hard
        bypassAlignmentTest (Fs);
        bypassToggleTest (Fs);
        deltaMixIdentityTest (Fs);
        suppressionTest (Fs);
        envelopeNotchTest (Fs);
        selectivityContrastTest (Fs);
        gainSmoothingTest (Fs);
        profileTest (Fs);
        preSpectrumTest (Fs);
        listenProfileTest (Fs);
        stereoLinkTest (Fs);
        silenceTest (Fs, 0); // Soft
        silenceTest (Fs, 1); // Hard
        resolutionTest (Fs);
        softLevelInvarianceTest (Fs);
        hardLevelDependenceTest (Fs);
        hardSuppressionTest (Fs);
        hardStabilityTest (Fs);
        reductionDefaultTest (Fs);
        resetStateTest (Fs);
        releaseBallisticsTest (Fs);
        attackBallisticsTest (Fs);
        tiltBallisticsTest (Fs);
        qualityTest (Fs);
        rangeGatingTest (Fs);
        depthSharpnessMidStreamTest (Fs);
        dualReconstructionTest (Fs);
        dualSplitHzDefaultIdentityTest (Fs);
        dualDeltaTest (Fs);
        dualBypassTest (Fs);
        dualSpeedTest (Fs);
        dualCrossoverTest (Fs);
        dualQualityTest (Fs);
        dualDisplayTest (Fs);
        displaySmoothingIsAudioTransparentTest (Fs);
        linkBlendTest (Fs);
        msRoundtripTest (Fs);
        sidechainTest (Fs);
        scListenTest (Fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
