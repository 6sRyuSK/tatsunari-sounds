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

#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace rs_core         { class RsCore; }

namespace rs_shell
{
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
