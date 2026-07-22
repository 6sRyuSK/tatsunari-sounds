#pragma once
//
// factory_shell/ClapShellPlugin.h — the generic CLAP plugin implementation, over
// the CLAP C API, parameterised by a plugin "Policy". It owns the reusable glue
// (audio ports, params, state, latency, tail, the FTZ/DAZ boundary, block-granular
// parameter delivery) and delegates every DSP-specific decision to the Policy, so
// the shell itself stays framework-free and DSP-agnostic.
//
// ─────────────────────────────────────────────────────────────────────────────
// POLICY CONCEPT (a plugin supplies one; see plugins/*/shell/ClapEntry.cpp).
// The shell itself has no dependency on the shipping VST3/AU build; the Policy
// bridges to the plugin's DSP core + shared param/preset tables:
//
//   using Core = <plain C++ DSP core>;             // e.g. rs_core::RsCore
//
//   static const clap_plugin_descriptor_t* descriptor();
//   static std::vector<factory_params::ParamDesc> params();      // the ParamDesc table
//   static const factory_presets::PresetBank&     presetBank();
//   static std::vector<std::string>               excludeIds();  // preset-exclusion set
//
//   static constexpr bool kHasSidechain;           // declare the optional stereo SC input port
//
//   static bool isClapExposed (const factory_params::ParamDesc&); // CLAP-surface predicate
//   static void migrateState (factory_presets::StateModel&);     // legacy-migration hook
//
//   static void     prepare (Core&, double sampleRate, std::uint32_t maxFrames);
//   static void     process (Core&, const factory_params::ParamStore&,
//                            float* L, float* R, const float* scL, const float* scR,
//                            std::uint32_t frames);               // snapshot store -> Core.process
//   static std::uint32_t latencySamples (const Core&);
//   static std::uint32_t primeFrames();            // silent frames activate() runs to settle
//                                                  // steady-state latency (0 = constant-latency core)
//
// ─────────────────────────────────────────────────────────────────────────────
// GUI (chunk 3b): OPT-IN and Policy-driven. The shell advertises CLAP_EXT_GUI
// (and, on Linux, CLAP_EXT_POSIX_FD_SUPPORT) ONLY when the Policy provides an
// editor — i.e. defines `static constexpr bool kHasEditor = true` plus a
// `makeEditor(core, store, session, host)` factory returning a
// factory_shell::IClapEditor. Detection is compile-time (PolicyHasEditor), so a
// Policy WITHOUT an editor (every headless chunk-3a plugin) is byte-for-byte the
// old GUI-less shell: get_extension() returns nullptr for CLAP_EXT_GUI and every
// gate stays headless-validatable. The concrete editor (Visage-backed for RS)
// lives in the plugin and is the ONLY place that links Visage; this header — and
// the shell library — never see it. All gui.* calls are [main-thread]; the audio
// thread never touches the editor.
//
// REAL-TIME SAFETY: process() and everything it calls (event apply, the Policy
// snapshot+process, the latency check) performs no allocation, lock, or syscall.
// All buffers are sized in activate() (main thread). Values shared between the
// audio thread and the main thread (reportedLatency, the restart-pending latch,
// active) are std::atomic and accessed lock-free.
//
#include "factory_shell/ClapParamBridge.h"
#include "factory_shell/ClapStateBridge.h"
#include "factory_shell/ClapEditor.h"
#include "factory_shell/DenormalGuard.h"

