#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "RsTheme.h"

//
// rs::AnalyzerStyle — Phase P3a: the settings "engine" behind the analyzer's
// trace rendering (Input/PRE spectrum, a minimal POST-line style, the Delta/
// reduction "curtain", the combined + per-node reduction-profile curve, and a
// couple of smoothness knobs), so a later DEV panel (P3b) can blend the look
// live between the shipped v2.0.1 chrome and the "4a" demo mockup
// (rs-ui-work/demo-analysis.md SS3.2) without SuppressionCurveComponent.h
// hard-coding either look. Field set + endpoint values are transcribed from
// spec-P3.md's "AnalyzerStyle スキーマ" table -- that document is the source
// of truth here, same convention as RsTheme.h for the P1 chrome.
//
// `kV201Style` reproduces the CURRENT (pre-P3a) hardcoded
// SuppressionCurveComponent rendering EXACTLY -- every default member
// initializer below IS today's magic number, so `style == kV201Style` (the
// default) must render bit-for-bit the same as before this struct existed.
// `kDemoStyle` carries the "4a" demo's values; its Smooth-group numbers
// (tempoSmoothingMs/freqSmoothingOct) are first-guess design values, not
// measurements (the demo is a static mockup with no runtime smoothing at all
// -- see demo-analysis SS3.7), and P3b is where they get exercised/tuned live.
//
// Header-only, GUI-thread only (styling data + a pure lerp -- no
// juce::Component/paint code lives here).
//
namespace rs
{
    struct AnalyzerStyle
    {
        // ------------------------------------------------ Input (PRE spectrum)
        // Only one mode exists today (a vertical-gradient filled area, closed
        // to the plot's bottom edge) -- kept as an enum, not a bool, so a
        // future DEV mode (e.g. outline-only) extends the schema instead of
        // breaking it. Both v2.0.1 and the demo use FilledArea.
        enum class InputMode { FilledArea };

        InputMode    inputMode         = InputMode::FilledArea;
        float        inputFillTopAlpha = 0.22f;                  // gradient alpha at the plot top
        float        inputFillBotAlpha = 0.02f;                  // gradient alpha at the plot foot
        juce::Colour inputColour       = colour::textMuted();    // #b9a39b (v2.0.1); demo: mauve #efe0d9
        float        inputLineWidth    = 0.0f;                   // 0 = fill only (both endpoints today)
        float        inputLineAlpha    = 0.0f;

        // ------------------------------------ minimal POST (output) line style
        // The output spectrum is a stroke only (no fill) in both endpoints;
        // not a distinct "layer" in spec-P3.md's schema table, just enough to
        // move its two current magic numbers (0.85 alpha, 1.4 width) into the
        // style so kDemoStyle can carry the demo's w2/opaque/round-join look.
        juce::Colour postColour    = colour::accent(); // #ff7a6b (same hue in both endpoints)
        float        postLineWidth = 1.4f;
        float        postLineAlpha = 0.85f;

        // --------------------------------- Delta (live reduction "curtain")
        // AreaFromZero: hangs from the 0 dB gridline (v2.0.1, current look).
        // CurtainFromTop: hangs from the plot's physical top edge, matching
        // the demo's Layer 2 (`M 0 0 ... Z`), and can saturate at
        // `deltaClampFrac` of the plot height (the demo clamps at
        // y<=150/300 = 50%; demo-analysis SS3.2 Layer 2). The curtain's teal
        // hue is fixed (`kTeal` in SuppressionCurveComponent.h) in both
        // endpoints -- deliberately not a style field.
        enum class DeltaMode { AreaFromZero, CurtainFromTop };

        DeltaMode deltaMode        = DeltaMode::AreaFromZero;
        float     deltaFillAlpha   = 0.28f;
        float     deltaStrokeAlpha = 0.8f;
        float     deltaStrokeWidth = 1.0f;
        // Fraction of the plot height the curtain may hang before it is
        // flattened (visually saturates). 1.0 == effectively unclamped: only
        // the pre-existing -60 dB numeric floor applies (unchanged from
        // before this struct existed) -- see drawReduction()'s comment.
        float deltaClampFrac = 1.0f;

