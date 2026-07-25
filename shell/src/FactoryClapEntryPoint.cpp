//
// FactoryClapEntryPoint.cpp — the SHARED ENTRY_SOURCE handed to make_clapfirst_plugins
// by every clap-first plugin (shell/cmake/FactoryClapPlugin.cmake defaults ENTRY_SOURCE
// to this file). It exports the single `clap_entry` symbol every CLAP needs, forwarding
// to the three common-named hooks the plugin's IMPL library defines via
// FACTORY_CLAP_ENTRY(POLICY) (factory_shell/ClapEntryPoint.h).
//
// Deliberately tiny + Policy-free: the wrapper recompiles THIS TU once per format (CLAP
// exports clap_entry directly; the VST3/AU wrappers consume the same symbol through the
// static link), while the Policy-bearing IMPL library is compiled once. See the
// ClapEntryPoint.h header for the full rationale on the split + the common-name ODR
// argument (one IMPL_TARGET per module => exactly one definition per binary).
//
#include <clap/clap.h>

extern "C"
{
    extern bool        factory_clap_entry_init (const char* plugin_path);
    extern void        factory_clap_entry_deinit (void);
    extern const void* factory_clap_entry_get_factory (const char* factory_id);

#ifdef __GNUC__
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wattributes"
#endif

    const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
        CLAP_VERSION,
        factory_clap_entry_init,
        factory_clap_entry_deinit,
        factory_clap_entry_get_factory
    };

#ifdef __GNUC__
 #pragma GCC diagnostic pop
#endif
}
