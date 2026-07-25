#pragma once
//
// DeqClapEditor.h — the framework-free factory the Deq Policy hands the generic shell to
// build the CLAP editor. The DECLARATION is Visage-free (only the factory_shell seam +
// CLAP host type + forward-declared refs), so ClapEntry.cpp — which composes the headless
// Policy — includes it WITHOUT pulling in any GUI framework. The visage-backed definition
// lives in DeqClapEditor.cpp, the single translation unit in the dynamic-eq CLAP build
// that links Visage; compiled ONLY under FACTORY_DEQ_CLAP_GUI.
//
#include "factory_shell/ClapEditor.h"

#include <clap/clap.h>

#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace deq_core        { class DeqCore; }

namespace deq_shell
{
    // Construct the Deq Visage editor host (a ResizableVisageClapEditor derivation),
    // backed by the SAME live objects the CLAP shell owns: the DeqCore (its analyser
    // rings + live gains feed the curve), the ParamStore (every control binds by id), and
    // the PresetSession (the real program list / apply path). The returned object
    // allocates no native window until its create() is called by the shell.
    std::unique_ptr<factory_shell::IClapEditor>
    makeDeqClapEditor (deq_core::DeqCore& core,
                       factory_params::ParamStore& store,
                       factory_presets::PresetSession& session,
                       const clap_host_t* host);
}
