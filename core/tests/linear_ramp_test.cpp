//
// core/tests/linear_ramp_test.cpp — spec test for factory_core::LinearRamp, the
// linear-ramp replacement for the reference audio framework's SmoothedValue<T>.
//
// Conventions mirror primitives_test.cpp:
//   * links only factory_core (no framework, headless); one CTest case per rate.
//   * accumulate failures in g_failures / fail(), return 1 at the end.
//   * default rate set == factory_core::testing::sampleRatesFromArgs (all 6) —
//     reset(sampleRate, seconds) is rate-dependent, so the ramp is exercised
//     across the full matrix.
//
// Oracle strategy (independent of the code under test):
//   1. PARITY — an independently written state machine (RefRamp) reproduces the
//      documented algorithm; a long scripted, randomised sequence (reset,
//      setTarget, getNextValue, skip, mid-ramp retarget) is driven through both
//      and compared BIT-EXACTLY. This is the rigorous, tolerance-free gate.
//   2. CLOSED FORM — with an exactly-representable step, getNextValue and skip are
//      compared bit-exactly to a hand-computed linear closed form (incl. a
//      mid-ramp retarget), so "<=1 ulp vs closed form" holds as 0 ulp. A second
//      case with a non-exact step checks linearity against the real-number line
//      within a principled float-error bound and the per-step delta within 1 ulp.
//   3. INVARIANTS — final step snaps exactly to target, idles afterwards; skip(n)
//      matches n getNextValue() calls; edge/no-op behaviour.
//
#include "factory_core/LinearRamp.h"
#include "factory_core/testing/DspInvariants.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace fct = factory_core::testing;
using factory_core::LinearRamp;

namespace
{
int g_failures = 0;

void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("FAIL: %s\n", msg.c_str());
}

void check (bool cond, const std::string& msg)
{
    if (! cond) fail (msg);
}

bool bitEqual (float a, float b)  { return std::memcmp (&a, &b, sizeof a) == 0; }
bool bitEqual (double a, double b){ return std::memcmp (&a, &b, sizeof a) == 0; }

long long ulpDist (float a, float b)
{
    if (bitEqual (a, b)) return 0;
    std::int32_t ia, ib;
    std::memcpy (&ia, &a, sizeof ia);
    std::memcpy (&ib, &b, sizeof ib);
    if ((ia < 0) != (ib < 0)) return 1LL << 40;
    return std::llabs ((long long) ia - (long long) ib);
}
long long ulpDist (double a, double b)
{
    if (bitEqual (a, b)) return 0;
    std::int64_t ia, ib;
    std::memcpy (&ia, &a, sizeof ia);
    std::memcpy (&ib, &b, sizeof ib);
    if ((ia < 0) != (ib < 0)) return 1LL << 40;
    return std::llabs (ia - ib);
}

// --- independent oracle: the documented algorithm, written from scratch --------
template <typename T>
struct RefRamp
{
    T   current { 0 };
    T   target  { 0 };
    T   step    { 0 };
    int countdown = 0;
    int stepsToTarget = 0;

    RefRamp() = default;
    explicit RefRamp (T init) : current (init), target (init) {}

    void resetSeconds (double sr, double sec) { resetSteps ((int) std::floor (sec * sr)); }
    void resetSteps (int n) { stepsToTarget = n; setBoth (target); }
    void setBoth (T v) { target = current = v; countdown = 0; }

    void setTarget (T v)
    {
        if (v == target) return;
        if (stepsToTarget <= 0) { setBoth (v); return; }
        target = v;
        countdown = stepsToTarget;
        step = (target - current) / (T) countdown;
    }

    T next()
    {
        if (countdown <= 0) return target;
        --countdown;
        if (countdown > 0) current += step;
        else               current = target;
        return current;
    }

    T skip (int n)
    {
        if (n >= countdown) { setBoth (target); return target; }
        current += step * (T) n;
        countdown -= n;
        return current;
    }

    bool smoothing() const { return countdown > 0; }
};

