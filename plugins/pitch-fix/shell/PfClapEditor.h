#pragma once
//
// PfClapEditor.h — the framework-free factory the Pf Policy hands the generic
// shell to build the CLAP editor. The DECLARATION is deliberately Visage-free
// (only the factory_shell seam + CLAP host type + forward-declared refs), so
// ClapEntry.cpp — which composes the headless Policy — includes it WITHOUT
// pulling in any GUI framework. The visage-backed definition lives in
// PfClapEditor.cpp, the single translation unit in the pitch-fix CLAP build
// that links Visage; it is compiled ONLY under FACTORY_PF_CLAP_GUI.
//
#include "factory_shell/ClapEditor.h"

#include <clap/clap.h>

#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace pf_core         { class PfCore; }

namespace pf_shell
{
    // Construct the Pf Visage editor host, backed by the SAME live objects the
    // CLAP shell owns: the PfCore (its ui* atomics feed the latency/pitch
    // read-out), the ParamStore (every control binds by id), and the
    // PresetSession (the real program list / apply path). `host` lets the editor
    // relay GUI-driven bulk changes back (rescan + mark-dirty) and ask for a
    // param flush while inactive. The returned object allocates no native window
    // until its create() is called by the shell.
    std::unique_ptr<factory_shell::IClapEditor>
    makePfClapEditor (pf_core::PfCore& core,
                      factory_params::ParamStore& store,
                      factory_presets::PresetSession& session,
                      const clap_host_t* host);
}
