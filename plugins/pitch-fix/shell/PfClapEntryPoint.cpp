//
// PfClapEntryPoint.cpp — the ENTRY_SOURCE for make_clapfirst_plugins. It exports
// the single `clap_entry` symbol every CLAP needs, forwarding to the three hooks
// defined in the impl static library (ClapEntry.cpp). Deliberately tiny: the
// wrapper recompiles this TU once per format (CLAP exports it directly; the VST3
// wrapper consumes the same symbol through the static link).
//
#include <clap/clap.h>

#include "PfClapEntry.h"

extern "C"
{
#ifdef __GNUC__
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wattributes"
#endif

    const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
        CLAP_VERSION,
        pf_clap_entry_init,
        pf_clap_entry_deinit,
        pf_clap_entry_get_factory
    };

#ifdef __GNUC__
 #pragma GCC diagnostic pop
#endif
}
