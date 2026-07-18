#pragma once
//
// factory_shell/ClapStateBridge.h — the CLAP `clap.state` save/load path,
// expressed over factory_presets::StateCodec and a factory_params::ParamStore.
// DSP-agnostic and plugin-agnostic (no Core/Policy/templates) — a plain compiled
// unit in shell/src.
//
// SAVE: encode every ParamStore parameter's REAL value plus the current preset
// index into the StateCodec wire format (magic-framed <PARAMS> XML,
// byte-compatible framing with the shipping build's copyXmlToBinary blob). The
// codec's `stateVersion` is stamped to kStateVersionCurrent.
//
// LOAD: decode (tolerant), run the plugin's legacy-migration hook against the
// parsed model (e.g. RS injects "detail" from a pre-detail sharpness/selectivity
// pair — the SAME mean formula the shipping setStateInformation uses), then write
// each parameter back into the store: the model's value where present, the
// descriptor's default otherwise (so a partial/foreign blob leaves no residue —
// matching the shipping applyStateXml "missing key -> default" contract). The
// decoded preset index is returned for the caller to adopt via PresetSession.
//
#include "factory_params/ParamStore.h"
#include "factory_presets/StateCodec.h"

#include <clap/clap.h>

#include <functional>

namespace factory_shell
{
    // Encode store real values + presetIndex, write to the CLAP output stream.
    // Returns false only on a stream write error.
    bool saveState (const factory_params::ParamStore& store,
                    int presetIndex,
                    const clap_ostream_t* stream);

    // Read the whole CLAP input stream, decode+migrate, apply to the store.
    // `migrate` may be empty. On success returns true and sets outPresetIndex;
    // on a bad/truncated/foreign blob returns false and leaves the store untouched.
    bool loadState (factory_params::ParamStore& store,
                    const clap_istream_t* stream,
                    const std::function<void (factory_presets::StateModel&)>& migrate,
                    int& outPresetIndex);
} // namespace factory_shell