#include "factory_params/ParamDesc.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetBank.h"
#include "factory_presets/PresetSession.h"
#include "factory_presets/StateCodec.h"

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace factory_shell
{
    template <class Policy>
    class ClapShellPlugin
    {
    public:
        // Factory entry: allocate an instance and hand back its clap_plugin_t.
        static clap_plugin_t* create (const clap_host_t* host)
        {
            auto* self = new ClapShellPlugin (host);
            return &self->plugin;
        }

    private:
        explicit ClapShellPlugin (const clap_host_t* h)
            : host (h),
              paramTable (Policy::params()),
              store (paramTable),
              bridge (paramTable, &Policy::isClapExposed),
              session (store, Policy::presetBank(), Policy::excludeIds())
        {
            plugin.desc            = Policy::descriptor();
            plugin.plugin_data     = this;
            plugin.init            = &clapInit;
            plugin.destroy         = &clapDestroy;
            plugin.activate        = &clapActivate;
            plugin.deactivate      = &clapDeactivate;
            plugin.start_processing = &clapStartProcessing;
            plugin.stop_processing  = &clapStopProcessing;
            plugin.reset           = &clapReset;
            plugin.process         = &clapProcess;
            plugin.get_extension   = &clapGetExtension;
            plugin.on_main_thread  = &clapOnMainThread;
        }

        static ClapShellPlugin* self (const clap_plugin_t* p) noexcept
        {
            return static_cast<ClapShellPlugin*> (p->plugin_data);
        }

        static void copyName (char* dst, const char* src) noexcept
        {
            std::snprintf (dst, CLAP_NAME_SIZE, "%s", src);
        }

        // ───────────────────────────── lifecycle ────────────────────────────
        static bool clapInit (const clap_plugin_t* p)
        {
            auto* s = self (p);
            s->hostParams  = static_cast<const clap_host_params_t*>  (s->host->get_extension (s->host, CLAP_EXT_PARAMS));
            s->hostLatency = static_cast<const clap_host_latency_t*> (s->host->get_extension (s->host, CLAP_EXT_LATENCY));
            s->hostState   = static_cast<const clap_host_state_t*>   (s->host->get_extension (s->host, CLAP_EXT_STATE));
#ifdef __linux__
            // Only relevant when we run a GUI; harmless to fetch otherwise. The
            // editor's X11 window fd is pumped by the host through this extension.
            s->hostPosixFd = static_cast<const clap_host_posix_fd_support_t*> (
                s->host->get_extension (s->host, CLAP_EXT_POSIX_FD_SUPPORT));
#endif
            return true;
        }

        static void clapDestroy (const clap_plugin_t* p) { delete self (p); }

        static bool clapActivate (const clap_plugin_t* p, double sampleRate,
                                  std::uint32_t /*minFrames*/, std::uint32_t maxFrames)
        {
            auto* s = self (p);
            s->sampleRate = sampleRate;
            s->maxFrames  = maxFrames;
            Policy::prepare (s->core, sampleRate, maxFrames);

            // Settle the core to its steady-state latency for the CURRENT parameter
            // state (an STFT core reports its default-quality latency straight after
            // prepare(), then swaps window length at a frame boundary once the live
            // Quality is applied). Latch the settled value so get() is stable and
            // process() won't spuriously request a restart on the first block.
            s->primeLatency();
            const std::uint32_t lat = Policy::latencySamples (s->core);
            s->reportedLatency.store (lat, std::memory_order_relaxed);
            s->latencyRestartPending.store (false, std::memory_order_relaxed);

            // clap_host_latency.changed() is [main-thread & being-activated] — here.
            if (s->hostLatency != nullptr && lat != s->lastAnnounced)
            {
                s->hostLatency->changed (s->host);
                s->lastAnnounced = lat;
            }

            s->active.store (true, std::memory_order_release);
            return true;
        }

        static void clapDeactivate (const clap_plugin_t* p)
        {
            self (p)->active.store (false, std::memory_order_release);
        }

        static bool clapStartProcessing (const clap_plugin_t*) { return true; }
        static void clapStopProcessing  (const clap_plugin_t*) {}

        static void clapReset (const clap_plugin_t*)
        {
            // [audio-thread] — must not allocate. The DSP cores expose no cheap,
            // non-reallocating reset yet, so (like the S2 spike) this is a no-op; a
            // reset() that clears the OLA/detector rings in place is a later
            // refinement. Latency does not change on reset, so nothing to re-latch.
        }

        // ─────────────────────────── audio ports ────────────────────────────
        static std::uint32_t audioPortsCount (const clap_plugin_t*, bool is_input)
        {
            if (is_input)
                return Policy::kHasSidechain ? 2u : 1u;
            return 1u;
        }

        static bool audioPortsGet (const clap_plugin_t*, std::uint32_t index, bool is_input,
                                   clap_audio_port_info_t* info)
        {
            if (! is_input)
            {
                if (index != 0) return false;
                info->id            = 0;
                copyName (info->name, "Output");
                info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
                info->channel_count = 2;
                info->port_type     = CLAP_PORT_STEREO;
                info->in_place_pair = 0; // pairs with input id 0 (in-place capable)
                return true;
            }
            if (index == 0) // main input
            {
                info->id            = 0;
                copyName (info->name, "Input");
                info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
                info->channel_count = 2;
                info->port_type     = CLAP_PORT_STEREO;
                info->in_place_pair = 0; // pairs with output id 0
                return true;
            }
            if (Policy::kHasSidechain && index == 1) // optional stereo sidechain
            {
                info->id            = 1;
                copyName (info->name, "Sidechain");
                info->flags         = 0; // not the main port
                info->channel_count = 2;
                info->port_type     = CLAP_PORT_STEREO;
                info->in_place_pair = CLAP_INVALID_ID;
                return true;
            }
            return false;
        }

        // ───────────────────────────── params ───────────────────────────────
        static std::uint32_t paramsCount (const clap_plugin_t* p) { return self (p)->bridge.count(); }

        static bool paramsGetInfo (const clap_plugin_t* p, std::uint32_t idx, clap_param_info_t* info)
        {
            return self (p)->bridge.getInfo (idx, info);
        }

        static bool paramsGetValue (const clap_plugin_t* p, clap_id id, double* out)
        {
            auto* s  = self (p);
            const int si = s->bridge.storeIndexForId (id);
            if (si < 0) return false;
            *out = static_cast<double> (s->store.value (si));
            return true;
        }

        static bool paramsValueToText (const clap_plugin_t* p, clap_id id, double value,
                                       char* out, std::uint32_t cap)
        {
            auto* s  = self (p);
            const int si = s->bridge.storeIndexForId (id);
            if (si < 0) return false;
            s->bridge.valueToText (si, value, out, cap);
            return true;
        }

        static bool paramsTextToValue (const clap_plugin_t* p, clap_id id, const char* text, double* out)
        {
            auto* s  = self (p);
            const int si = s->bridge.storeIndexForId (id);
            if (si < 0) return false;
            return s->bridge.textToValue (si, text, out);
        }

        static void paramsFlush (const clap_plugin_t* p, const clap_input_events_t* in,
                                 const clap_output_events_t* out)
        {
            auto* s = self (p);
            s->applyInputEvents (in);
            s->emitHostWrites (out); // GUI edits made while not processing reach the host here
        }

        // Apply parameter events into the store at BLOCK granularity (last value per
        // block wins), matching today's per-block APVTS pull. Real-time safe.
        void applyInputEvents (const clap_input_events_t* in) noexcept
        {
            if (in == nullptr) return;
            const std::uint32_t n = in->size (in);
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const clap_event_header_t* h = in->get (in, i);
                if (h == nullptr) continue;
                if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
                if (h->type != CLAP_EVENT_PARAM_VALUE)        continue;
                const auto* ev = reinterpret_cast<const clap_event_param_value_t*> (h);
                const int si = bridge.storeIndexForId (ev->param_id);
                if (si >= 0)
                    store.setFromHost (si, static_cast<float> (ev->value));
            }
        }

        // Relay GUI-driven parameter edits to the host as CLAP output events. The
        // editor (main thread) writes edits into the ParamStore via the UI gesture
        // path (beginGesture / setFromUi / endGesture), which enqueues a lock-free
        // HostWrite per event; the shell is the SINGLE consumer of that queue and
        // drains it here, emitting the matching CLAP output event so the DAW records
        // automation from GUI knob moves (begin/value/end gestures) — the capability
        // the shipping JUCE build got from its APVTS attachments. Host→plugin input
        // (automation playback) rides applyInputEvents and never enqueues a
        // HostWrite, so there is no echo loop. Bulk GUI changes (preset load, A/B
        // switch) use setFromHost (no HostWrite) + clap_host_params.rescan(VALUES)
        // instead, so they update the host WITHOUT writing an automation point.
        //
        // Called from process() (audio thread, process->out_events) and from
        // params.flush() (main thread when inactive, its out queue). CLAP guarantees
        // flush and process never run concurrently, and the editor no longer drains
        // the queue (it observes gestureEndCount() for undo), so there is exactly one
        // consumer at any instant. Real-time safe: draining is a lock-free ring read,
        // try_push is the host's RT-safe sink; no allocation, lock, or syscall. The
        // event construction lives in the free function (unit-tested in the shell).
        void emitHostWrites (const clap_output_events_t* out) noexcept
        {
            emitParamEventsToHost (store, bridge, out);
        }

        // ───────────────────────────── state ────────────────────────────────
        static bool clapStateSave (const clap_plugin_t* p, const clap_ostream_t* stream)
        {
            auto* s = self (p);
            return saveState (s->store, s->session.currentProgram(), stream);
        }

        static bool clapStateLoad (const clap_plugin_t* p, const clap_istream_t* stream)
        {
            auto* s = self (p);
            int presetIndex = 0;
            std::function<void (factory_presets::StateModel&)> migrate = &Policy::migrateState;
            if (! loadState (s->store, stream, migrate, presetIndex))
                return false;

            // Adopt the restored program index as the clean dirty-tracking baseline
            // (parameters are already in place, so no re-apply).
            s->session.setCurrentProgramClean (presetIndex);

            // Tell the host the parameter values (and their text) changed.
            if (s->hostParams != nullptr)
                s->hostParams->rescan (s->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);

            // Resync an open editor to the replaced state (clears/re-seeds its undo
            // timeline, rebuilds the preset selector, redraws) — the CLAP counterpart
            // of JUCE's setStateInformation -> replaceState follow-through. Nothing to
            // do when no editor is created (headless host, or GUI not opened).
            if (s->editor)
                s->editor->onHostStateRestored();

            // A restore may change a latency-affecting parameter (Quality). CLAP only
            // permits latency to change across (de)activation, so if we are active,
            // ask the host to restart — activate() then re-primes the latency and
            // announces it via clap_host_latency.changed(). If inactive, the next
            // activate() picks up the new value anyway.
            if (s->active.load (std::memory_order_acquire))
                s->host->request_restart (s->host);
            return true;
        }

        // Plugin-initiated state dirtiness (non-parameter changes, e.g. a future
        // GUI preset pick). Parameter value changes are implicitly dirty per the
        // CLAP contract, so they don't call this. Wired here; the trigger is the
        // 3b editor / preset selector.
        void markStateDirty() noexcept
        {
            if (hostState != nullptr)
                hostState->mark_dirty (host);
        }

        // ──────────────────────── latency / tail ────────────────────────────
        static std::uint32_t latencyGet (const clap_plugin_t* p)
        {
            return self (p)->reportedLatency.load (std::memory_order_relaxed);
        }

        static std::uint32_t tailGet (const clap_plugin_t* p)
        {
            // Mirrors the shipping build's tail (tail seconds == latencySamples()/
            // sampleRate): in samples that is exactly the reported latency.
            return self (p)->reportedLatency.load (std::memory_order_relaxed);
        }

        // ──────────────────────────── process ───────────────────────────────
        static clap_process_status clapProcess (const clap_plugin_t* p, const clap_process_t* process)
        {
            auto* s = self (p);
            ScopedNoDenormals denormalGuard; // FTZ/DAZ for the audio-boundary; restored on return

            // 1. Block-granular parameter delivery.
            s->applyInputEvents (process->in_events);

            // 1b. Relay any GUI-driven parameter edits (knob moves in the editor) to
            //     the host as CLAP output param/gesture events, so automation records.
            s->emitHostWrites (process->out_events);

            // 2. Audio I/O. Chunk 3a handles 32-bit buffers (CLAP requires 32-bit
            //    support; 64-bit is optional and not advertised).
            const std::uint32_t n = process->frames_count;
            if (process->audio_outputs_count == 0) return CLAP_PROCESS_CONTINUE;
            clap_audio_buffer_t* out = &process->audio_outputs[0];
            if (out->data32 == nullptr) return CLAP_PROCESS_CONTINUE;

            float* outL = out->channel_count > 0 ? out->data32[0] : nullptr;
            float* outR = out->channel_count > 1 ? out->data32[1] : nullptr;
            if (outL == nullptr) return CLAP_PROCESS_CONTINUE;

            const clap_audio_buffer_t* in =
                (process->audio_inputs_count > 0) ? &process->audio_inputs[0] : nullptr;
            const float* inL = (in && in->data32 && in->channel_count > 0) ? in->data32[0] : nullptr;
            const float* inR = (in && in->data32 && in->channel_count > 1) ? in->data32[1] : nullptr;

            // The core processes the main signal IN PLACE on outL/outR, so seed the
            // output buffers with the input first when not already in-place.
            if (inL != nullptr) { if (inL != outL) std::memcpy (outL, inL, sizeof (float) * n); }
            else                std::memset (outL, 0, sizeof (float) * n);

            if (outR != nullptr)
            {
                if (inR != nullptr)            { if (inR != outR) std::memcpy (outR, inR, sizeof (float) * n); }
                else if (inL != nullptr)       std::memcpy (outR, outL, sizeof (float) * n); // mono in -> stereo
                else                           std::memset (outR, 0, sizeof (float) * n);
            }

            // 3. Optional stereo sidechain (input port index 1) — passed only when the
            //    port exists AND carries data (a disconnected bus -> nullptr, which the
            //    core reads as "no sidechain", falling back to internal detection).
            const float* scL = nullptr;
            const float* scR = nullptr;
            if (Policy::kHasSidechain && process->audio_inputs_count > 1)
            {
                const clap_audio_buffer_t* sc = &process->audio_inputs[1];
                if (sc->data32 != nullptr && sc->channel_count > 0)
                {
                    scL = sc->data32[0];
                    scR = sc->channel_count > 1 ? sc->data32[1] : sc->data32[0]; // mono SC duplicates
                }
            }

            // 4. Snapshot the store into the core's parameter form and run the DSP.
            Policy::process (s->core, s->store, outL, outR, scL, scR, n);

            // 5. Latency-change detection. If the core settled to a new latency
            //    (a mid-stream Quality switch), defer a restart request to the main
            //    thread; the reported latency stays put until reactivation.
            const std::uint32_t live = Policy::latencySamples (s->core);
            if (live != s->reportedLatency.load (std::memory_order_relaxed))
            {
                if (! s->latencyRestartPending.exchange (true, std::memory_order_acq_rel))
                    s->host->request_callback (s->host); // -> clapOnMainThread
            }
            return CLAP_PROCESS_CONTINUE;
        }

        static void clapOnMainThread (const clap_plugin_t* p)
        {
            auto* s = self (p);
            // Deferred latency restart: ask the host to deactivate -> reactivate.
            // activate() re-latches the settled latency and clears the pending latch.
            if (s->latencyRestartPending.load (std::memory_order_acquire))
                s->host->request_restart (s->host);
        }

        // ────────────────────────── extensions ──────────────────────────────
        static const void* clapGetExtension (const clap_plugin_t*, const char* id)
        {
            if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &sAudioPorts;
            if (std::strcmp (id, CLAP_EXT_PARAMS)      == 0) return &sParams;
            if (std::strcmp (id, CLAP_EXT_STATE)       == 0) return &sState;
            if (std::strcmp (id, CLAP_EXT_LATENCY)     == 0) return &sLatency;
            if (std::strcmp (id, CLAP_EXT_TAIL)        == 0) return &sTail;

            // GUI (chunk 3b) — advertised ONLY when the Policy carries an editor.
            // For a GUI-less Policy this whole block folds away and the host sees a
            // headless plugin, exactly as in chunk 3a (see file header).
            if constexpr (PolicyHasEditor<Policy>::value)
            {
                if (std::strcmp (id, CLAP_EXT_GUI) == 0) return &sGui;
#ifdef __linux__
                if (std::strcmp (id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &sPosixFd;
#endif
            }
            return nullptr;
        }

        // ──────────────────────────── gui (3b) ──────────────────────────────
        // Thin [main-thread] delegation to the Policy's IClapEditor. The editor is
        // created lazily in guiCreate and owned here; guiDestroy tears it down. On
        // Linux the shell (not the editor) owns the host posix-fd register/unregister
        // round-trip, driven off the editor's window fd.
        static bool guiIsApiSupported (const clap_plugin_t* p, const char* api, bool floating)
        {
            auto* s = self (p);
            // Answerable before create(): construct the editor object if needed (it
            // allocates no native window until create()).
            if (! s->ensureEditor()) return false;
            return s->editor->isApiSupported (api, floating);
        }

        static bool guiGetPreferredApi (const clap_plugin_t* p, const char** api, bool* floating)
        {
            auto* s = self (p);
            if (! s->ensureEditor()) return false;
            return s->editor->getPreferredApi (api, floating);
        }

        static bool guiCreate (const clap_plugin_t* p, const char* api, bool floating)
        {
            auto* s = self (p);
            if (! s->ensureEditor()) return false;
            return s->editor->create (api, floating);
        }

        static void guiDestroy (const clap_plugin_t* p)
        {
            auto* s = self (p);
            if (! s->editor) return;
#ifdef __linux__
            const int fd = s->editor->posixFd();
            if (fd >= 0 && s->hostPosixFd != nullptr)
                s->hostPosixFd->unregister_fd (s->host, fd);
#endif
            s->editor->destroy();
            s->editor.reset();
        }

        static bool guiSetScale (const clap_plugin_t* p, double scale)
        {
            auto* s = self (p);
            return s->editor && s->editor->setScale (scale);
        }

        static bool guiGetSize (const clap_plugin_t* p, std::uint32_t* w, std::uint32_t* h)
        {
            auto* s = self (p);
            return s->editor && s->editor->getSize (w, h);
        }

        static bool guiCanResize (const clap_plugin_t* p)
        {
            auto* s = self (p);
            return s->editor && s->editor->canResize();
        }

        static bool guiGetResizeHints (const clap_plugin_t* p, clap_gui_resize_hints_t* hints)
        {
            auto* s = self (p);
            return s->editor && s->editor->getResizeHints (hints);
        }

        static bool guiAdjustSize (const clap_plugin_t* p, std::uint32_t* w, std::uint32_t* h)
        {
            auto* s = self (p);
            return s->editor && s->editor->adjustSize (w, h);
        }

        static bool guiSetSize (const clap_plugin_t* p, std::uint32_t w, std::uint32_t h)
        {
            auto* s = self (p);
            return s->editor && s->editor->setSize (w, h);
        }

        static bool guiSetParent (const clap_plugin_t* p, const clap_window_t* window)
        {
            auto* s = self (p);
            if (! s->editor) return false;
            if (! s->editor->setParent (window)) return false;
#ifdef __linux__
            const int fd = s->editor->posixFd();
            if (fd >= 0 && s->hostPosixFd != nullptr)
            {
                const clap_posix_fd_flags_t flags =
                    CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE | CLAP_POSIX_FD_ERROR;
                return s->hostPosixFd->register_fd (s->host, fd, flags);
            }
#endif
            return true;
        }

        static bool guiSetTransient (const clap_plugin_t*, const clap_window_t*) { return false; }
        static void guiSuggestTitle (const clap_plugin_t*, const char*) {}

        static bool guiShow (const clap_plugin_t* p) { auto* s = self (p); return s->editor && s->editor->show(); }
        static bool guiHide (const clap_plugin_t* p) { auto* s = self (p); return s->editor && s->editor->hide(); }

        static void posixFdOnFd (const clap_plugin_t* p, int /*fd*/, clap_posix_fd_flags_t flags)
        {
            auto* s = self (p);
            if (s->editor) s->editor->onPosixFd (flags);
        }

        // Construct (but do not open) the editor object. Returns false when the
        // Policy carries no editor (the `if constexpr` folds the whole GUI path
        // away for such policies). Cheap + allocation-only — no native window yet.
        bool ensureEditor()
        {
            if constexpr (PolicyHasEditor<Policy>::value)
            {
                if (! editor)
                    editor = Policy::makeEditor (core, store, session, host);
                return editor != nullptr;
            }
            else
            {
                return false;
            }
        }

        // ──────────────────── latency priming (main thread) ─────────────────
        void primeLatency()
        {
            const std::uint32_t need = Policy::primeFrames();
            if (need == 0) return; // constant-latency core: nothing to settle

            std::uint32_t chunk = maxFrames != 0 ? std::min<std::uint32_t> (maxFrames, 4096u) : 512u;
            if (chunk == 0) chunk = 512u;
            primeL.assign (chunk, 0.0f);
            primeR.assign (chunk, 0.0f);

            std::uint32_t done = 0;
            while (done < need)
            {
                const std::uint32_t nn = std::min (chunk, need - done);
                std::fill (primeL.begin(), primeL.begin() + nn, 0.0f);
                std::fill (primeR.begin(), primeR.begin() + nn, 0.0f);
                Policy::process (core, store, primeL.data(), primeR.data(), nullptr, nullptr, nn);
                done += nn;
            }
        }

        // ───────────────────────────── data ─────────────────────────────────
        const clap_host_t*         host       = nullptr;
        const clap_host_params_t*  hostParams  = nullptr;
        const clap_host_latency_t* hostLatency = nullptr;
        const clap_host_state_t*   hostState   = nullptr;
        const clap_host_posix_fd_support_t* hostPosixFd = nullptr; // Linux GUI event pump

        std::vector<factory_params::ParamDesc> paramTable; // owns the table (backs the bridge)
        factory_params::ParamStore             store;      // copies the table; the live value store
        ParamBridge                            bridge;     // holds a ref to paramTable
        factory_presets::PresetSession         session;    // drives the store for presets/program idx
        typename Policy::Core                  core;       // the DSP core

        // The 3b editor aliases core / store / session (its feed + preset/A-B models
        // hold refs), so it is declared LAST — destroyed FIRST — guaranteeing those
        // objects outlive it even if a host tears the plugin down without a prior
        // gui.destroy(). Null unless the Policy carries an editor.
        std::unique_ptr<IClapEditor> editor;

        clap_plugin_t plugin {};

        double        sampleRate = 0.0;
        std::uint32_t maxFrames  = 0;

        std::atomic<bool>          active                { false };
        std::atomic<std::uint32_t> reportedLatency       { 0 };
        std::atomic<bool>          latencyRestartPending { false };
        std::uint32_t              lastAnnounced         = 0xffffffffu; // main-thread only

        std::vector<float> primeL, primeR; // silence scratch for activate() priming

        // Per-instantiation extension vtables.
        static const clap_plugin_audio_ports_t sAudioPorts;
        static const clap_plugin_params_t      sParams;
        static const clap_plugin_state_t       sState;
        static const clap_plugin_latency_t     sLatency;
        static const clap_plugin_tail_t        sTail;
        static const clap_plugin_gui_t         sGui;      // returned only when PolicyHasEditor
        static const clap_plugin_posix_fd_support_t sPosixFd; // Linux only
    };

    template <class Policy>
    const clap_plugin_audio_ports_t ClapShellPlugin<Policy>::sAudioPorts {
        &ClapShellPlugin<Policy>::audioPortsCount,
        &ClapShellPlugin<Policy>::audioPortsGet
    };

    template <class Policy>
    const clap_plugin_params_t ClapShellPlugin<Policy>::sParams {
        &ClapShellPlugin<Policy>::paramsCount,
        &ClapShellPlugin<Policy>::paramsGetInfo,
        &ClapShellPlugin<Policy>::paramsGetValue,
        &ClapShellPlugin<Policy>::paramsValueToText,
        &ClapShellPlugin<Policy>::paramsTextToValue,
        &ClapShellPlugin<Policy>::paramsFlush
    };

    template <class Policy>
    const clap_plugin_state_t ClapShellPlugin<Policy>::sState {
        &ClapShellPlugin<Policy>::clapStateSave,
        &ClapShellPlugin<Policy>::clapStateLoad
    };

    template <class Policy>
    const clap_plugin_latency_t ClapShellPlugin<Policy>::sLatency {
        &ClapShellPlugin<Policy>::latencyGet
    };

    template <class Policy>
    const clap_plugin_tail_t ClapShellPlugin<Policy>::sTail {
        &ClapShellPlugin<Policy>::tailGet
    };

    template <class Policy>
    const clap_plugin_gui_t ClapShellPlugin<Policy>::sGui {
        &ClapShellPlugin<Policy>::guiIsApiSupported,
        &ClapShellPlugin<Policy>::guiGetPreferredApi,
        &ClapShellPlugin<Policy>::guiCreate,
        &ClapShellPlugin<Policy>::guiDestroy,
        &ClapShellPlugin<Policy>::guiSetScale,
        &ClapShellPlugin<Policy>::guiGetSize,
        &ClapShellPlugin<Policy>::guiCanResize,
        &ClapShellPlugin<Policy>::guiGetResizeHints,
        &ClapShellPlugin<Policy>::guiAdjustSize,
        &ClapShellPlugin<Policy>::guiSetSize,
        &ClapShellPlugin<Policy>::guiSetParent,
        &ClapShellPlugin<Policy>::guiSetTransient,
        &ClapShellPlugin<Policy>::guiSuggestTitle,
        &ClapShellPlugin<Policy>::guiShow,
        &ClapShellPlugin<Policy>::guiHide
    };

    template <class Policy>
    const clap_plugin_posix_fd_support_t ClapShellPlugin<Policy>::sPosixFd {
        &ClapShellPlugin<Policy>::posixFdOnFd
    };
} // namespace factory_shell
