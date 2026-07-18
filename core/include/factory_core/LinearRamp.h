#pragma once
//
// factory_core/LinearRamp.h — a header-only, allocation-free, dependency-free
// LINEAR parameter ramp. It is a drop-in replacement for the reference audio
// framework's linear SmoothedValue<T> (ValueSmoothingTypes::Linear), reproducing
// its exact per-sample arithmetic so a call site that swaps one for the other
// renders bit-identically (the migration's DSP-identity gate). Only the linear
// smoothing type is provided — the multiplicative variant is intentionally out
// of scope (no factory call site uses it).
//
// Real-time safe: every method is noexcept, branch-only, and never allocates,
// locks, or makes a syscall — safe to call from processBlock.
//
// SEMANTICS (transcribed operation-for-operation from the reference framework so
// the trajectories match to the bit; do NOT "simplify" the accumulation):
//   * reset(sampleRate, rampSeconds) -> reset(floor(rampSeconds*sampleRate)).
//     floor (NOT round) mirrors the framework's (int) std::floor conversion — the
//     two agree for every factory ramp length at every standard rate (the
//     products are exact integers), and floor is what a bit-identical swap needs.
//   * reset(numSteps) latches the ramp length and snaps current==target (idle).
//   * setTargetValue(v): a no-op if v equals the current target (EXACT equality —
//     a deliberate, documented divergence from the framework's approximatelyEqual
//     guard; call sites feed deterministic, block-stable targets so the exact test
//     fires identically, and the trajectories converge regardless). With no ramp
//     length latched it snaps; otherwise it arms countdown and fixes the per-step
//     increment step=(target-current)/countdown ONCE (never re-divided per step).
//   * getNextValue() advances one step by accumulation (current+=step) and snaps
//     EXACTLY to target on the final step; past the end it returns target.
//   * skip(n) advances n steps as a single current+=step*n (the framework's own
//     shortcut — so it matches the framework bit-for-bit, but is only APPROXIMATELY
//     equal to n separate getNextValue() calls, which round n times); skipping to
//     or past the end snaps to target.
//
#include <cmath>

namespace factory_core
{
    template <typename T>
    class LinearRamp
    {
    public:
        LinearRamp() noexcept = default;

        // Direct-list-init construction (LinearRamp<double> g { 1.0 };) seeds the
        // ramp already settled at initialValue, like the framework's value ctor.
        explicit LinearRamp (T initialValue) noexcept
            : currentValue (initialValue), targetValue (initialValue)
        {
        }

        // Set the ramp length from a duration. floor matches the reference
        // framework exactly (see the header note on floor vs round).
        void reset (double sampleRate, double rampSeconds) noexcept
        {
            reset (static_cast<int> (std::floor (rampSeconds * sampleRate)));
        }

        // Set the ramp length directly in samples; snaps current==target and idles.
        void reset (int numSteps) noexcept
        {
            stepsToTarget = numSteps;
            setCurrentAndTargetValue (targetValue);
        }

        void setCurrentAndTargetValue (T newValue) noexcept
        {
            targetValue   = currentValue = newValue;
            countdownValue = 0;
        }

        void setTargetValue (T newValue) noexcept
        {
            if (equalExact (newValue, targetValue))
                return;

            if (stepsToTarget <= 0)
            {
                setCurrentAndTargetValue (newValue);
                return;
            }

            targetValue    = newValue;
            countdownValue = stepsToTarget;
            step           = (targetValue - currentValue) / static_cast<T> (countdownValue);
        }

        T getNextValue() noexcept
        {
            if (! isSmoothing())
                return targetValue;

            --countdownValue;

            if (isSmoothing())
                currentValue += step;
            else
                currentValue = targetValue;

            return currentValue;
        }

        // Advance n samples at once. Identical in spirit to n getNextValue() calls
        // (and bit-identical to the reference framework's own skip), returning the
        // new current value; skipping to/past the end snaps exactly to target.
        T skip (int numSamples) noexcept
        {
            if (numSamples >= countdownValue)
            {
                setCurrentAndTargetValue (targetValue);
                return targetValue;
            }

            currentValue   += step * static_cast<T> (numSamples);
            countdownValue -= numSamples;
            return currentValue;
        }

        bool isSmoothing()      const noexcept { return countdownValue > 0; }
        T    getCurrentValue()  const noexcept { return currentValue; }
        T    getTargetValue()   const noexcept { return targetValue; }
        int  getStepsToTarget() const noexcept { return stepsToTarget; }

    private:
        // Exact float equality for the retarget guard. Bit-exact by design, so the
        // -Wfloat-equal warning is locally silenced exactly as the shared Range
        // helper does (GCC/Clang only; MSVC has no such warning).
#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
        static bool equalExact (T a, T b) noexcept { return a == b; }
#if defined(__clang__)
 #pragma clang diagnostic pop
#elif defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

        T   currentValue   { 0 };
        T   targetValue    { 0 };
        T   step           { 0 };
        int countdownValue = 0;   // samples remaining in the active ramp (0 == idle)
        int stepsToTarget  = 0;   // latched ramp length in samples
    };
}
