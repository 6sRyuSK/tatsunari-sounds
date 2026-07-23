#pragma once
//
// RsClapEditor.h — the framework-free factory the RS Policy hands the generic shell
// to build the CLAP editor. The DECLARATION is deliberately Visage-free (only the
// factory_shell seam + CLAP host type + forward-declared model refs), so
// ClapEntry.cpp — which composes the headless Policy — includes it WITHOUT pulling
// in any GUI framework. The visage-backed definition lives in RsClapEditor.cpp and
// is the single translation unit in the whole RS CLAP build that links Visage; it
// is compiled ONLY under FACTORY_RS_CLAP_GUI.
//
#include "factory_shell/ClapEditor.h"

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace rs_core         { class RsCore; }

namespace rs_shell
{
    // Snap a host-proposed CLAP size to the RS design aspect and resize limits.
    //
    // CLAP/VST3 GUI sizes are LOGICAL points on macOS but PHYSICAL pixels on
    // Windows/X11; `scale` is the native-per-logical ratio (1.0 on macOS). The snap
    // runs in LOGICAL space — the space the editor lays out in — against the resize
    // limits [471x329 .. 1320x922] and the 1069:747 design aspect, then converts back
    // to native px. It is width-driven: the width is clamped to its logical limits,
    // the aspect-locked height derived, then the height clamped to its own limits
    // (the limit box's corners are marginally off-aspect, so the height clamp binds at
    // the maximum, giving the 1320x922 corner exactly). The host advertises
    // preserve_aspect_ratio, so it proposes matching w:h pairs; the width drives.
    //
    // Rounding is settled in logical units first and only then scaled to native, which
    // makes the snap a FIXED POINT: re-applying it to its own output returns the same
    // native size. `scale <= 0` is treated as 1.0. Pure + Visage-free (unit-tested in
    // clap_shell_test), so it needs no GUI build to validate.
    //
    // `maxWindowW` / `maxWindowH` are an OPTIONAL dynamic upper bound (0 = unused) in the
    // SAME units as the proposal after the `scale` division — i.e. window units when the
    // caller passes scale 1.0 (the shipping path). They tighten the static 1320x922 cap
    // to whatever the current display can actually hold (see dynamicMaxWindowUnits), so a
    // height-driven grip drag cannot push the window (and its resize grip) off-screen.
    // Because the design aspect is fixed, BOTH axes are honoured: when a dynamic limit
    // binds, the width is re-derived from the aspect-clamped height so it cannot overshoot
    // (the "width overshoot" trap). The static 1320x922 corner is intentionally the box
    // corner (marginally off-aspect) and is preserved exactly when no dynamic limit binds.
    // If the dynamic max falls below the usable minimum (an absurdly small display) the
    // MINIMUM wins — better a slightly-overflowing minimum window than a degenerate one.
    inline void snapEditorSizeForScale (double scale, std::uint32_t& w, std::uint32_t& h,
                                        double maxWindowW = 0.0, double maxWindowH = 0.0) noexcept
    {
        constexpr double kDesignW = 1069.0, kDesignH = 747.0;
        constexpr double kMinW = 471.0, kMinH = 329.0, kMaxW = 1320.0, kMaxH = 922.0;
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
        // preserved, height-driven). The static-only corner (1320x922) skips this so its
        // marginally-off-aspect box corner is preserved for the existing oracle.
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
    // Relaying UNCONDITIONALLY is what closed the Logic AU stack-overflow loop
    // (crash report: ___chkstk_darwin at a guard page). The recurring cycle was
    //   windowContentsResized -> [MTKView setFrameSize] -> _resizeDrawable ->
    //   WindowEventHandler::handleResized -> TopLevelFrame::resized ->
    //   notifyContentsResized -> OUR onWindowContentsResized -> request_resize ->
    //   host [NSView setFrame:] -> setSize -> setWindowDimensions -> Frame::setBounds
    //   -> TopLevelFrame::resized -> windowContentsResized -> ...
    // i.e. our handler asked the host to resize on every contents change, the host
    // obliged by re-driving setSize, and the two size conventions never settled to a
    // fixed point.
    //
    // The cure is to relay ONLY a size the host does not already know about: relay
    // when the window's host-facing size actually CHANGED from what the host last
    // gave us, and NEVER while we are inside a host-driven setSize (`inSetSize` — the
    // host set that size, echoing it back is what re-enters). With both guards,
    // feeding the current size straight back in is a no-op, so the callback chain is
    // a fixed point that terminates in one pass. Pure + Visage-free (unit-tested in
    // clap_shell_test).
    inline bool shouldRelayHostResize (std::uint32_t curW, std::uint32_t curH,
                                       std::uint32_t newW, std::uint32_t newH,
                                       bool inSetSize) noexcept
    {
        if (inSetSize)            return false; // host is the origin of this change
        if (newW == 0 || newH == 0) return false; // degenerate / not yet realised
        return newW != curW || newH != curH;   // relay only a genuine change
    }

    // Construct the RS Visage editor host, backed by the SAME live objects the CLAP
    // shell owns: the RsCore (its published analyser snapshots feed the editor via
    // RsFeedFromCore), the ParamStore (every control binds by id), and the
    // PresetSession (the real program list / apply path). `host` lets the editor
    // relay GUI-driven parameter changes back to the host (rescan + mark-dirty) and
    // request window resizes. The returned object allocates no native window until
    // its create() is called by the shell.
    std::unique_ptr<factory_shell::IClapEditor>
    makeRsClapEditor (rs_core::RsCore& core,
                      factory_params::ParamStore& store,
                      factory_presets::PresetSession& session,
                      const clap_host_t* host);
}
