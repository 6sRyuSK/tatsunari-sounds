//
// ClapEntry.cpp — the resonance-suppressor CLAP plugin impl (IMPL_TARGET static
// library for make_clapfirst_plugins). It composes the framework-free pieces the
// factory already ships:
//
//   * rs_core::RsCore                     — the extracted, JUCE-free DSP core (chunk 2)
//   * factory_params::ParamStore          — over resonance_suppressor_params::buildRsParams()
//   * factory_presets::PresetSession       — over the RS bank + kExclude set
//   * factory_shell::ClapShellPlugin<...>  — the generic CLAP glue
//
// and supplies the RS-specific bits the generic shell can't know: the
// RsParamSnapshot fill from the ParamStore, the optional stereo sidechain input
// port (kHasSidechain), and the v2.0.x -> v2.1 "detail" legacy-migration hook
// (the SAME mean formula the JUCE setStateInformation applies, via DetailParam.h).
//
// NO Visage / editor link in chunk 3a — this is headless-validatable (audio-ports,
// params, state, latency, tail). The clap.gui editor is chunk 3b.
//
// clap_plugin_descriptor id: jp.tatsunari-sounds.resonance-suppressor (reverse-DNS).
//
#include <clap/clap.h>

#include "factory_shell/ClapShellPlugin.h"

#include "RsClapEntry.h"

