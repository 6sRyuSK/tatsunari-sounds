//
// ClapEntry.cpp — the dynamic-eq CLAP plugin impl (IMPL_TARGET static library for
// make_clapfirst_plugins). Composes the framework-free factory pieces:
//
//   * deq_core::DeqCore                    — the JUCE-free DSP core (DeqCore.h)
//   * factory_params::ParamStore           — over dynamic_eq_params::buildDeqParams()
//   * factory_presets::PresetSession        — over the dynamic-eq bank + kExclude
//   * factory_shell::ClapShellPlugin<...>   — the generic CLAP glue
//
// and supplies the Deq-specific bits the generic shell can't know: the DeqParamSnapshot
// fill from the ParamStore and the 2.0.0 clean-break state migration (v0/JUCE blob ->
// defaults). The JUCE DynamicEqAudioProcessor is retained ONLY as the byte-equivalence
// oracle (deqcore_equiv_test), never shipped.
//
// clap_plugin_descriptor id: jp.tatsunari-sounds.dynamic-eq (reverse-DNS).
//
#include <clap/clap.h>

#include "factory_shell/ClapEntryPoint.h" // ClapShellPlugin + the shared factory/entry glue

#include "DeqClapEditor.h"

#if FACTORY_DEQ_CLAP_GUI
 #include <memory>
#endif

#include "DeqCore.h"              // deq_core::DeqCore / DeqParamSnapshot (plugin root on the include path)
#include "DeqParams.h"            // dynamic_eq_params::buildDeqParams()
#include "DeqStateMigration.h"    // dynamic_eq_state::cleanBreakMigrate()
#include "Source/FactoryPresets.h" // dynamic_eq_presets::bank / kExclude

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef DEQ_CLAP_VERSION
 #define DEQ_CLAP_VERSION "0.0.0" // real value injected by CMake from plugin.toml
#endif

namespace
{
    // ── descriptor ───────────────────────────────────────────────────────────
    const char* const s_features[] = {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
        CLAP_PLUGIN_FEATURE_STEREO,
        CLAP_PLUGIN_FEATURE_EQUALIZER,
        nullptr
    };

    const clap_plugin_descriptor_t s_desc = {
        CLAP_VERSION_INIT,
        "jp.tatsunari-sounds.dynamic-eq",             // id (reverse-DNS)
        "Dynamic TatEQ",                              // name
        "Tatsunari Sounds",                           // vendor
        "https://github.com/tatsunari-sounds",        // url
        "",                                           // manual_url
        "",                                           // support_url
        DEQ_CLAP_VERSION,                             // version (from plugin.toml)
        "Pro-Q-style multi-band dynamic EQ",          // description
        s_features
    };

    // ── ParamStore index cache (fixed layout, computed once) ──────────────────
    struct DeqIx
    {
        int on[deq_core::kNumBands], byp[deq_core::kNumBands], lsn[deq_core::kNumBands],
            dyn[deq_core::kNumBands], chan[deq_core::kNumBands], type[deq_core::kNumBands],
            slope[deq_core::kNumBands], freq[deq_core::kNumBands], gain[deq_core::kNumBands],
            q[deq_core::kNumBands], thr[deq_core::kNumBands], rng[deq_core::kNumBands],
            atk[deq_core::kNumBands], rel[deq_core::kNumBands], knee[deq_core::kNumBands];
        int bypass;
    };

    DeqIx computeDeqIx (const factory_params::ParamStore& store)
    {
        DeqIx ix {};
        for (int b = 0; b < deq_core::kNumBands; ++b)
        {
            const std::string p = "b" + std::to_string (b) + "_";
            ix.on[b]    = store.indexOf (p + "on");
            ix.byp[b]   = store.indexOf (p + "byp");
            ix.lsn[b]   = store.indexOf (p + "lsn");
            ix.dyn[b]   = store.indexOf (p + "dyn");
            ix.chan[b]  = store.indexOf (p + "chan");
            ix.type[b]  = store.indexOf (p + "type");
            ix.slope[b] = store.indexOf (p + "slope");
            ix.freq[b]  = store.indexOf (p + "freq");
            ix.gain[b]  = store.indexOf (p + "gain");
            ix.q[b]     = store.indexOf (p + "q");
            ix.thr[b]   = store.indexOf (p + "thr");
            ix.rng[b]   = store.indexOf (p + "rng");
            ix.atk[b]   = store.indexOf (p + "atk");
            ix.rel[b]   = store.indexOf (p + "rel");
            ix.knee[b]  = store.indexOf (p + "knee");
        }
        ix.bypass = store.indexOf ("bypass");
        return ix;
    }

