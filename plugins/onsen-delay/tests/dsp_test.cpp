//
// dsp_test.cpp — headless verification of the onsen-delay DSP core
// (factory_core::OnsenDelay), spec-based with independent oracles:
//
//   1. Interval echo timing — an impulse must echo at exactly
//      baseMs * 2^(-semitones/12) for every interval choice (static table of
//      semitones lives here, independent of the enum mapping), including the
//      worst-case buffer point (max time x octave-down = 4 s).
//   2. Auto sequencer schedule — step k lasts round(baseMs*ratio(k)*fs/1000)
//      samples (integer arithmetic oracle computed here).
//   3. Doppler glide oracle — the glide spec (one-pole lag, tau from the
//      published formula) predicts the delay trajectory D(t) analytically;
//      the number of rising zero crossings of a delayed sine over a window
//      must equal f0*(W + D(t0) - D(t1))/fs. Measured on real signal.
//   4. impulseResponseNonIncreasing at the worst case (max regen, tone wide
//      open, wet-only), both static and while the sequencer+glide modulate.
//   5. Long-hold realistic peak bound + allFinite at worst-case settings, at
//      max glide and again in the fastest time-varying region (glide=0).
//   6. Finite guard: a NaN input sample must not propagate.
//   7. State reset on prepare; silence in -> silence out.
//   8. Feedback tone damping — echo(k+1)/echo(k) spectral ratio must match
//      regen * |H_onepole(e^jw)| evaluated in the z-domain (never the analog
//      prototype), at a low and a high probe frequency.
//
#include "factory_core/OnsenDelay.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace fct = factory_core::testing;
    using factory_core::OnsenDelay;
    using Interval = OnsenDelay::Interval;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    constexpr double kPi = 3.14159265358979323846;

    // Independent semitone table (do not call OnsenDelay::semitonesFor here).
    struct IvSpec { Interval iv; double semitones; const char* name; };
    const IvSpec kIntervals[] = {
        { Interval::OctaveDown, -12.0, "OctaveDown" },
        { Interval::FifthDown,   -7.0, "FifthDown"  },
        { Interval::FourthDown,  -5.0, "FourthDown" },
        { Interval::Unison,       0.0, "Unison"     },
        { Interval::FourthUp,     5.0, "FourthUp"   },
        { Interval::FifthUp,      7.0, "FifthUp"    },
        { Interval::OctaveUp,    12.0, "OctaveUp"   },
    };

    // -- driving helpers ------------------------------------------------------

    void processMono (OnsenDelay& e, std::vector<double>& io)
    {
        constexpr int kChunk = 512;
        std::vector<float> buf (kChunk);
        for (std::size_t pos = 0; pos < io.size(); pos += kChunk)
        {
            const int n = (int) std::min<std::size_t> (kChunk, io.size() - pos);
            for (int i = 0; i < n; ++i) buf[(size_t) i] = (float) io[pos + (size_t) i];
            float* chans[1] = { buf.data() };
            e.process (chans, 1, n);
            for (int i = 0; i < n; ++i) io[pos + (size_t) i] = (double) buf[(size_t) i];
        }
    }

    std::size_t argMaxAbs (const std::vector<double>& x)
    {
        std::size_t best = 0;
        for (std::size_t i = 1; i < x.size(); ++i)
            if (std::abs (x[i]) > std::abs (x[best])) best = i;
        return best;
    }

    // Goertzel magnitude of x[start..start+len) at frequency f.
    double goertzelMag (const std::vector<double>& x, std::size_t start, std::size_t len,
                        double f, double fs)
    {
        const double w = 2.0 * kPi * f / fs;
        const double coeff = 2.0 * std::cos (w);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (std::size_t i = 0; i < len && start + i < x.size(); ++i)
        {
            s0 = x[start + i] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2));
    }

    // Deterministic noise (test-local; no library RNG).
    struct Lcg
    {
        unsigned long long s;
        explicit Lcg (unsigned long long seed) : s (seed) {}
        double next() // in [-1, 1)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return (double) ((s >> 11) & ((1ULL << 53) - 1)) / (double) (1ULL << 52) - 1.0;
        }
    };

    // Wet-only engine with a pinned (manual, never advanced) sequencer step 0.
    void configureStatic (OnsenDelay& e, double fs, double baseMs,
                          double glide, double regen, double toneHz)
    {
        e.prepare (fs, 1);
        e.setBaseTimeMs (baseMs);
        e.setIntervals (Interval::Unison, Interval::Unison);
        e.setGlide (glide);
        e.setRegen (regen);
        e.setToneHz (toneHz);
        e.setMix (1.0);
        e.setManualStep (true);
        e.reset();
    }

    // -- 1. interval echo timing ----------------------------------------------

    void checkEchoAt (OnsenDelay& e, double fs, double expectSamples, const std::string& what)
    {
        // Settle the glide (tau floor is 5 ms; 0.2 s = 40 tau), then impulse.
        std::vector<double> settle ((std::size_t) (0.2 * fs), 0.0);
        processMono (e, settle);

        std::vector<double> cap ((std::size_t) std::ceil (expectSamples) + (std::size_t) (0.05 * fs), 0.0);
        cap[0] = 1.0;
        processMono (e, cap);

        const std::size_t peak = argMaxAbs (cap);
        if (std::abs ((double) peak - expectSamples) > 2.0)
            fail (what + ": echo at " + std::to_string (peak) + " expected "
                        + std::to_string (expectSamples));
        if (std::abs (cap[peak]) < 0.45 || std::abs (cap[peak]) > 1.0 + 1e-9)
            fail (what + ": echo amplitude " + std::to_string (cap[peak]) + " out of [0.45, 1]");
    }

    void testIntervalEchoes (double fs)
    {
        const double baseMs = 200.0;
        for (const auto& spec : kIntervals)
        {
            OnsenDelay e;
            configureStatic (e, fs, baseMs, 0.0, 0.0, OnsenDelay::kMaxToneHz);
            e.setIntervals (spec.iv, Interval::Unison);
            e.triggerStep(); // pin the sequencer on step 1 = the interval under test

            const double expect = baseMs * 0.001 * fs * std::exp2 (-spec.semitones / 12.0);
            checkEchoAt (e, fs, expect, std::string ("interval ") + spec.name);
        }

        // Worst-case buffer: max base time at octave-down = 4 s of delay. A
        // silent clamp/wrap would put the echo at the wrong place (or nowhere).
        {
            OnsenDelay e;
            configureStatic (e, fs, OnsenDelay::kMaxTimeMs, 0.0, 0.0, OnsenDelay::kMaxToneHz);
            e.setIntervals (Interval::OctaveDown, Interval::Unison);
            e.triggerStep();
            checkEchoAt (e, fs, OnsenDelay::kMaxTimeMs * 0.001 * fs * 2.0, "worst-case buffer (4 s)");
        }
    }

    // -- 2. auto sequencer schedule -------------------------------------------

    void testSequencerSchedule (double fs)
    {
        const double baseMs = 100.0;
        OnsenDelay e;
        e.prepare (fs, 1);
        e.setBaseTimeMs (baseMs);
        e.setIntervals (Interval::OctaveUp, Interval::Unison);
        e.setGlide (0.0);
        e.setRegen (0.0);
        e.setMix (1.0);
        e.setManualStep (false);
        e.reset();

        // Independent schedule: step durations in samples (base, int1=x0.5, int2=x1).
        const long long dur[3] = {
            std::llround (baseMs * 0.001 * fs),
            std::llround (baseMs * 0.001 * fs * 0.5),
            std::llround (baseMs * 0.001 * fs),
        };

        std::vector<long long> transitions;
        int prev = e.currentStep();
        const long long total = (dur[0] + dur[1] + dur[2]) * 2 + 16;
        float sample = 0.0f;
        float* chans[1] = { &sample };
        for (long long n = 0; n < total; ++n)
        {
            sample = 0.0f;
            e.process (chans, 1, 1);
            if (e.currentStep() != prev)
            {
                transitions.push_back (n);
                prev = e.currentStep();
            }
        }

        if (transitions.size() < 5)
        {
            fail ("sequencer: expected >= 5 transitions, got " + std::to_string (transitions.size()));
            return;
        }
        // Spacing between transitions == duration of the step being left.
        // transitions[0] leaves step 0; [1] leaves step 1; etc.
        for (std::size_t k = 1; k < 5; ++k)
        {
            const long long spacing  = transitions[k] - transitions[k - 1];
            const long long expected = dur[k % 3];
            if (spacing != expected)
                fail ("sequencer: step spacing " + std::to_string (spacing)
                      + " != expected " + std::to_string (expected));
        }
    }

    // -- 3. Doppler glide oracle ----------------------------------------------

    long long risingCrossings (const std::vector<double>& x, std::size_t start, std::size_t len)
    {
        long long count = 0;
        for (std::size_t i = std::max<std::size_t> (start, 1); i < start + len && i < x.size(); ++i)
            if (x[i - 1] < 0.0 && x[i] >= 0.0)
                ++count;
        return count;
    }

    void testDopplerGlide (double fs)
    {
        const double t1Ms = 300.0, t2Ms = 150.0, glide = 0.5, f0 = 500.0;

        OnsenDelay e;
        configureStatic (e, fs, t1Ms, glide, 0.0, OnsenDelay::kMaxToneHz);

        // Phase-continuous sine source.
        double phase = 0.0;
        auto makeSine = [&] (std::size_t n)
        {
            std::vector<double> v (n);
            const double inc = 2.0 * kPi * f0 / fs;
            for (std::size_t i = 0; i < n; ++i) { v[i] = 0.8 * std::sin (phase); phase += inc; }
            return v;
        };

        auto prefill = makeSine ((std::size_t) (1.0 * fs)); // > D1, glide settled
        processMono (e, prefill);

        e.setBaseTimeMs (t2Ms);
        auto cap = makeSine ((std::size_t) (2.0 * fs));
        processMono (e, cap);

        if (! fct::allFinite (cap)) { fail ("doppler: non-finite output"); return; }

        // Spec-side delay trajectory (samples), evaluated analytically.
        const double d1 = t1Ms * 0.001 * fs, d2 = t2Ms * 0.001 * fs;
        const double tauMs = OnsenDelay::kMinGlideTauMs + glide * glide * OnsenDelay::kMaxGlideTauMs;
        const double alpha = 1.0 - std::exp (-1.0 / std::max (1.0, tauMs * 0.001 * fs));
        auto delayAt = [&] (double n) { return d2 + (d1 - d2) * std::pow (1.0 - alpha, n); };

        const double W = 0.5 * fs;
        struct Win { double start; const char* name; } wins[] = {
            { 0.0,      "glide-active" },
            { 1.5 * fs, "settled"      },
        };
        for (const auto& w : wins)
        {
            const long long meas = risingCrossings (cap, (std::size_t) w.start, (std::size_t) W);
            const double expect  = f0 * (W + delayAt (w.start) - delayAt (w.start + W)) / fs;
            if (std::abs ((double) meas - expect) > 2.5)
                fail (std::string ("doppler ") + w.name + ": crossings "
                      + std::to_string (meas) + " expected " + std::to_string (expect));
        }

        // Direction: shortening the delay must raise the pitch during the glide.
        const long long measA = risingCrossings (cap, 0, (std::size_t) W);
        if ((double) measA <= f0 * W / fs + 5.0)
            fail ("doppler: no clear pitch-up during glide toward shorter delay");
    }

    // -- 4. feedback stability at worst case -----------------------------------

    void testFeedbackStability (double fs)
    {
        // Static worst case: max regen, tone wide open, wet-only.
        {
            OnsenDelay e;
            configureStatic (e, fs, 100.0, 0.0, 1.0, OnsenDelay::kMaxToneHz);
            auto tick = [&] (double x)
            {
                float s = (float) x;
                float* c[1] = { &s };
                e.process (c, 1, 1);
                return (double) s;
            };
            if (! fct::impulseResponseNonIncreasing (tick, fs))
                fail ("impulse response grows at max regen (static)");
        }
        // Modulated worst case: sequencer stepping octaves with max glide.
        {
            OnsenDelay e;
            e.prepare (fs, 1);
            e.setBaseTimeMs (100.0);
            e.setIntervals (Interval::OctaveDown, Interval::OctaveUp);
            e.setGlide (1.0);
            e.setRegen (1.0);
            e.setToneHz (OnsenDelay::kMaxToneHz);
            e.setMix (1.0);
            e.setManualStep (false);
            e.reset();
            auto tick = [&] (double x)
            {
                float s = (float) x;
                float* c[1] = { &s };
                e.process (c, 1, 1);
                return (double) s;
            };
            if (! fct::impulseResponseNonIncreasing (tick, fs))
                fail ("impulse response grows at max regen (sequenced + glide)");
        }
    }

    // -- 5. long-hold peak bound ------------------------------------------------

    void testLongHoldBounded (double fs)
    {
        OnsenDelay e;
        e.prepare (fs, 2);
        e.setBaseTimeMs (200.0);
        e.setIntervals (Interval::OctaveDown, Interval::OctaveUp);
        e.setGlide (1.0);
        e.setRegen (1.0);
        e.setToneHz (OnsenDelay::kMaxToneHz);
        e.setMix (1.0);
        e.setManualStep (false);
        e.reset();

        // Realistic bound: |dry| <= 0.5, |feedback| <= kMaxRegen/kSatPregain
        // (tanh ceiling), so buffer contents stay < 1.14 and wet reads below
        // ~1.2 even mid-glide. 1.5 leaves margin without hiding a runaway.
        Lcg lcgL (0xA5A5A5A5ULL), lcgR (12345ULL);
        const int chunk = 173; // deliberately not a power of two
        std::vector<float> l (chunk), r (chunk);
        double peak = 0.0;
        bool finite = true;
        const long long total = (long long) (8.0 * fs);
        for (long long done = 0; done < total; done += chunk)
        {
            const int n = (int) std::min<long long> (chunk, total - done);
            for (int i = 0; i < n; ++i)
            {
                l[(size_t) i] = (float) (0.5 * lcgL.next());
                r[(size_t) i] = (float) (0.5 * lcgR.next());
            }
            float* chans[2] = { l.data(), r.data() };
            e.process (chans, 2, n);
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i])) finite = false;
                peak = std::max ({ peak, (double) std::abs (l[(size_t) i]), (double) std::abs (r[(size_t) i]) });
            }
        }
        if (! finite) fail ("long hold produced non-finite output");
        if (peak > 1.5) fail ("long-hold peak " + std::to_string (peak) + " exceeds realistic bound 1.5");
    }

    // Fastest time-varying region: glide at its tau floor (5 ms) with octave
    // jumps every 250 ms, so the read pointer sweeps hard and re-reads written
    // history. The impulse-based non-increasing oracle is unsuitable here (a
    // step transition defers echoes across measurement windows), so the "no
    // energy pump" invariant is gated long-hold style with the same bound.
    void testFastSweepLongHoldBounded (double fs)
    {
        OnsenDelay e;
        e.prepare (fs, 2);
        e.setBaseTimeMs (250.0);
        e.setIntervals (Interval::OctaveDown, Interval::OctaveUp);
        e.setGlide (0.0);
        e.setRegen (1.0);
        e.setToneHz (OnsenDelay::kMaxToneHz);
        e.setMix (1.0);
        e.setManualStep (false);
        e.reset();

        Lcg lcgL (0x5EED5EEDULL), lcgR (67890ULL);
        const int chunk = 173; // deliberately not a power of two
        std::vector<float> l (chunk), r (chunk);
        double peak = 0.0;
        bool finite = true;
        const long long total = (long long) (8.0 * fs);
        for (long long done = 0; done < total; done += chunk)
        {
            const int n = (int) std::min<long long> (chunk, total - done);
            for (int i = 0; i < n; ++i)
            {
                l[(size_t) i] = (float) (0.5 * lcgL.next());
                r[(size_t) i] = (float) (0.5 * lcgR.next());
            }
            float* chans[2] = { l.data(), r.data() };
            e.process (chans, 2, n);
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i])) finite = false;
                peak = std::max ({ peak, (double) std::abs (l[(size_t) i]), (double) std::abs (r[(size_t) i]) });
            }
        }
        if (! finite) fail ("fast-sweep long hold produced non-finite output");
        if (peak > 1.5) fail ("fast-sweep long-hold peak " + std::to_string (peak) + " exceeds realistic bound 1.5");
    }

    // -- 6. NaN input recovery ---------------------------------------------------

    void testNanRecovery (double fs)
    {
        OnsenDelay e;
        e.prepare (fs, 1);
        e.setBaseTimeMs (150.0);
        e.setIntervals (Interval::FifthUp, Interval::OctaveUp);
        e.setGlide (0.3);
        e.setRegen (0.6);
        e.setToneHz (8000.0);
        e.setMix (0.5);
        e.setManualStep (false);
        e.reset();

        std::vector<double> pre ((std::size_t) (0.5 * fs));
        for (std::size_t i = 0; i < pre.size(); ++i)
            pre[i] = 0.5 * std::sin (2.0 * kPi * 330.0 * (double) i / fs);
        processMono (e, pre);

        std::vector<double> hit (1, std::nan (""));
        processMono (e, hit);
        if (! fct::allFinite (hit))
            fail ("NaN input propagated through the dry/wet path");

        std::vector<double> post ((std::size_t) (1.0 * fs));
        for (std::size_t i = 0; i < post.size(); ++i)
            post[i] = 0.5 * std::sin (2.0 * kPi * 330.0 * (double) i / fs);
        processMono (e, post);
        if (! fct::allFinite (post))
            fail ("engine state stayed corrupt after a NaN input sample");
    }

    // -- 7. reset on prepare / silence floor --------------------------------------

    void testResetAndSilence (double fs)
    {
        OnsenDelay e;
        configureStatic (e, fs, 300.0, 0.0, 1.0, OnsenDelay::kMaxToneHz);

        std::vector<double> noise ((std::size_t) (1.0 * fs));
        Lcg lcg (99);
        for (auto& v : noise) v = 0.8 * lcg.next();
        processMono (e, noise);

        e.prepare (fs, 1); // must drop all state
        std::vector<double> silence ((std::size_t) (0.5 * fs), 0.0);
        processMono (e, silence);
        if (fct::peakAbs (silence) > 1e-12)
            fail ("residue after prepare(): peak " + std::to_string (fct::peakAbs (silence)));
    }

    // -- 8. z-domain tone damping oracle -------------------------------------------

    void testToneDampingZDomain (double fs)
    {
        const double baseMs = 250.0, toneHz = 2000.0, regenParam = 0.8;
        OnsenDelay e;
        configureStatic (e, fs, baseMs, 0.0, regenParam, toneHz);

        std::vector<double> settle ((std::size_t) (0.2 * fs), 0.0);
        processMono (e, settle);

        const double dSamples = baseMs * 0.001 * fs;
        std::vector<double> cap ((std::size_t) (3.4 * dSamples), 0.0);
        cap[0] = 1.0;
        processMono (e, cap);

        // Echo k sits at k*D; window it and probe two frequencies. The ratio
        // echo2/echo1 must equal regen_eff * |H(e^jw)| of the one-pole lowpass
        // y[n] = (1-a) x[n] + a y[n-1], a = exp(-2*pi*fc/fs)  (z-domain, not the
        // analog prototype). At 44.1 kHz (the worst case here) the tone LP
        // leaves an echo1 peak around 1-a ~= 0.25, so the shaper (drive 1.5)
        // applies ~5% tanh compression -- inside the 15% tolerance below.
        const double regenEff = regenParam * OnsenDelay::kMaxRegen;
        const double a = std::exp (-2.0 * kPi * toneHz / fs);
        auto zMag = [&] (double f)
        {
            const double w = 2.0 * kPi * f / fs;
            const double re = 1.0 - a * std::cos (w), im = a * std::sin (w);
            return (1.0 - a) / std::sqrt (re * re + im * im);
        };

        const std::size_t win = 512;
        auto echoWindowStart = [&] (int k)
        {
            const std::size_t center = (std::size_t) std::llround (k * dSamples);
            return center - std::min (center, win / 4); // impulse near window head, tail covered
        };

        for (double f : { 500.0, 6000.0 })
        {
            const double m1 = goertzelMag (cap, echoWindowStart (1), win, f, fs);
            const double m2 = goertzelMag (cap, echoWindowStart (2), win, f, fs);
            const double measured = m2 / std::max (m1, 1e-30);
            const double expected = regenEff * zMag (f);
            if (std::abs (measured / expected - 1.0) > 0.15)
                fail ("tone damping @" + std::to_string ((int) f) + " Hz: echo ratio "
                      + std::to_string (measured) + " expected " + std::to_string (expected));
        }
    }

    void coreTests (double Fs)
    {
        std::printf ("onsen-delay core @ Fs=%.0f\n", Fs);
        testIntervalEchoes (Fs);
        testSequencerSchedule (Fs);
        testDopplerGlide (Fs);
        testFeedbackStability (Fs);
        testLongHoldBounded (Fs);
        testFastSweepLongHoldBounded (Fs);
        testNanRecovery (Fs);
        testResetAndSilence (Fs);
        testToneDampingZDomain (Fs);
    }
}

int main (int argc, char** argv)
{
    // Full standard rate matrix by default; CTest passes one rate as argv[1].
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
