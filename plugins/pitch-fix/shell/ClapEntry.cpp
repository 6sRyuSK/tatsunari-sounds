//
// ClapEntry.cpp — the pitch-fix CLAP plugin impl (IMPL_TARGET static library
// for make_clapfirst_plugins). Composes the framework-free factory pieces:
//
//   * pf_core::PfCore                      — the JUCE-free DSP core (PfCore.h)
//   * factory_params::ParamStore           — over pitch_fix_params::buildPfParams()
//   * factory_presets::PresetSession       — over the pitch-fix bank + kExclude
//   * factory_shell::ClapShellPlugin<...>  — the generic CLAP glue
//
// pitch-fix is clap-first FROM BIRTH: there is no JUCE processor at all (unlike
// RS, which keeps one as its migration byte-equivalence oracle), so migrateState
// is a no-op — the only wire format that exists is StateCodec v1+.
//
// clap_plugin_descriptor id: jp.tatsunari-sounds.pitch-fix (reverse-DNS).
//
#include <clap/clap.h>

#include "factory_shell/ClapShellPlugin.h"

#include "PfClapEntry.h"

#if FACTORY_PF_CLAP_GUI
 // Visage-free declaration (the definition — the only visage TU — is
 // PfClapEditor.cpp, compiled only under FACTORY_PF_CLAP_GUI). Keeping this
 // header framework-free keeps THIS file headless-buildable.
 #include "PfClapEditor.h"
 #include <memory>
#endif

#include "PfCore.h"      // pf_core::PfCore / PfParamSnapshot (plugin root on the include path)
#include "PfParams.h"    // pitch_fix_params::buildPfParams()
#include "PfPresets.h"   // pitch_fix_presets::bank / kExclude

#include <cstdint>
#include <cstring>
#include <vector>

#ifndef PF_CLAP_VERSION
 #define PF_CLAP_VERSION "0.0.0" // real value injected by CMake from plugin.toml
#endif

namespace
{
    // ── descriptor ───────────────────────────────────────────────────────────
    const char* const s_features[] = {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
        CLAP_PLUGIN_FEATURE_PITCH_CORRECTION,
        CLAP_PLUGIN_FEATURE_STEREO,
        nullptr
    };

    const clap_plugin_descriptor_t s_desc = {
        CLAP_VERSION_INIT,
        "jp.tatsunari-sounds.pitch-fix",                    // id (reverse-DNS)
        "Pitch TatFixer",                                   // name
        "Tatsunari Sounds",                                 // vendor
        "https://github.com/tatsunari-sounds",              // url
        "",                                                 // manual_url
        "",                                                 // support_url
        PF_CLAP_VERSION,                                    // version (from plugin.toml)
        "Real-time monophonic pitch correction",            // description
        s_features
    };

    // ── ParamStore index cache (fixed layout, computed once) ──────────────────
    struct PfIx
    {
        int amount, retune, glide, tolerance, hysteresis;
        int minPitch, maxPitch, threshold;
        int buffer, key, scale, a4, mix, out;
    };

    PfIx computePfIx (const factory_params::ParamStore& store)
    {
        PfIx ix {};
        ix.amount     = store.indexOf ("amount");
        ix.retune     = store.indexOf ("retune");
        ix.glide      = store.indexOf ("glide");
        ix.tolerance  = store.indexOf ("tolerance");
        ix.hysteresis = store.indexOf ("hysteresis");
        ix.minPitch   = store.indexOf ("min_pitch");
        ix.maxPitch   = store.indexOf ("max_pitch");
        ix.threshold  = store.indexOf ("threshold");
        ix.buffer     = store.indexOf ("buffer");
        ix.key        = store.indexOf ("key");
        ix.scale      = store.indexOf ("scale");
        ix.a4         = store.indexOf ("a4");
        ix.mix        = store.indexOf ("mix");
        ix.out        = store.indexOf ("out");
        return ix;
    }

    // First touch happens on the main thread inside activate()'s latency
    // priming, so the magic-static init never runs on the audio thread.
    const PfIx& pfIndices (const factory_params::ParamStore& store)
    {
        static const PfIx ix = computePfIx (store);
        return ix;
    }