    // First touch happens on the main thread inside activate()'s latency priming (or the
    // first process for a zero-prime core), so the magic-static init never races.
    const DeqIx& deqIndices (const factory_params::ParamStore& store)
    {
        static const DeqIx ix = computeDeqIx (store);
        return ix;
    }

    // Snapshot the live ParamStore into the core's per-block parameter form, mirroring the
    // processor's atomic reads exactly (`> 0.5f` toggles, `(int)` choices, `(double)`
    // continuous) so the core is fed bit-for-bit what the shipping processor feeds its bands.
    void fillSnapshot (const factory_params::ParamStore& store, deq_core::DeqParamSnapshot& s) noexcept
    {
        const DeqIx& ix = deqIndices (store);
        for (int b = 0; b < deq_core::kNumBands; ++b)
        {
            auto& bs = s.bands[(std::size_t) b];
            bs.on    = store.value (ix.on[b])  > 0.5f;
            bs.byp   = store.value (ix.byp[b]) > 0.5f;
            bs.lsn   = store.value (ix.lsn[b]) > 0.5f;
            bs.dyn   = store.value (ix.dyn[b]) > 0.5f;
            bs.chan  = static_cast<int> (store.value (ix.chan[b]));
            bs.type  = static_cast<int> (store.value (ix.type[b]));
            bs.slope = static_cast<int> (store.value (ix.slope[b]));
            bs.freq  = store.value (ix.freq[b]);
            bs.gain  = store.value (ix.gain[b]);
            bs.q     = store.value (ix.q[b]);
            bs.thr   = store.value (ix.thr[b]);
            bs.rng   = store.value (ix.rng[b]);
            bs.atk   = store.value (ix.atk[b]);
            bs.rel   = store.value (ix.rel[b]);
            bs.knee  = store.value (ix.knee[b]);
        }
        s.bypass = store.value (ix.bypass) > 0.5f;
    }

    // ── the shell Policy for dynamic-eq ───────────────────────────────────────
    struct DeqClapPolicy
    {
        using Core = deq_core::DeqCore;

        static const clap_plugin_descriptor_t* descriptor() { return &s_desc; }

        static std::vector<factory_params::ParamDesc> params()
        {
            return dynamic_eq_params::buildDeqParams();
        }

        static const factory_presets::PresetBank& presetBank()
        {
            return dynamic_eq_presets::bank;
        }

        static std::vector<std::string> excludeIds()
        {
            return std::vector<std::string> (
                dynamic_eq_presets::kExclude,
                dynamic_eq_presets::kExclude + dynamic_eq_presets::kNumExclude);
        }

        // The EQ reads only the main input (no sidechain).
        static constexpr bool kHasSidechain = false;

        // No legacy JUCE-only parameters — the whole table is the CLAP surface.
        static bool isClapExposed (const factory_params::ParamDesc&) { return true; }

        // 2.0.0 CLEAN BREAK: a v0 (JUCE-era / foreign) blob is reset to defaults; a genuine
        // StateCodec state (stateVersion >= 1) passes through untouched.
        static void migrateState (factory_presets::StateModel& m)
        {
            dynamic_eq_state::cleanBreakMigrate (m);
        }

        static void prepare (Core& core, double sampleRate, std::uint32_t maxFrames)
        {
            core.prepare (sampleRate, static_cast<int> (maxFrames));
        }

        static void process (Core& core, const factory_params::ParamStore& store,
                             float* L, float* R, const float*, const float*,
                             std::uint32_t frames)
        {
            deq_core::DeqParamSnapshot snap;
            fillSnapshot (store, snap);
            core.process (L, R, static_cast<int> (frames), snap);
        }

        // Zero-latency core (biquad cascade); nothing to settle before latching.
        static std::uint32_t latencySamples (const Core&) { return 0u; }
        static std::uint32_t primeFrames() { return 0u; }

#if FACTORY_DEQ_CLAP_GUI
        // The presence of kHasEditor makes the generic shell expose CLAP_EXT_GUI
        // (+ Linux posix-fd); makeEditor builds the Visage editor over the shell's live
        // core / store / session.
        static constexpr bool kHasEditor = true;

        static std::unique_ptr<factory_shell::IClapEditor>
        makeEditor (Core& core, factory_params::ParamStore& store,
                    factory_presets::PresetSession& session, const clap_host_t* host)
        {
            return deq_shell::makeDeqClapEditor (core, store, session, host);
        }
#endif
    };
} // namespace

// The single-plugin CLAP factory + the three common-named entry hooks (clap_entry lives in
// the shared ENTRY_SOURCE and forwards here).
FACTORY_CLAP_ENTRY (DeqClapPolicy)
