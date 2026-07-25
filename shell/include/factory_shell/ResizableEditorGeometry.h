#pragma once
//
// factory_shell/ResizableEditorGeometry.h — the pure, framework-free geometry helpers
// a resizable Visage CLAP editor uses: the aspect-lock + resize-limit SNAP and the
// host-resize RELAY decision. Extracted (verbatim logic) from RsClapEditor.h so every
// resizable clap-first editor — RS, dynamic-eq — shares one implementation, and so the
// RS clap_shell_test's fixed-point oracle guards the shared code. Header-only, depends
// only on <cstdint>/<cmath> (no CLAP, no Visage), so it links into factory_shell and
// the pure shell test without a GUI build.
//
// The snap is PARAMETERISED by an EditorGeometry (design aspect + min/max limits)
// instead of RS's former hardcoded constants; a plugin passes its own numbers and the
// behaviour is otherwise identical to the RS original (the RS call sites pass
// {1069,747, 471,329, 1320,922}, preserving the tested fixed points).
//
#include <cmath>
#include <cstdint>

namespace factory_shell
{
    // A resizable editor's design aspect (designW:designH) + window resize limits, in
    // LOGICAL units (the space the editor lays out in; window units on non-macOS).
    struct EditorGeometry
    {
        double designW, designH; // reference layout — fixes the aspect ratio
        double minW,    minH;    // smallest allowed window
        double maxW,    maxH;    // largest allowed window (the static cap)
    };

    // Snap a host-proposed CLAP size to the design aspect + resize limits.
    //
    // CLAP/VST3 GUI sizes are LOGICAL points on macOS but PHYSICAL pixels on
    // Windows/X11; `scale` is the native-per-logical ratio (1.0 on macOS). The snap
    // runs in LOGICAL space against the [minW,minH .. maxW,maxH] limits and the
    // designW:designH aspect, then converts back to native px. It is width-driven: the
    // width is clamped to its logical limits, the aspect-locked height derived, then the
    // height clamped to its own limits (the limit box's corners are marginally
    // off-aspect, so the height clamp binds at the maximum, giving the maxW x maxH corner
    // exactly). The host advertises preserve_aspect_ratio, so it proposes matching w:h
    // pairs; the width drives.
    //
    // Rounding is settled in logical units first and only then scaled to native, which
    // makes the snap a FIXED POINT: re-applying it to its own output returns the same
    // native size. `scale <= 0` is treated as 1.0. Pure + Visage-free (unit-tested in
    // clap_shell_test).
    //
    // `maxWindowW` / `maxWindowH` are an OPTIONAL dynamic upper bound (0 = unused) in the
    // SAME units as the proposal after the `scale` division — i.e. window units when the
    // caller passes scale 1.0 (the shipping path). They tighten the static cap to
    // whatever the current display can hold, so a height-driven grip drag cannot push the
    // window off-screen. Because the design aspect is fixed, BOTH axes are honoured: when
    // a dynamic limit binds, the width is re-derived from the aspect-clamped height so it
    // cannot overshoot. The static maxW x maxH corner is intentionally the box corner
    // (marginally off-aspect) and is preserved exactly when no dynamic limit binds. If the
    // dynamic max falls below the usable minimum the MINIMUM wins.
    inline void snapEditorSizeForScale (const EditorGeometry& g, double scale,
                                        std::uint32_t& w, std::uint32_t& h,
                                        double maxWindowW = 0.0, double maxWindowH = 0.0) noexcept
    {
        const double kDesignW = g.designW, kDesignH = g.designH;
        const double kMinW = g.minW, kMinH = g.minH, kMaxW = g.maxW, kMaxH = g.maxH;
        if (! (scale > 0.0)) scale = 1.0;

        const auto clampd = [] (double v, double lo, double hi) noexcept
        { return v < lo ? lo : (v > hi ? hi : v); };

        // Effective per-axis logical max: the static cap tightened by the (optional)
        // dynamic display limit. Never let either axis fall below the usable minimum.
        double effMaxW = kMaxW, effMaxH = kMaxH;
        const bool dynW = maxWindowW > 0.0, dynH = maxWindowH > 0.0;
        if (dynW) effMaxW = effMaxW < maxWindowW ? effMaxW : maxWindowW;
        if (dynH) effMaxH = effMaxH < maxWindowH ? effMaxH : maxWindowH;
        if (effMaxW < kMinW) effMaxW = kMinW;
        if (effMaxH < kMinH) effMaxH = kMinH;

        // native -> logical (the proposed height follows the width under the host's
        // aspect lock, so the width drives the snap).
        const double logicalW = static_cast<double> (w) / scale;
        double lw = clampd (logicalW, kMinW, effMaxW);
        double lh = lw * kDesignH / kDesignW;
        lh = clampd (lh, kMinH, effMaxH);
        // When a DYNAMIC limit tightened the cap, re-derive the width from the aspect-
        // clamped height so the width cannot overshoot a short/narrow display (aspect
        // preserved, height-driven). The static-only corner (maxW x maxH) skips this so
        // its marginally-off-aspect box corner is preserved for the existing oracle.
        if ((dynW && effMaxW < kMaxW) || (dynH && effMaxH < kMaxH))
        {
            const double wForHeight = lh * kDesignW / kDesignH;
            lw = lw < wForHeight ? lw : wForHeight;
            if (lw < kMinW) lw = kMinW;
        }

        // Settle the logical rounding, THEN scale back to native (fixed-point rule).
        const long lwi = std::lround (lw);
        const long lhi = std::lround (lh);
        w = static_cast<std::uint32_t> (std::lround (static_cast<double> (lwi) * scale));
        h = static_cast<std::uint32_t> (std::lround (static_cast<double> (lhi) * scale));
    }

    // Decide whether an OS-driven "window contents resized" notification should be
    // relayed to the host as a gui.request_resize.
    //
    // Relaying UNCONDITIONALLY is what closed the Logic AU stack-overflow loop (the
    // windowContentsResized -> request_resize -> host setFrame -> setSize -> ... cycle
    // that never settled to a fixed point). The cure is to relay ONLY a size the host
    // does not already know about: relay when the window's host-facing size actually
    // CHANGED from what the host last gave us, and NEVER while we are inside a host-driven
    // setSize (`inSetSize` — the host set that size, echoing it back is what re-enters).
    // With both guards, feeding the current size straight back in is a no-op, so the
    // callback chain is a fixed point that terminates in one pass. Pure + Visage-free
    // (unit-tested in clap_shell_test).
    inline bool shouldRelayHostResize (std::uint32_t curW, std::uint32_t curH,
                                       std::uint32_t newW, std::uint32_t newH,
                                       bool inSetSize) noexcept
    {
        if (inSetSize)              return false; // host is the origin of this change
        if (newW == 0 || newH == 0) return false; // degenerate / not yet realised
        return newW != curW || newH != curH;      // relay only a genuine change
    }
} // namespace factory_shell