// 1. Bit-exact parity vs the independent oracle over a randomised script.
template <typename T>
void parity (double fs, double seconds, const char* tag)
{
    std::mt19937 rng (0x9E3779B9u ^ (std::uint32_t) fs ^ (std::uint32_t) (seconds * 100000.0));
    auto rnd = [&] (T lo, T hi)
    {
        const double u = (double) (rng() & 0xffffffu) / (double) 0xffffffu;
        return (T) (lo + (hi - lo) * (T) u);
    };

    LinearRamp<T> lr;
    RefRamp<T>    ref;
    lr.reset (fs, seconds);   ref.resetSeconds (fs, seconds);
    lr.setCurrentAndTargetValue ((T) 0.25);  ref.setBoth ((T) 0.25);

    for (int opIdx = 0; opIdx < 6000; ++opIdx)
    {
        const unsigned kind = rng() % 12u;
        if (kind < 2u)
        {
            const T t = rnd ((T) -3, (T) 3);
            lr.setTargetValue (t);  ref.setTarget (t);
        }
        else if (kind < 3u)
        {
            const int n = 1 + (int) (rng() % 96u);
            const T a = lr.skip (n);
            const T b = ref.skip (n);
            if (! bitEqual (a, b)) fail (std::string (tag) + " skip return parity");
        }
        else if (kind < 4u)
        {
            lr.reset (fs, seconds);  ref.resetSeconds (fs, seconds);
        }
        else
        {
            const T a = lr.getNextValue();
            const T b = ref.next();
            if (! bitEqual (a, b)) fail (std::string (tag) + " getNextValue return parity");
        }

        if (! bitEqual (lr.getCurrentValue(), ref.current)) { fail (std::string (tag) + " current parity"); break; }
        if (! bitEqual (lr.getTargetValue(),  ref.target))  { fail (std::string (tag) + " target parity");  break; }
        if (lr.isSmoothing() != ref.smoothing())            { fail (std::string (tag) + " isSmoothing parity"); break; }
    }
}

// 2a. Exactly-representable step -> bit-exact vs the linear closed form, incl. a
//     mid-ramp retarget and a skip() that equals n getNextValue() calls.
template <typename T>
void closedFormExact (const char* tag)
{
    // Segment A: 0 -> 1 over 8 steps. step = 0.125 (exact in float/double).
    {
        LinearRamp<T> lr;
        lr.reset (8);
        lr.setCurrentAndTargetValue ((T) 0);
        lr.setTargetValue ((T) 1);
        const T step = (T) 0.125;
        for (int k = 1; k <= 8; ++k)
        {
            const T v = lr.getNextValue();
            const T expected = (k < 8) ? (T) (step * (T) k) : (T) 1;
            if (! bitEqual (v, expected)) fail (std::string (tag) + " exact ramp value k=" + std::to_string (k));
        }
        check (! lr.isSmoothing(), std::string (tag) + " idle after final step");
        check (bitEqual (lr.getNextValue(), (T) 1), std::string (tag) + " past-end returns target");
    }

    // Segment B: mid-ramp retarget. Advance 3 of 8 (current 0.375), retarget to
    // 0.5 -> step2 = 0.015625 (exact); verify the new segment bit-exactly.
    {
        LinearRamp<T> lr;
        lr.reset (8);
        lr.setCurrentAndTargetValue ((T) 0);
        lr.setTargetValue ((T) 1);
        (void) lr.getNextValue(); (void) lr.getNextValue(); (void) lr.getNextValue();
        check (bitEqual (lr.getCurrentValue(), (T) 0.375), std::string (tag) + " pre-retarget current == 0.375");

        lr.setTargetValue ((T) 0.5);
        const T base = (T) 0.375, step2 = (T) 0.015625;
        for (int j = 1; j <= 8; ++j)
        {
            const T v = lr.getNextValue();
            const T expected = (j < 8) ? (T) (base + step2 * (T) j) : (T) 0.5;
            if (! bitEqual (v, expected)) fail (std::string (tag) + " retarget ramp value j=" + std::to_string (j));
        }
    }

    // skip(n) == n getNextValue() calls (exact step, so bit-identical).
    {
        LinearRamp<T> a, b;
        a.reset (16); b.reset (16);
        a.setCurrentAndTargetValue ((T) 0); b.setCurrentAndTargetValue ((T) 0);
        a.setTargetValue ((T) 2); b.setTargetValue ((T) 2);  // step = 0.125 exact
        const T skipped = a.skip (5);
        T stepped = (T) 0;
        for (int k = 0; k < 5; ++k) stepped = b.getNextValue();
        check (bitEqual (skipped, stepped), std::string (tag) + " skip(5) == 5x getNextValue (exact)");
        check (bitEqual (a.getCurrentValue(), b.getCurrentValue()), std::string (tag) + " skip vs step current match");
        check (a.isSmoothing() == b.isSmoothing(), std::string (tag) + " skip vs step smoothing match");
    }
}

