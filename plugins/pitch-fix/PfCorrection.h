#pragma once
//
// plugins/pitch-fix/PfCorrection.h — pure correction-curve math for Pitch
// TatFixer (P4 stage separation: detection / note / correction). Framework-free,
// header-only, real-time safe. PfCore applies the one-pole retune on top.
//
// Stability / Accuracy continuous deadzone (pitch-fix-detection-accuracy.md
// §2.6-3): avoids a jump of (Stability - Accuracy) at the deadzone edge.
//   x = max(0, |err| - Stability)
//   R = max(0, Stability - Accuracy)
//   d = x + min(x, R)
//   dead = sign(err) * d
// Accuracy == Stability → identical to the old Tolerance formula;
// Accuracy == 0 → full pull to the target once outside Stability.
//
#include <algorithm>
#include <cmath>

namespace pf_core
{
    // err = targetCents - detectedCents (same sign convention as PfCore).
    // Returns the deadzoned error BEFORE Amount scaling.
    inline double correctionDeadzone (double err,
                                      double stabilityCt,
                                      double accuracyCt) noexcept
    {
        const double stab = std::clamp (stabilityCt, 0.0, 75.0);
        const double acc  = std::clamp (accuracyCt,  0.0, 75.0);
        const double x = std::max (0.0, std::abs (err) - stab);
        const double R = std::max (0.0, stab - acc);
        const double d = x + std::min (x, R);
        return (err >= 0.0 ? d : -d);
    }

    // Full per-hop correction target in cents (clamped to ±maxShiftCt).
    inline double correctionTargetCents (double err,
                                         double stabilityCt,
                                         double accuracyCt,
                                         double amount01,
                                         double maxShiftCt = 1200.0) noexcept
    {
        const double dead = correctionDeadzone (err, stabilityCt, accuracyCt);
        const double a = std::clamp (amount01, 0.0, 1.5);
        return std::clamp (dead * a, -maxShiftCt, maxShiftCt);
    }
} // namespace pf_core
