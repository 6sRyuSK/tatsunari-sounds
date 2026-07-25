#pragma once
//
// factory_shell/ClapEntryPoint.h — the shared, framework-free CLAP factory + entry
// glue every clap-first plugin needs, so a plugin's ClapEntry.cpp keeps ONLY its
// Policy (descriptor + params/preset tables + the DSP hooks) and drops the identical
// factory / entry-hook boilerplate that used to be copy-pasted per plugin (compare
// the old resonance-suppressor / pitch-fix ClapEntry.cpp tails).
//
//   * SinglePluginClapFactory<Policy> — the clap_plugin_factory_t for a plugin that
//     exposes exactly one descriptor (every factory plugin): count == 1, descriptor
//     from Policy::descriptor(), create == ClapShellPlugin<Policy>::create after the
//     clap-version + id checks.
//   * FACTORY_CLAP_ENTRY(POLICY) — emits the three COMMON-NAMED extern "C" entry
//     hooks (factory_clap_entry_init / _deinit / _get_factory) into the plugin's
//     IMPL translation unit, where the (possibly anonymous-namespace) POLICY is
//     visible.
//
// THE make_clapfirst IMPL / ENTRY SPLIT (do not break it): clap-wrapper compiles the
// IMPL_TARGET static library ONCE and recompiles the tiny ENTRY_SOURCE ONCE PER
// FORMAT (CLAP exports clap_entry directly; the VST3/AU wrappers consume the same
// symbol through the static link). Therefore:
//   * the exported `clap_entry` symbol lives ONLY in the shared ENTRY_SOURCE
//     (shell/src/FactoryClapEntryPoint.cpp), forwarding to the three hooks below;
//   * this macro emits ONLY the hooks (never clap_entry) into the compiled-once IMPL.
// A macro that emitted clap_entry itself would either place it in the compiled-once
// IMPL (wrong — it must be per-format) or drag the Policy/shell/core into the
// per-format TU (defeats the split).
//
// ODR / linkage: the hooks + clap_entry use COMMON names (no per-plugin prefix). That
// is safe because make_clapfirst links exactly ONE IMPL_TARGET into each <slug>_clap /
// _vst3 / _au module, so each symbol has exactly one definition per binary. This holds
// BECAUSE the factory is single-plugin-per-module; a future multi-plugin-in-one-bundle
// module would reintroduce a collision and must revisit this.
//
#include "factory_shell/ClapShellPlugin.h"

#include <clap/clap.h>

#include <cstdint>
#include <cstring>

namespace factory_shell
{
    // The clap_plugin_factory_t for a plugin exposing a single descriptor. Policy
    // supplies descriptor(); ClapShellPlugin<Policy> supplies create().
    template <class Policy>
    struct SinglePluginClapFactory
    {
        using Shell = ClapShellPlugin<Policy>;

        static std::uint32_t count (const clap_plugin_factory_t*) { return 1; }

        static const clap_plugin_descriptor_t* descriptor (const clap_plugin_factory_t*,
                                                           std::uint32_t index)
        {
            return index == 0 ? Policy::descriptor() : nullptr;
        }

        static const clap_plugin_t* create (const clap_plugin_factory_t*,
                                            const clap_host_t* host, const char* plugin_id)
        {
            if (! clap_version_is_compatible (host->clap_version)) return nullptr;
            if (std::strcmp (plugin_id, Policy::descriptor()->id) != 0) return nullptr;
            return Shell::create (host);
        }

        // Function-local static: one instance per (Policy) per binary, constructed on
        // first use (thread-safe magic-static), never destroyed before the host is done.
        static const clap_plugin_factory_t* instance()
        {
            static const clap_plugin_factory_t f { &count, &descriptor, &create };
            return &f;
        }
    };
} // namespace factory_shell

// Emit the three common-named entry hooks into the IMPL TU. POLICY must be visible at
// the point of expansion (anonymous-namespace policies are, within their TU). The
// shared ENTRY_SOURCE turns these into the exported clap_entry per format.
#define FACTORY_CLAP_ENTRY(POLICY)                                                       \
    extern "C" bool factory_clap_entry_init (const char*) { return true; }               \
    extern "C" void factory_clap_entry_deinit (void) {}                                  \
    extern "C" const void* factory_clap_entry_get_factory (const char* factory_id)       \
    {                                                                                    \
        if (std::strcmp (factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)                       \
            return factory_shell::SinglePluginClapFactory<POLICY>::instance();           \
        return nullptr;                                                                  \
    }