        // --------------------------- Combined-curve (+ per-node) "Curve" face
        // PerNodePlusCombined: v2.0.1's modern-EQ look -- each active node's
        //   own translucent fill+stroke, under one glowing combined-response
        //   stroke.
        // CombinedOnly: the demo's look -- only the single dashed combined
        //   curve, no per-node curves at all (demo-analysis SS3.2 Layer 4 /
        //   SS5.2 "no per-node individual curves").
        // PerNodeFillsOnly: per-node fills/strokes with no combined curve on
        //   top -- completes the 3-way choice the DEV panel (P3b) exposes.
        enum class CurveMode { PerNodePlusCombined, CombinedOnly, PerNodeFillsOnly };

        CurveMode curveMode         = CurveMode::PerNodePlusCombined;
        float     perNodeFillAlpha   = 0.12f;
        float     perNodeStrokeAlpha = 0.7f;
        // 0 (or combinedGlowWidth <= 0) skips the glow pass entirely.
        float combinedGlowAlpha   = 0.22f;
        float combinedGlowWidth   = 5.0f;
        float combinedStrokeWidth = 2.2f;
        float combinedStrokeAlpha = 1.0f;
        // Dash pattern for the combined stroke: 0 == solid (v2.0.1); >0 draws
        // a dashed stroke (`combinedDashLen` on, `combinedDashGap` off), e.g.
        // the demo's 5/4 (demo-analysis SS3.2 Layer 4).
        float        combinedDashLen = 0.0f;
        float        combinedDashGap = 0.0f;
        juce::Colour combinedColour  = colour::accent(); // #ff7a6b; demo: muted #e08a7f

        // --------------------------------------------------------- Smoothness
        // Temporal display smoothing (core's setDisplaySmoothingMs, in ms).
        // NOT wired to the processor in P3a -- the core's existing 50 ms
        // default stays in force regardless of this value; P3b adds the live
        // atomic setter (spec-P3.md "processor 変更" section). Kept in the
        // schema now so the DEV panel's four-group layout (Input/Delta/Curve/
        // Smooth) is already complete.
        float tempoSmoothingMs = 50.0f;
        // 1/N-octave moving-average smoothing applied to the PRE/POST/delta
        // bin arrays before they're traced (0 == raw, i.e. today's behaviour
        // exactly). See SuppressionCurveComponent::smoothOctaveBins().
        float freqSmoothingOct = 0.0f;
        // true: sample the combined/profile curve densely (current, ~1 point
        // per pixel -- reads as a smooth curve purely from point density, no
        // actual spline math). false: sample it with a fixed, sparser point
        // count and straight segments, closer to the demo's hand-authored,
        // visibly-faceted polyline (demo-analysis SS3.7: the demo has no
        // curve-smoothing math at all, only sparse points + straight `L`
        // segments).
        bool pathProfileCurved = true;
        // JointStyle for stroked spectra trace lines (the PRE outline when
        // inputLineWidth > 0, and the POST line): false = mitered (today's
        // implicit default -- the current code never sets a JointStyle), true
        // = curved/round join, matching the demo's `stroke-linejoin="round"`
        // on its POST curve (Layer 3).
        bool traceRoundJoin = false;
    };

    // v2.0.1 == every field's default above; spelled out as its own constant
    // (rather than every caller writing `AnalyzerStyle{}`) so a future edit to
    // a default value can't silently drift the "current look" reference point.
    inline const AnalyzerStyle kV201Style {};

    namespace detail
    {
        // Two demo-only hues (rs-ui-work/demo-analysis.md SS1.3) with no
        // rs::colour role of their own -- RsTheme.h is out of scope for this
        // phase, so these stay local, same pattern as the
        // kHighCutRing/kGrBg/kGrBorder/kGrText constants in
        // SuppressionCurveComponent.h.
        inline juce::Colour demoMauve()      { return juce::Colour (0xffefe0d9); } // PRE fill
        inline juce::Colour demoMutedCoral() { return juce::Colour (0xffe08a7f); } // combined dashed stroke