    // Snapshot the live ParamStore into the core's per-block parameter form:
    // (float) for continuous params, (int) for the choices.
    void fillSnapshot (const factory_params::ParamStore& store,
                       pf_core::PfParamSnapshot& s) noexcept
    {
        const PfIx& ix = pfIndices (store);

        s.amount       = store.value (ix.amount);
        s.retuneMs     = store.value (ix.retune);
        s.glideMs      = store.value (ix.glide);
        s.toleranceCt  = store.value (ix.tolerance);
        s.hysteresisCt = store.value (ix.hysteresis);
        s.minPitchHz   = store.value (ix.minPitch);
        s.maxPitchHz   = store.value (ix.maxPitch);
        s.thresholdPct = store.value (ix.threshold);
        s.buffer       = static_cast<int> (store.value (ix.buffer));
        s.key          = static_cast<int> (store.value (ix.key));
        s.scale        = static_cast<int> (store.value (ix.scale));
        s.a4Hz         = store.value (ix.a4);
        s.mixPct       = store.value (ix.mix);
        s.outDb        = store.value (ix.out);
    }

    // ── the shell Policy for pitch-fix ────────────────────────────────────────
    struct PfClapPolicy
    {
        using Core = pf_core::PfCore;

        static const clap_plugin_descriptor_t* descriptor() { return &s_desc; }

        static std::vector<factory_params::ParamDesc> params()
        {
            return pitch_fix_params::buildPfParams();
        }

        static const factory_presets::PresetBank& presetBank()
        {
            return pitch_fix_presets::bank;
        }

        static std::vector<std::string> excludeIds()
        {
            return std::vector<std::string> (
                pitch_fix_presets::kExclude,
                pitch_fix_presets::kExclude + pitch_fix_presets::kNumExclude);
        }

        // Pitch correction reads only the main input.
        static constexpr bool kHasSidechain = false;

        // No legacy JUCE build ever existed — the whole table is the surface.
        static bool isClapExposed (const factory_params::ParamDesc&) { return true; }

        // Clap-first from birth: StateCodec v1+ is the only wire format, so
        // there is nothing to migrate (foreign blobs are rejected upstream).
        static void migrateState (factory_presets::StateModel&) {}

        static void prepare (Core& core, double sampleRate, std::uint32_t maxFrames)
        {
            core.prepare (sampleRate, static_cast<int> (maxFrames));
        }

        static void process (Core& core, const factory_params::ParamStore& store,
                             float* L, float* R, const float*, const float*,
                             std::uint32_t frames)
        {
            pf_core::PfParamSnapshot snap;
            fillSnapshot (store, snap);
            core.process (L, R, static_cast<int> (frames), snap);
        }

        static std::uint32_t latencySamples (const Core& core)
        {
            return static_cast<std::uint32_t> (core.latencySamples());
        }

        // Silent frames activate() runs so the first process() call adopts the
        // live snapshot (Buffer / Min Pitch) and latencySamples() reports the
        // settled lookahead before the shell latches it.
        static std::uint32_t primeFrames() { return 1u << 13; }

#if FACTORY_PF_CLAP_GUI
        // The presence of kHasEditor makes the generic shell expose
        // CLAP_EXT_GUI (+ Linux posix-fd); makeEditor builds the Visage editor
        // host over the shell's live core / store / session.
        static constexpr bool kHasEditor = true;

        static std::unique_ptr<factory_shell::IClapEditor>
        makeEditor (Core& core, factory_params::ParamStore& store,
                    factory_presets::PresetSession& session, const clap_host_t* host)
        {
            return pf_shell::makePfClapEditor (core, store, session, host);
        }
#endif
    };

    using PfShell = factory_shell::ClapShellPlugin<PfClapPolicy>;

    // ── factory ───────────────────────────────────────────────────────────────
    uint32_t factory_get_plugin_count (const clap_plugin_factory_t*) { return 1; }

    const clap_plugin_descriptor_t* factory_get_plugin_descriptor (const clap_plugin_factory_t*,
                                                                   uint32_t index)
    {
        return index == 0 ? &s_desc : nullptr;
    }

    const clap_plugin_t* factory_create_plugin (const clap_plugin_factory_t*,
                                                const clap_host_t* host, const char* plugin_id)
    {
        if (! clap_version_is_compatible (host->clap_version)) return nullptr;
        if (std::strcmp (plugin_id, s_desc.id) != 0)           return nullptr;
        return PfShell::create (host);
    }

    const clap_plugin_factory_t s_factory = {
        factory_get_plugin_count,
        factory_get_plugin_descriptor,
        factory_create_plugin
    };
} // namespace

// ── entry hooks (consumed by PfClapEntryPoint.cpp's clap_entry) ───────────────
bool pf_clap_entry_init (const char*) { return true; }
void pf_clap_entry_deinit (void) {}
const void* pf_clap_entry_get_factory (const char* factory_id)
{
    if (std::strcmp (factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &s_factory;
    return nullptr;
}