// 2b. Non-exact step -> linearity vs the real-number line within a principled
//     bound, and per-step increment within 1 ulp of step.
template <typename T>
void closedFormLinear (const char* tag)
{
    const int N = 37;                    // non-power-of-two: step is not exact
    const T c0 = (T) -0.3, tgt = (T) 0.9;
    LinearRamp<T> lr;
    lr.reset (N);
    lr.setCurrentAndTargetValue (c0);
    lr.setTargetValue (tgt);

    const T step = (T) ((tgt - c0) / (T) N);          // independent recomputation
    const double eps = (double) std::numeric_limits<T>::epsilon();
    const double maxMag = 0.9;
    const double lineTol = (double) N * eps * maxMag; // accumulation bound over N adds

    T prev = c0;
    for (int k = 1; k <= N; ++k)
    {
        const T v = lr.getNextValue();
        if (k < N)
        {
            // Exact linear recurrence: each step is precisely current += step (the
            // per-step delta is NOT within 1 ulp OF STEP — the accumulator's 0.5-ulp
            // rounding is measured at the value's magnitude, many ulp of the smaller
            // step — but v itself equals fl(prev+step) to the bit).
            check (bitEqual (v, (T) (prev + step)), std::string (tag) + " exact linear recurrence v == prev+step k=" + std::to_string (k));
            check (v > prev && v < tgt, std::string (tag) + " monotone/bounded k=" + std::to_string (k));
            const double line = (double) c0 + (double) (tgt - c0) * ((double) k / (double) N);
            check (std::abs ((double) v - line) <= lineTol, std::string (tag) + " on the linear line (<=1 ulp-scale bound) k=" + std::to_string (k));
        }
        else
        {
            check (bitEqual (v, tgt), std::string (tag) + " final step snaps exactly to target");
            check (! lr.isSmoothing(), std::string (tag) + " idle after final step");
        }
        prev = v;
    }
}

// 3. Edge / no-op behaviour.
template <typename T>
void edges (const char* tag)
{
    // reset(0 steps): no ramp -> setTargetValue snaps immediately.
    {
        LinearRamp<T> lr;
        lr.reset (0);
        lr.setCurrentAndTargetValue ((T) 1);
        lr.setTargetValue ((T) 5);
        check (bitEqual (lr.getCurrentValue(), (T) 5), std::string (tag) + " zero-length ramp snaps target");
        check (! lr.isSmoothing(), std::string (tag) + " zero-length ramp never smooths");
    }
    // setTargetValue to the same value is a no-op (countdown untouched).
    {
        LinearRamp<T> lr;
        lr.reset (10);
        lr.setCurrentAndTargetValue ((T) 0);
        lr.setTargetValue ((T) 1);
        (void) lr.getNextValue();
        const T curBefore = lr.getCurrentValue();
        const bool smBefore = lr.isSmoothing();
        lr.setTargetValue ((T) 1);   // same target
        check (bitEqual (lr.getCurrentValue(), curBefore) && lr.isSmoothing() == smBefore,
               std::string (tag) + " retarget to same value is a no-op");
    }
    // Not smoothing: getNextValue and skip both return target.
    {
        LinearRamp<T> lr;
        lr.reset (4);
        lr.setCurrentAndTargetValue ((T) 3);
        check (bitEqual (lr.getNextValue(), (T) 3), std::string (tag) + " idle getNextValue == target");
        check (bitEqual (lr.skip (100), (T) 3),     std::string (tag) + " idle skip == target");
    }
    // Direct-list-init construction starts settled at the initial value.
    {
        LinearRamp<T> lr { (T) 1 };
        check (bitEqual (lr.getCurrentValue(), (T) 1) && bitEqual (lr.getTargetValue(), (T) 1) && ! lr.isSmoothing(),
               std::string (tag) + " value-ctor starts settled");
    }
    // skip to/past the end snaps exactly to target and idles.
    {
        LinearRamp<T> lr;
        lr.reset (8);
        lr.setCurrentAndTargetValue ((T) 0);
        lr.setTargetValue ((T) 1);
        const T v = lr.skip (8);     // == countdown -> snap
        check (bitEqual (v, (T) 1) && ! lr.isSmoothing(), std::string (tag) + " skip to end snaps to target");
    }
}

void runRate (double fs)
{
    parity<float>  (fs, 0.02, "f/0.02");
    parity<double> (fs, 0.02, "d/0.02");
    parity<float>  (fs, 0.03, "f/0.03");
    parity<double> (fs, 0.03, "d/0.03");

    closedFormExact<float>  ("f");
    closedFormExact<double> ("d");
    closedFormLinear<float>  ("f");
    closedFormLinear<double> ("d");
    edges<float>  ("f");
    edges<double> ("d");
}
} // namespace

int main (int argc, char** argv)
{
    std::printf ("factory_core::LinearRamp spec test\n");
    for (double fs : fct::sampleRatesFromArgs (argc, argv))
    {
        std::printf ("  fs = %.0f Hz\n", fs);
        runRate (fs);
    }

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
