#pragma once
//
// RsClapEntry.h — the three entry hooks the resonance-suppressor CLAP impl static
// library defines and the tiny per-format ENTRY_SOURCE (RsClapEntryPoint.cpp)
// turns into the exported `clap_entry` symbol. Mirrors the S2 spike's split:
// the impl lib is compiled once; the entry TU is recompiled per format by
// make_clapfirst_plugins.
//
extern bool        rs_clap_entry_init (const char* plugin_path);
extern void        rs_clap_entry_deinit (void);
extern const void* rs_clap_entry_get_factory (const char* factory_id);