#include "RsCore.h"                  // rs_core::RsCore / RsParamSnapshot (plugin root on the include path)
#include "Source/Params.h"          // resonance_suppressor_params::buildRsParams()
#include "Source/FactoryPresets.h"  // resonance_suppressor_presets::bank / kExclude
#include "Source/DetailParam.h"     // resonance_suppressor_detail::detailFromLegacy()

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef RS_CLAP_VERSION
 #define RS_CLAP_VERSION "0.0.0" // real value injected by CMake from plugin.toml
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
        "jp.tatsunari-sounds.resonance-suppressor",   // id (reverse-DNS)
        "Resonance TatSuppressor",                    // name
        "Tatsunari Sounds",                           // vendor
        "https://github.com/tatsunari-sounds",        // url
        "",                                           // manual_url
        "",                                           // support_url
        RS_CLAP_VERSION,                              // version (from plugin.toml)
        "Soothe-style dynamic resonance suppressor",  // description
        s_features
    };

    // ── ParamStore index cache (RS layout is fixed, so this is computed once) ──
    struct RsIx
    {
        int depth, detail, attack, release, mix, tilt, linkAmt, out;
        int delta, link, bypass, scEnable, scListen;
        int mode, quality, channelMode;
        int lcOn, lcFreq, lcSlope, hcOn, hcFreq, hcSlope;
        int bOn[rs_core::kNumBands], bFreq[rs_core::kNumBands], bType[rs_core::kNumBands],
            bSens[rs_core::kNumBands], bWidth[rs_core::kNumBands];
    };

    RsIx computeRsIx (const factory_params::ParamStore& store)
    {
        RsIx ix {};
        ix.depth   = store.indexOf ("depth");
        ix.detail  = store.indexOf ("detail");
        ix.attack  = store.indexOf ("attack");
        ix.release = store.indexOf ("release");
        ix.mix     = store.indexOf ("mix");
        ix.tilt    = store.indexOf ("tilt");
        ix.linkAmt = store.indexOf ("linkAmt");
        ix.out     = store.indexOf ("out");
        ix.delta    = store.indexOf ("delta");
        ix.link     = store.indexOf ("link");
        ix.bypass   = store.indexOf ("bypass");
        ix.scEnable = store.indexOf ("scEnable");
        ix.scListen = store.indexOf ("scListen");
        ix.mode        = store.indexOf ("mode");
        ix.quality     = store.indexOf ("quality");
        ix.channelMode = store.indexOf ("channelMode");
        ix.lcOn   = store.indexOf ("lc_on");
        ix.lcFreq = store.indexOf ("lc_freq");
        ix.lcSlope = store.indexOf ("lc_slope");
        ix.hcOn   = store.indexOf ("hc_on");
        ix.hcFreq = store.indexOf ("hc_freq");
        ix.hcSlope = store.indexOf ("hc_slope");
        for (int b = 0; b < rs_core::kNumBands; ++b)
        {
            const std::string p = "b" + std::to_string (b) + "_";
            ix.bOn[b]    = store.indexOf (p + "on");
            ix.bFreq[b]  = store.indexOf (p + "freq");
            ix.bType[b]  = store.indexOf (p + "type");
            ix.bSens[b]  = store.indexOf (p + "sens");
            ix.bWidth[b] = store.indexOf (p + "width");
        }
        return ix;
    }

    // First touch happens on the main thread inside activate()'s latency priming,
    // so the magic-static one-time init never runs on the audio thread.
    const RsIx& rsIndices (const factory_params::ParamStore& store)
    {
        static const RsIx ix = computeRsIx (store);
        return ix;
    }

    // Snapshot the live ParamStore into the core's per-block parameter form. The
    // reads mirror the JUCE processor's atomic reads exactly: (double) for the
    // continuous params, (> 0.5f) for the toggles, (int) for the choices — so the
    // core is fed bit-for-bit what the shipping processor feeds its engine.
    void fillSnapshot (const factory_params::ParamStore& store, rs_core::RsParamSnapshot& s) noexcept
    {
        const RsIx& ix = rsIndices (store);

        s.depth   = store.value (ix.depth);
        s.detail  = store.value (ix.detail);
        s.attack  = store.value (ix.attack);
        s.release = store.value (ix.release);
        s.mix     = store.value (ix.mix);
        s.tilt    = store.value (ix.tilt);
        s.linkAmt = store.value (ix.linkAmt);
        s.out     = store.value (ix.out);

        s.delta    = store.value (ix.delta)    > 0.5f;
        s.link     = store.value (ix.link)     > 0.5f;
        s.bypass   = store.value (ix.bypass)   > 0.5f;
        s.scEnable = store.value (ix.scEnable) > 0.5f;
        s.scListen = store.value (ix.scListen) > 0.5f;

        s.mode        = static_cast<int> (store.value (ix.mode));
        s.quality     = static_cast<int> (store.value (ix.quality));
        s.channelMode = static_cast<int> (store.value (ix.channelMode));

        s.lowCut.on    = store.value (ix.lcOn)   > 0.5f;
        s.lowCut.freq  = store.value (ix.lcFreq);
        s.lowCut.slope = static_cast<int> (store.value (ix.lcSlope));
        s.highCut.on   = store.value (ix.hcOn)   > 0.5f;
        s.highCut.freq = store.value (ix.hcFreq);
        s.highCut.slope = static_cast<int> (store.value (ix.hcSlope));

        for (int b = 0; b < rs_core::kNumBands; ++b)
        {
            auto& band = s.bands[static_cast<std::size_t> (b)];
            band.on    = store.value (ix.bOn[b])   > 0.5f;
            band.freq  = store.value (ix.bFreq[b]);
            band.type  = static_cast<int> (store.value (ix.bType[b]));
            band.sens  = store.value (ix.bSens[b]);
            band.width = store.value (ix.bWidth[b]);
        }
    }

    // ── the shell Policy for the resonance suppressor ─────────────────────────
    struct RsClapPolicy
    {
        using Core = rs_core::RsCore;

        static const clap_plugin_descriptor_t* descriptor() { return &s_desc; }

        static std::vector<factory_params::ParamDesc> params()
        {
            return resonance_suppressor_params::buildRsParams();
        }

        static const factory_presets::PresetBank& presetBank()
        {
            return resonance_suppressor_presets::bank;
        }

        static std::vector<std::string> excludeIds()
        {
            return std::vector<std::string> (
                resonance_suppressor_presets::kExclude,
                resonance_suppressor_presets::kExclude + resonance_suppressor_presets::kNumExclude);
        }

        // RS keys detection off an OPTIONAL stereo sidechain input.
        static constexpr bool kHasSidechain = true;

        // CLAP-surface predicate: exclude the legacy sharpness/selectivity pair —
        // kept registered in the shipping build for automation-lane / old-session
        // compatibility, but no longer consumed by the DSP, so off the CLAP surface.
        static bool isClapExposed (const factory_params::ParamDesc& d)
        {
            return (d.flags & factory_params::kFlagLegacyJuceOnly) == 0;
        }

        // v2.0.x -> v2.1: a state lacking "detail" carries the legacy sharpness/
        // selectivity pair; inject detail = their mean (DetailParam.h) — the same
        // migration the JUCE setStateInformation performs.
        static void migrateState (factory_presets::StateModel& m)
        {
            if (m.find ("detail") != nullptr)
                return;
            const double sharp = m.get ("sharpness", 50.0);
            const double sel   = m.get ("selectivity", 50.0);
            m.set ("detail", resonance_suppressor_detail::detailFromLegacy (sharp, sel));
        }

        static void prepare (Core& core, double sampleRate, std::uint32_t maxFrames)
        {
            core.prepare (sampleRate, static_cast<int> (maxFrames));
        }

        static void process (Core& core, const factory_params::ParamStore& store,
                             float* L, float* R, const float* scL, const float* scR,
                             std::uint32_t frames)
        {
            rs_core::RsParamSnapshot snap;
            fillSnapshot (store, snap);
            core.process (L, R, scL, scR, static_cast<int> (frames), snap);
        }

        static std::uint32_t latencySamples (const Core& core)
        {
            return static_cast<std::uint32_t> (core.latencySamples());
        }

        // Silent frames activate() runs so the STFT engine completes its Quality
        // window swap and reports its steady-state latency before we latch it.
        // 2^15 comfortably exceeds the largest window (2^14) + hop, for any rate.
        static std::uint32_t primeFrames() { return 1u << 15; }
    };

    using RsShell = factory_shell::ClapShellPlugin<RsClapPolicy>;

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
        return RsShell::create (host);
    }

    const clap_plugin_factory_t s_factory = {
        factory_get_plugin_count,
        factory_get_plugin_descriptor,
        factory_create_plugin
    };
} // namespace

// ── entry hooks (consumed by RsClapEntryPoint.cpp's clap_entry) ───────────────
bool rs_clap_entry_init (const char*) { return true; }
void rs_clap_entry_deinit (void) {}
const void* rs_clap_entry_get_factory (const char* factory_id)
{
    if (std::strcmp (factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &s_factory;
    return nullptr;
}