        inline AnalyzerStyle makeDemoStyle()
        {
            AnalyzerStyle s;
            s.inputMode         = AnalyzerStyle::InputMode::FilledArea;
            s.inputFillTopAlpha = 0.55f;
            s.inputFillBotAlpha = 0.55f; // ~flat -- demo Layer 1 has almost no vertical gradient
            s.inputColour       = demoMauve();
            s.inputLineWidth    = 0.0f;
            s.inputLineAlpha    = 0.0f;

            s.postColour    = colour::accent(); // #ff7a6b, opaque (demo-analysis Layer 3)
            s.postLineWidth = 2.0f;
            s.postLineAlpha = 1.0f;

            s.deltaMode        = AnalyzerStyle::DeltaMode::CurtainFromTop;
            s.deltaFillAlpha   = 0.34f;
            s.deltaStrokeAlpha = 0.0f;
            s.deltaStrokeWidth = 0.0f;
            s.deltaClampFrac   = 0.5f;

            s.curveMode           = AnalyzerStyle::CurveMode::CombinedOnly;
            s.perNodeFillAlpha    = 0.0f;
            s.perNodeStrokeAlpha  = 0.0f;
            s.combinedGlowAlpha   = 0.0f;
            s.combinedGlowWidth   = 0.0f;
            s.combinedStrokeWidth = 1.5f;
            s.combinedStrokeAlpha = 1.0f;
            s.combinedDashLen     = 5.0f;
            s.combinedDashGap     = 4.0f;
            s.combinedColour      = demoMutedCoral();

            // Design values, not measurements -- demo-analysis SS3.7 confirms
            // the mockup has no runtime smoothing at all (static SVG), so
            // these are a first tuning guess aimed at the demo's *look*
            // (sparse points + round joins); P3b is where they get exercised
            // and adjusted by ear/eye.
            s.tempoSmoothingMs  = 20.0f;
            s.freqSmoothingOct  = 1.0f / 6.0f;
            s.pathProfileCurved = false;
            s.traceRoundJoin    = true;
            return s;
        }
    } // namespace detail

    inline const AnalyzerStyle kDemoStyle = detail::makeDemoStyle();

    // Continuous fields interpolate linearly; enum/bool fields snap to `b`
    // once t reaches the midpoint (spec-P3.md: "enum/boolは t<0.5 で a").
    inline AnalyzerStyle lerp (const AnalyzerStyle& a, const AnalyzerStyle& b, float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        const bool useB = t >= 0.5f;

        auto f = [t] (float x, float y) { return x + (y - x) * t; };
        auto c = [t] (juce::Colour x, juce::Colour y) { return x.interpolatedWith (y, t); };

        AnalyzerStyle r;

        r.inputMode         = useB ? b.inputMode : a.inputMode;
        r.inputFillTopAlpha = f (a.inputFillTopAlpha, b.inputFillTopAlpha);
        r.inputFillBotAlpha = f (a.inputFillBotAlpha, b.inputFillBotAlpha);
        r.inputColour       = c (a.inputColour, b.inputColour);
        r.inputLineWidth    = f (a.inputLineWidth, b.inputLineWidth);
        r.inputLineAlpha    = f (a.inputLineAlpha, b.inputLineAlpha);

        r.postColour    = c (a.postColour, b.postColour);
        r.postLineWidth = f (a.postLineWidth, b.postLineWidth);
        r.postLineAlpha = f (a.postLineAlpha, b.postLineAlpha);

        r.deltaMode        = useB ? b.deltaMode : a.deltaMode;
        r.deltaFillAlpha   = f (a.deltaFillAlpha, b.deltaFillAlpha);
        r.deltaStrokeAlpha = f (a.deltaStrokeAlpha, b.deltaStrokeAlpha);
        r.deltaStrokeWidth = f (a.deltaStrokeWidth, b.deltaStrokeWidth);
        r.deltaClampFrac   = f (a.deltaClampFrac, b.deltaClampFrac);

        r.curveMode           = useB ? b.curveMode : a.curveMode;
        r.perNodeFillAlpha    = f (a.perNodeFillAlpha, b.perNodeFillAlpha);
        r.perNodeStrokeAlpha  = f (a.perNodeStrokeAlpha, b.perNodeStrokeAlpha);
        r.combinedGlowAlpha   = f (a.combinedGlowAlpha, b.combinedGlowAlpha);
        r.combinedGlowWidth   = f (a.combinedGlowWidth, b.combinedGlowWidth);
        r.combinedStrokeWidth = f (a.combinedStrokeWidth, b.combinedStrokeWidth);
        r.combinedStrokeAlpha = f (a.combinedStrokeAlpha, b.combinedStrokeAlpha);
        r.combinedDashLen     = f (a.combinedDashLen, b.combinedDashLen);
        r.combinedDashGap     = f (a.combinedDashGap, b.combinedDashGap);
        r.combinedColour      = c (a.combinedColour, b.combinedColour);

        r.tempoSmoothingMs  = f (a.tempoSmoothingMs, b.tempoSmoothingMs);
        r.freqSmoothingOct  = f (a.freqSmoothingOct, b.freqSmoothingOct);
        r.pathProfileCurved = useB ? b.pathProfileCurved : a.pathProfileCurved;
        r.traceRoundJoin    = useB ? b.traceRoundJoin : a.traceRoundJoin;

        return r;
    }

