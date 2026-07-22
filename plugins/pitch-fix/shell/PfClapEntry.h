#pragma once
//
// PfClapEntry.h — the three entry hooks the pitch-fix CLAP impl static library
// defines and the tiny per-format ENTRY_SOURCE (PfClapEntryPoint.cpp) turns
// into the exported `clap_entry` symbol. Same split as the S2 spike: the impl
// lib is compiled once; the entry TU is recompiled per format by
// make_clapfirst_plugins.
//
extern bool        pf_clap_entry_init (const char* plugin_path);
extern void        pf_clap_entry_deinit (void);
extern const void* pf_clap_entry_get_factory (const char* factory_id);
