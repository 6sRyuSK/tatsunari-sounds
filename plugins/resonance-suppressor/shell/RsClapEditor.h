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
#include "factory_shell/ResizableEditorGeometry.h" // the shared aspect/limit snap + relay

#include <clap/clap.h>

#include <cstdint>
#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace rs_core         { class RsCore; }

namespace rs_shell
{
    // The RS editor's design aspect + resize limits: reference layout 1069x747, resize
    // limits [471x329 .. 1320x922]. The pure snap + relay helpers now live in
    // factory_shell (ResizableEditorGeometry.h), parameterised by this geometry and
    // shared with dynamic-eq; the thin rs_shell:: wrappers below keep the RS call sites
    // (RsClapEditor.cpp) and the clap_shell_test fixed-point oracle unchanged.
    inline constexpr factory_shell::EditorGeometry kRsGeometry {
        /*designW*/ 1069.0, /*designH*/ 747.0, /*minW*/ 471.0, /*minH*/ 329.0,
        /*maxW*/ 1320.0, /*maxH*/ 922.0
    };

    inline void snapEditorSizeForScale (double scale, std::uint32_t& w, std::uint32_t& h,
                                        double maxWindowW = 0.0, double maxWindowH = 0.0) noexcept
    {
        factory_shell::snapEditorSizeForScale (kRsGeometry, scale, w, h, maxWindowW, maxWindowH);
    }

    inline bool shouldRelayHostResize (std::uint32_t curW, std::uint32_t curH,
                                       std::uint32_t newW, std::uint32_t newH,
                                       bool inSetSize) noexcept
    {
        return factory_shell::shouldRelayHostResize (curW, curH, newW, newH, inSetSize);
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