    // Per-face blend for the DEV panel: each of the four faces (Input / Delta /
    // Curve / Smooth) interpolates kV201Style<->kDemoStyle at its OWN t, so e.g.
    // the delta curtain can be pure-demo while the combined curve stays v2.0.1.
    // POST-line fields ride the Input face (both are spectrum traces); the
    // combined + per-node fields ride the Curve face. traceRoundJoin rides Smooth.
    inline AnalyzerStyle composePerFace (float tInput, float tDelta, float tCurve, float tSmooth)
    {
        const auto li = lerp (kV201Style, kDemoStyle, tInput);
        const auto ld = lerp (kV201Style, kDemoStyle, tDelta);
        const auto lc = lerp (kV201Style, kDemoStyle, tCurve);
        const auto ls = lerp (kV201Style, kDemoStyle, tSmooth);
        AnalyzerStyle r = kV201Style;
        // Input face (+ POST line)
        r.inputMode = li.inputMode; r.inputFillTopAlpha = li.inputFillTopAlpha; r.inputFillBotAlpha = li.inputFillBotAlpha;
        r.inputColour = li.inputColour; r.inputLineWidth = li.inputLineWidth; r.inputLineAlpha = li.inputLineAlpha;
        r.postColour = li.postColour; r.postLineWidth = li.postLineWidth; r.postLineAlpha = li.postLineAlpha;
        // Delta face
        r.deltaMode = ld.deltaMode; r.deltaFillAlpha = ld.deltaFillAlpha; r.deltaStrokeAlpha = ld.deltaStrokeAlpha;
        r.deltaStrokeWidth = ld.deltaStrokeWidth; r.deltaClampFrac = ld.deltaClampFrac;
        // Curve face (combined + per-node)
        r.curveMode = lc.curveMode; r.perNodeFillAlpha = lc.perNodeFillAlpha; r.perNodeStrokeAlpha = lc.perNodeStrokeAlpha;
        r.combinedGlowAlpha = lc.combinedGlowAlpha; r.combinedGlowWidth = lc.combinedGlowWidth;
        r.combinedStrokeWidth = lc.combinedStrokeWidth; r.combinedStrokeAlpha = lc.combinedStrokeAlpha;
        r.combinedDashLen = lc.combinedDashLen; r.combinedDashGap = lc.combinedDashGap; r.combinedColour = lc.combinedColour;
        // Smooth face
        r.tempoSmoothingMs = ls.tempoSmoothingMs; r.freqSmoothingOct = ls.freqSmoothingOct;
        r.pathProfileCurved = ls.pathProfileCurved; r.traceRoundJoin = ls.traceRoundJoin;
        return r;
    }
} // namespace rs
