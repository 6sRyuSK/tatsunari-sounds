#pragma once

#include "factory_params/ParamDesc.h"

#include <cmath>

//
// factory_params::Range — a JUCE-free, BIT-EXACT reproduction of the value<->0..1
// conversion math in JUCE's NormalisableRange<float>.
//
// TRANSCRIPTION SOURCE (JUCE 8.0.13, verbatim — operation order, clamping,
// exp/log vs pow, and comparisons preserved), from the JUCE core-maths module
// headers (the framework prefixes each filename with its own tag):
//   NormalisableRange.h — convertTo0to1 / convertFrom0to1 / snapToLegalValue /
//                         setSkewForCentre
//   MathsFunctions.h    — jlimit (and exactlyEqual, here inlined as `== 1.0f`)
// Only the subset the factory uses is reproduced: NO symmetric skew, NO custom
// lambda remaps.
//
// Bit-exact float parity with live JUCE parameter objects is a HARD TEST GATE
// (params_test's Range checks + resonance-suppressor preset_test's "paramdesc
// parity", which compares via ==/memcmp with NO tolerance). DO NOT SIMPLIFY THE
// MATH: every operation stays in float, in this order. Algebraic simplification
// (e.g. pow(x, log(0.5)/log(P)) -> 0.5) changes the rounding and breaks parity.
//
namespace factory_params
{
    // The subset of JUCE's NormalisableRange<float> state the factory uses.
    struct RangeSpec
    {
        float start    = 0.0f;
        float end      = 1.0f;
        float interval = 0.0f;   // 0 = continuous
        float skew     = 1.0f;   // 1 = linear
    };

    // Exact float equality, mirroring JUCE's own exactlyEqual (which the transcribed
    // convertTo0to1/convertFrom0to1 use for the `skew == 1` test). The comparison is
    // deliberately bit-exact, so the -Wfloat-equal warning is locally silenced here
    // exactly as JUCE does for its helper (GCC/Clang only; MSVC has no such warning).
#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
    inline bool exactlyEqual (float a, float b) noexcept { return a == b; }
#if defined(__clang__)
 #pragma clang diagnostic pop
#elif defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

    // JUCE's jlimit (MathsFunctions.h). upperLimit wins the ordering exactly as
    // JUCE writes it. The jassert(lowerLimit <= upperLimit) is a debug-only check
    // that does not alter the result.
    inline float jlimitf (float lowerLimit, float upperLimit, float value) noexcept
    {
        return value < lowerLimit ? lowerLimit
                                  : (upperLimit < value ? upperLimit : value);
    }

    // NormalisableRange::clampTo0To1. JUCE additionally jasserts that the input was
    // already in [0,1]; that assertion (compiled out in Release) does not change
    // the returned, clamped value, so it is intentionally omitted.
    inline float clampTo0To1 (float value) noexcept
    {
        return jlimitf (0.0f, 1.0f, value);
    }

    // NormalisableRange<float>::convertTo0to1 — the non-symmetric-skew path.
    inline float convertTo0to1 (const RangeSpec& r, float v) noexcept
    {
        auto proportion = clampTo0To1 ((v - r.start) / (r.end - r.start));

        if (exactlyEqual (r.skew, 1.0f))
            return proportion;

        return std::pow (proportion, r.skew);
    }

    // NormalisableRange<float>::convertFrom0to1 — the non-symmetric-skew path.
    inline float convertFrom0to1 (const RangeSpec& r, float proportion) noexcept
    {
        proportion = clampTo0To1 (proportion);

        if (! exactlyEqual (r.skew, 1.0f) && proportion > 0.0f)
            proportion = std::exp (std::log (proportion) / r.skew);

        return r.start + (r.end - r.start) * proportion;
    }

    // NormalisableRange<float>::snapToLegalValue — the interval path (no custom lambda).
    inline float snapToLegalValue (const RangeSpec& r, float v) noexcept
    {
        if (r.interval > 0.0f)
            v = r.start + r.interval * std::floor ((v - r.start) / r.interval + 0.5f);

        return (v <= r.start || r.end <= r.start) ? r.start
                                                  : (v >= r.end ? r.end : v);
    }

    // The skew a NormalisableRange<float> built as {start,end,interval} then
    // setSkewForCentre(centre) ends up with — JUCE's exact float expression:
    //   skew = std::log (0.5f) / std::log ((centre - start) / (end - start));
    inline float skewForCentre (float start, float end, float centre) noexcept
    {
        return std::log (0.5f) / std::log ((centre - start) / (end - start));
    }

    // The RangeSpec matching the JUCE NormalisableRange the ApvtsAdapter builds
    // for `d` — so Range's math here and the live JUCE object stay in lock-step.
    inline RangeSpec makeRange (const ParamDesc& d) noexcept
    {
        if (d.type == ParamType::Float)
        {
            const float skew = exactlyEqual (d.skewCentre, 0.0f)
                                   ? 1.0f
                                   : skewForCentre (d.minValue, d.maxValue, d.skewCentre);
            return { d.minValue, d.maxValue, d.interval, skew };
        }

        if (d.type == ParamType::Bool)
            return { 0.0f, 1.0f, 1.0f, 1.0f };

        // Choice: 0..(n-1), interval 1, linear — matches AudioParameterChoice's
        // range (its custom lambdas reduce, for start==0, to the same jlimit(v/end)).
        return { 0.0f, static_cast<float> (d.choices.size()) - 1.0f, 1.0f, 1.0f };
    }

    // Normalised default matching JUCE's RangedAudioParameter::getDefaultValue():
    //   Bool   -> 1 if default > 0.5 else 0          (AudioParameterBool::valueDefault, raw — no convert)
    //   Float  -> convertTo0to1(default)             (AudioParameterFloat)
    //   Choice -> convertTo0to1((float)(int) index)  (AudioParameterChoice::defaultValue)
    //
    // NOTE: for Float and Choice, getDefaultValue() routes through
    // RangedAudioParameter::convertTo0to1, which SNAPS to the legal grid before
    // normalising: range.convertTo0to1(range.snapToLegalValue(v)). The snap must be
    // reproduced here — omitting it diverges by ~1 ulp on a stepped range whose
    // default is not exactly representable on the grid (e.g. the width default 0.50
    // on a 0.01 interval), which the preset_test parity check compares bit-exactly.
    // Bool takes its default verbatim (getDefaultValue() returns valueDefault).
    inline float normalizedDefault (const ParamDesc& d) noexcept
    {
        if (d.type == ParamType::Bool)
            return d.defaultValue > 0.5f ? 1.0f : 0.0f;

        const RangeSpec r = makeRange (d);
        const float def = (d.type == ParamType::Choice)
                              ? static_cast<float> (static_cast<int> (d.defaultValue))
                              : d.defaultValue;
        return convertTo0to1 (r, snapToLegalValue (r, def));
    }
}
