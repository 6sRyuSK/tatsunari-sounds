//
// clap_shell_test.cpp — headless verification of the resonance-suppressor CLAP
// shell's host-facing behaviour that the 3.0.0 cutover added / made shipping:
//
//   1. PARAM OUTPUT EVENTS (ship-blocker): a GUI knob edit taken through the
//      ParamStore UI-gesture path (beginGesture / setFromUi / endGesture) must be
//      relayed to the host as CLAP output events — a PARAM_GESTURE_BEGIN, a
//      PARAM_VALUE carrying the snapped real value, and a PARAM_GESTURE_END, each
//      on the parameter's CLAP id (uid). A LEGACY (non-exposed) parameter has no
//      CLAP surface, so its edits emit NOTHING. (factory_shell::emitParamEventsToHost
//      — the exact code ClapShellPlugin::emitHostWrites calls in process()/flush().)
//
//   2. STATE CLEAN BREAK: the shipping state path is StateCodec. A genuine
//      (stateVersion>=1) blob round-trips exactly; a JUCE/APVTS-style version-0
//      blob (old session, or any foreign <PARAMS> blob) must FAIL GRACEFULLY —
//      reset to defaults, no import, no crash, no hang — and a truncated / wrong-
//      magic / non-PARAMS blob must be rejected (loadState false, store untouched).
//
//   3. A/B COMPARE: rs_ui::AbCompareModel reproduces the JUCE 2.1.0 setABSlot /
//      copyActiveToOther semantics over {full parameter state + program index}.
//
// Links ONLY factory_shell (+ its factory_params / factory_presets); no JUCE, no
// Visage. One ctest case (nothing here is sample-rate dependent).
//
#include "factory_shell/ClapParamBridge.h"
#include "factory_shell/ClapStateBridge.h"
#include "factory_shell/ClapShellPlugin.h" // drive the whole shell via its clap vtable (T1)

#include "factory_params/ParamStore.h"
#include "factory_presets/PresetBank.h"
#include "factory_presets/PresetSession.h"
#include "factory_presets/StateCodec.h"

#include "Source/Params.h"          // resonance_suppressor_params::buildRsParams()
#include "Source/FactoryPresets.h"  // resonance_suppressor_presets::bank / kExclude
#include "Source/StateMigration.h"  // resonance_suppressor_state::cleanBreakMigrate()
#include "ui/RsAbState.h"           // rs_ui::AbCompareModel
#include "shell/RsClapEditor.h"     // rs_shell::snapEditorSizeForScale (T2; visage-free header)

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    void check (bool ok, const char* what)
    {
        if (! ok) { std::printf ("  FAIL: %s\n", what); ++g_failures; }
    }

    bool exposed (const factory_params::ParamDesc& d)
    {
        return (d.flags & factory_params::kFlagLegacyJuceOnly) == 0;
    }

    // ---- mock clap_output_events_t: capture every pushed event ----------------
    struct Captured { std::uint16_t type; clap_id id; double value; };
    struct MockOut
    {
        std::vector<Captured> events;
        clap_output_events_t  api;

        MockOut()
        {
            api.ctx      = this;
            api.try_push = &tryPush;
        }
        static bool CLAP_ABI tryPush (const clap_output_events_t* list, const clap_event_header_t* h)
        {
            auto* self = static_cast<MockOut*> (list->ctx);
            if (h->type == CLAP_EVENT_PARAM_VALUE)
            {
                const auto* e = reinterpret_cast<const clap_event_param_value_t*> (h);
                self->events.push_back ({ h->type, e->param_id, e->value });
            }
            else
            {
                const auto* e = reinterpret_cast<const clap_event_param_gesture_t*> (h);
                self->events.push_back ({ h->type, e->param_id, 0.0 });
            }
            return true;
        }
    };

    // ---- mock clap streams over a byte buffer ---------------------------------
    struct MockStream
    {
        std::vector<unsigned char> data;
        std::uint64_t              readPos = 0;
        clap_istream_t             istream;
        clap_ostream_t             ostream;

        MockStream()
        {
            istream.ctx  = this; istream.read  = &readFn;
            ostream.ctx  = this; ostream.write = &writeFn;
        }
        static std::int64_t CLAP_ABI readFn (const clap_istream_t* s, void* buf, std::uint64_t size)
        {
            auto* self = static_cast<MockStream*> (s->ctx);
            const std::uint64_t remain = self->data.size() - self->readPos;
            const std::uint64_t n = size < remain ? size : remain;
            if (n > 0) std::memcpy (buf, self->data.data() + self->readPos, static_cast<std::size_t> (n));
            self->readPos += n;
            return static_cast<std::int64_t> (n); // 0 at EOF
        }
        static std::int64_t CLAP_ABI writeFn (const clap_ostream_t* s, const void* buf, std::uint64_t size)
        {
            auto* self = static_cast<MockStream*> (s->ctx);
            const auto* p = static_cast<const unsigned char*> (buf);
            self->data.insert (self->data.end(), p, p + size);
            return static_cast<std::int64_t> (size);
        }
    };

    factory_presets::StateBlob juceStyleBlob() // no stateVersion attribute -> version 0
    {
        return factory_presets::frame (
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<PARAMS presetIndex=\"0\">\n"
            "  <PARAM id=\"depth\" value=\"73.5\"/>\n"
            "  <PARAM id=\"mix\" value=\"12\"/>\n"
            "  <PARAM id=\"attack\" value=\"150\"/>\n"
            "</PARAMS>");
    }

    // ==== T1 fixtures: drive the real ClapShellPlugin vtable with a mock editor ====

    // A minimal editor recording only what T1 (F2) asserts: how many times the shell
    // told it the host restored state. Everything else returns a benign default.
    struct MockClapEditor final : factory_shell::IClapEditor
    {
        int onHostStateRestoredCount = 0;

        bool isApiSupported (const char*, bool) const noexcept override { return true; }
        bool getPreferredApi (const char** api, bool* fl) const noexcept override
        { if (api) *api = "mock"; if (fl) *fl = false; return true; }
        bool create (const char*, bool) noexcept override { return true; }
        void destroy() noexcept override {}
        bool setScale (double) noexcept override { return false; }
        bool getSize (std::uint32_t*, std::uint32_t*) noexcept override { return false; }
        bool canResize() const noexcept override { return false; }
        bool getResizeHints (clap_gui_resize_hints_t*) noexcept override { return false; }
        bool adjustSize (std::uint32_t*, std::uint32_t*) noexcept override { return false; }
        bool setSize (std::uint32_t, std::uint32_t) noexcept override { return false; }
        bool setParent (const clap_window_t*) noexcept override { return false; }
        bool show() noexcept override { return true; }
        bool hide() noexcept override { return true; }
        void onHostStateRestored() noexcept override { ++onHostStateRestoredCount; }
    };

    // makeEditor is a static Policy hook, so it publishes the just-built editor here
    // for the scenario to observe (the shell owns it). Reset before each scenario.
    MockClapEditor* g_lastEditor = nullptr;

    struct MockCore {};

    // A shell Policy that DOES carry an editor (kHasEditor) but whose DSP is inert, so
    // the shell advertises clap.gui and runs the F2 state-restore -> editor sync path
    // without any real audio work. Uses the real RS param/preset tables (already
    // linked) so paramsGetValue / state round-trip behave like the shipping build.
    struct MockEditorPolicy
    {
        using Core = MockCore;

        static const clap_plugin_descriptor_t* descriptor()
        {
            static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr };
            static const clap_plugin_descriptor_t d = {
                CLAP_VERSION_INIT, "jp.tatsunari-sounds.rs-mock", "RS Mock", "Tatsunari Sounds",
                "", "", "", "0.0.0", "", features
            };
            return &d;
        }

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

        static constexpr bool kHasSidechain = false;

        static bool isClapExposed (const factory_params::ParamDesc& d) { return exposed (d); }
        static void migrateState (factory_presets::StateModel& m)
        {
            resonance_suppressor_state::cleanBreakMigrate (m);
        }

        static void prepare (Core&, double, std::uint32_t) {}
        static void process (Core&, const factory_params::ParamStore&,
                             float*, float*, const float*, const float*, std::uint32_t) {}
        static std::uint32_t latencySamples (const Core&) { return 0u; }
        static std::uint32_t primeFrames() { return 0u; }

        static constexpr bool kHasEditor = true;
        static std::unique_ptr<factory_shell::IClapEditor>
        makeEditor (Core&, factory_params::ParamStore&, factory_presets::PresetSession&, const clap_host_t*)
        {
            auto e = std::make_unique<MockClapEditor>();
            g_lastEditor = e.get();
            return e;
        }
    };

    // A mock clap_host_t recording the round-trips clapStateLoad makes (rescan flags,
    // mark_dirty, request_restart) and exposing the params + state host extensions.
    struct MockHost
    {
        int                     rescanCount   = 0;
        clap_param_rescan_flags lastRescan    = 0;
        int                     flushCount    = 0;
        int                     markDirtyCount = 0;
        int                     requestRestartCount = 0;

        clap_host_params_t params {};
        clap_host_state_t  state {};
        clap_host_t        host {};

        MockHost()
        {
            params.rescan        = &rescanCb;
            params.clear         = &clearCb;
            params.request_flush = &flushCb;
            state.mark_dirty     = &markDirtyCb;

            host.clap_version    = CLAP_VERSION;
            host.host_data       = this;
            host.name            = "clap_shell_test";
            host.vendor          = "";
            host.url             = "";
            host.version         = "";
            host.get_extension   = &getExt;
            host.request_restart = &reqRestart;
            host.request_process = &reqProcess;
            host.request_callback = &reqCallback;
        }

        static MockHost* of (const clap_host_t* h) { return static_cast<MockHost*> (h->host_data); }
        static const void* CLAP_ABI getExt (const clap_host_t* h, const char* id)
        {
            if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &of (h)->params;
            if (std::strcmp (id, CLAP_EXT_STATE)  == 0) return &of (h)->state;
            return nullptr;
        }
        static void CLAP_ABI rescanCb (const clap_host_t* h, clap_param_rescan_flags f)
        { auto* s = of (h); ++s->rescanCount; s->lastRescan = f; }
        static void CLAP_ABI clearCb (const clap_host_t*, clap_id, clap_param_clear_flags) {}
        static void CLAP_ABI flushCb (const clap_host_t* h) { ++of (h)->flushCount; }
        static void CLAP_ABI markDirtyCb (const clap_host_t* h) { ++of (h)->markDirtyCount; }
        static void CLAP_ABI reqRestart (const clap_host_t* h) { ++of (h)->requestRestartCount; }
        static void CLAP_ABI reqProcess (const clap_host_t*) {}
        static void CLAP_ABI reqCallback (const clap_host_t*) {}
    };
}

int main()
{
    using namespace factory_params;
    using namespace factory_presets;

    // ================= 1. PARAM OUTPUT EVENTS =================================
    {
        std::vector<ParamDesc> descs = resonance_suppressor_params::buildRsParams();
        ParamStore store (descs);
        factory_shell::ParamBridge bridge (descs, &exposed);

        const int depth = store.indexOf ("depth");
        const int sharp = store.indexOf ("sharpness"); // legacy (kFlagLegacyJuceOnly)
        check (depth >= 0 && sharp >= 0, "param indices resolve");

        // clapIdForStoreIndex maps exposed -> uid, rejects legacy.
        clap_id depthId = 0, sharpId = 0;
        check (bridge.clapIdForStoreIndex (depth, depthId), "depth has a CLAP id");
        check (depthId == descs[(std::size_t) depth].uid, "depth CLAP id == uid");
        check (! bridge.clapIdForStoreIndex (sharp, sharpId), "legacy sharpness has NO CLAP id");

        // A GUI knob drag on depth: begin, set 55.0 (on-grid), end.
        store.beginGesture (depth);
        store.setFromUi (depth, 55.0f);
        store.endGesture (depth);
        // A legacy edit in the same batch must NOT reach the host.
        store.setFromUi (sharp, 42.0f);

        MockOut out;
        factory_shell::emitParamEventsToHost (store, bridge, &out.api);

        check (out.events.size() == 3, "exactly 3 host events for one gesture (begin/value/end); legacy dropped");
        if (out.events.size() == 3)
        {
            check (out.events[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN && out.events[0].id == depthId,
                   "event 0 = GESTURE_BEGIN on depth");
            check (out.events[1].type == CLAP_EVENT_PARAM_VALUE && out.events[1].id == depthId
                       && out.events[1].value == 55.0, "event 1 = PARAM_VALUE(depth, 55.0)");
            check (out.events[2].type == CLAP_EVENT_PARAM_GESTURE_END && out.events[2].id == depthId,
                   "event 2 = GESTURE_END on depth");
        }

        // The queue is now drained: a second emit with no new edits pushes nothing.
        MockOut out2;
        factory_shell::emitParamEventsToHost (store, bridge, &out2.api);
        check (out2.events.empty(), "drained queue emits nothing on re-drain");

        // Host->plugin input (setFromHost, automation playback) must NOT echo back.
        store.setFromHost (depth, 10.0f);
        MockOut out3;
        factory_shell::emitParamEventsToHost (store, bridge, &out3.api);
        check (out3.events.empty(), "setFromHost does not enqueue a host output event (no echo)");
    }

    // ================= 2. STATE CLEAN BREAK ===================================
    {
        std::vector<ParamDesc> descs = resonance_suppressor_params::buildRsParams();
        const int depth = ParamStore (descs).indexOf ("depth");
        const float depthDefault = descs[(std::size_t) depth].defaultValue;

        std::function<void (StateModel&)> migrate = &resonance_suppressor_state::cleanBreakMigrate;

        // (a) Genuine StateCodec (v1) blob round-trips exactly.
        {
            ParamStore store (descs);
            store.setFromHost (depth, 77.0f);
            MockStream s;
            check (factory_shell::saveState (store, /*presetIndex*/ 3, &s.ostream), "saveState ok");

            ParamStore restored (descs);
            int idx = -1;
            check (factory_shell::loadState (restored, &s.istream, migrate, idx), "loadState(v1) ok");
            check (restored.value (depth) == 77.0f, "v1 blob restores depth exactly");
            check (idx == 3, "v1 blob restores presetIndex");
        }

        // (b) JUCE / version-0 blob: CLEAN BREAK -> defaults, no import.
        {
            const StateBlob blob = juceStyleBlob();
            MockStream s; s.data.assign (blob.begin(), blob.end());
            ParamStore store (descs);
            store.setFromHost (depth, 5.0f); // a non-default prior value
            int idx = -99;
            const bool ok = factory_shell::loadState (store, &s.istream, migrate, idx);
            check (ok, "loadState(v0) returns true (handled, not an error)");
            check (store.value (depth) == depthDefault, "v0 (JUCE) blob does NOT import: depth back to default");
            check (idx == 0, "v0 blob -> program 0");
        }

        // (c) Garbage / wrong-magic / truncated -> false, store untouched.
        auto rejects = [&] (const std::vector<unsigned char>& bytes, const char* what)
        {
            MockStream s; s.data = bytes;
            ParamStore store (descs);
            store.setFromHost (depth, 33.0f);
            int idx = -1;
            const bool ok = factory_shell::loadState (store, &s.istream, migrate, idx);
            check (! ok, what);
            check (store.value (depth) == 33.0f, "rejected blob leaves the store untouched");
        };
        rejects ({ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 }, "random bytes rejected");
        {
            // valid magic but truncated payload length.
            StateBlob good = encode ([] { StateModel m; m.stateVersion = 1; m.params.push_back ({ "depth", 50.0 }); return m; }());
            good.resize (6); // chop below the 8-byte frame header
            rejects (good, "truncated frame rejected");
        }
        {
            // a non-PARAMS root (a foreign plugin's XML) with valid framing.
            StateBlob foreign = frame ("<?xml version=\"1.0\"?><OTHERPLUGIN a=\"1\"/>");
            rejects ({ foreign.begin(), foreign.end() }, "non-PARAMS root rejected");
        }
    }

    // ================= 3. A/B COMPARE (params + program) =====================
    {
        std::vector<ParamDesc> descs = resonance_suppressor_params::buildRsParams();
        ParamStore store (descs);
        const int depth = store.indexOf ("depth");
        const int mix   = store.indexOf ("mix");

        int program = 0; // stands in for PresetSession::currentProgram
        rs_ui::AbCompareModel ab (store,
                                  [&] { return program; },
                                  [&] (int p) { program = p; });

        // Slot A: depth 10, mix 20, program 1.
        store.setFromHost (depth, 10.0f); store.setFromHost (mix, 20.0f); program = 1;

        ab.setActiveSlot (1); // stash A(10,20,prog1), seed B from current, active=B
        check (ab.activeSlot() == 1, "A/B active slot -> B");

        // Slot B: change to depth 90, mix 80, program 2.
        store.setFromHost (depth, 90.0f); store.setFromHost (mix, 80.0f); program = 2;

        ab.setActiveSlot (0); // stash B(90,80,prog2), restore A
        check (store.value (depth) == 10.0f && store.value (mix) == 20.0f, "switch back to A restores A's params");
        check (program == 1, "switch back to A restores A's program index");

        ab.setActiveSlot (1); // restore B
        check (store.value (depth) == 90.0f && store.value (mix) == 80.0f, "switch to B restores B's params");
        check (program == 2, "switch to B restores B's program index");

        // copyActiveToOther: B(90,80,prog2) -> A. Then A holds B's state.
        ab.copyActiveToOther();
        ab.setActiveSlot (0);
        check (store.value (depth) == 90.0f && store.value (mix) == 80.0f && program == 2,
               "copyActiveToOther copied active (B) onto the other (A)");
    }

    // ============ 4. STATE RESTORE -> EDITOR SYNC (F2) =======================
    // Drives the REAL ClapShellPlugin through its clap vtable: a host state.load must
    // notify an open editor (onHostStateRestored) exactly once, AFTER rescanning the
    // host, and request a restart only while active. No editor -> no notify, no crash.
    {
        using RsMockShell = factory_shell::ClapShellPlugin<MockEditorPolicy>;

        std::vector<ParamDesc> descs = resonance_suppressor_params::buildRsParams();
        const int     depthIx = ParamStore (descs).indexOf ("depth");
        const clap_id depthId = descs[(std::size_t) depthIx].uid;

        // A canonical v1 blob: depth 63, mix 25, presetIndex 2.
        StateModel m; m.stateVersion = 1; m.presetIndex = 2;
        m.params.push_back ({ "depth", 63.0 });
        m.params.push_back ({ "mix",   25.0 });
        const StateBlob blob = encode (m);

        // -- Scenario A: editor present, plugin INACTIVE -----------------------
        {
            MockHost mh;
            g_lastEditor = nullptr;
            clap_plugin_t* plugin = RsMockShell::create (&mh.host);
            check (plugin != nullptr && plugin->init (plugin), "A: plugin init");

            const auto* gui = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI));
            check (gui != nullptr, "A: clap.gui advertised (Policy carries an editor)");
            if (gui) gui->is_api_supported (plugin, "mock", false); // ensureEditor -> makeEditor
            check (g_lastEditor != nullptr, "A: mock editor constructed on is_api_supported");

            const auto* state  = static_cast<const clap_plugin_state_t*>  (plugin->get_extension (plugin, CLAP_EXT_STATE));
            const auto* params = static_cast<const clap_plugin_params_t*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
            check (state != nullptr && params != nullptr, "A: state + params extensions present");

            MockStream s; s.data.assign (blob.begin(), blob.end());
            const bool ok = state->load (plugin, &s.istream);
            check (ok, "A: state.load returns true");
            check (g_lastEditor != nullptr && g_lastEditor->onHostStateRestoredCount == 1,
                   "A: editor onHostStateRestored called exactly once");
            check (mh.rescanCount == 1
                   && mh.lastRescan == (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT),
                   "A: host rescan(VALUES|TEXT) once");
            check (mh.requestRestartCount == 0, "A: inactive -> request_restart NOT called");

            double dv = -1.0;
            check (params->get_value (plugin, depthId, &dv) && std::abs (dv - 63.0) < 1e-6,
                   "A: depth restored into the store");

            MockStream s2;
            check (state->save (plugin, &s2.ostream), "A: re-save ok");
            auto decoded = factory_presets::decode (s2.data.data(), s2.data.size());
            check (decoded && decoded->presetIndex == 2, "A: re-saved blob keeps presetIndex 2");

            plugin->destroy (plugin);
        }

        // -- Scenario B: editor present, plugin ACTIVE -------------------------
        {
            MockHost mh;
            g_lastEditor = nullptr;
            clap_plugin_t* plugin = RsMockShell::create (&mh.host);
            plugin->init (plugin);
            const auto* gui = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI));
            if (gui) gui->is_api_supported (plugin, "mock", false);
            check (plugin->activate (plugin, 48000.0, 32, 512), "B: activate");

            const auto* state = static_cast<const clap_plugin_state_t*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
            MockStream s; s.data.assign (blob.begin(), blob.end());
            check (state->load (plugin, &s.istream), "B: state.load ok (active)");
            check (g_lastEditor != nullptr && g_lastEditor->onHostStateRestoredCount == 1,
                   "B: editor synced once (active)");
            check (mh.requestRestartCount == 1, "B: active -> request_restart called once");

            plugin->deactivate (plugin);
            plugin->destroy (plugin);
        }

        // -- Scenario C: NO editor created -------------------------------------
        {
            MockHost mh;
            g_lastEditor = nullptr;
            clap_plugin_t* plugin = RsMockShell::create (&mh.host);
            plugin->init (plugin);
            const auto* state = static_cast<const clap_plugin_state_t*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
            MockStream s; s.data.assign (blob.begin(), blob.end());
            check (state->load (plugin, &s.istream), "C: state.load ok (no editor)");
            check (g_lastEditor == nullptr, "C: no editor was ever created");
            check (mh.rescanCount == 1, "C: rescan still fires without an editor (no crash)");
            plugin->destroy (plugin);
        }
    }

    // ============ 5. EDITOR SIZE SNAP (F3) ==================================
    // rs_shell::snapEditorSizeForScale: a Visage-free aspect/limit snap in LOGICAL
    // space, applied in NATIVE px via the DPI scale. Expected outputs are FIXED numeric
    // literals (independent hand-computed oracle), NOT re-derived from the impl.
    {
        auto snap = [] (double scale, std::uint32_t w, std::uint32_t h,
                        std::uint32_t& ow, std::uint32_t& oh)
        { ow = w; oh = h; rs_shell::snapEditorSizeForScale (scale, ow, oh); };

        std::uint32_t w = 0, h = 0;

        // scale 1.0 (macOS logical, or a 1x display): logical == native.
        snap (1.0, 1069, 747, w, h);   check (w == 1069 && h == 747, "snap 1.0: 1069x747 is a fixed point");
        snap (1.0, 10000, 10000, w, h); check (w == 1320 && h == 922, "snap 1.0: huge -> max 1320x922");
        snap (1.0, 1, 1, w, h);         check (w == 940 && h == 657, "snap 1.0: tiny -> min 940x657");

        // Aspect invariant for arbitrary inputs: |w*747 - h*1069| <= 747 (< ~1 logical px).
        for (std::uint32_t in : { 700u, 950u, 1100u, 1200u, 1319u, 2000u })
        {
            snap (1.0, in, in, w, h);
            long d = (long) w * 747 - (long) h * 1069;
            if (d < 0) d = -d;
            check (d <= 747, "snap 1.0: aspect within 1px for a square input");
        }

        // scale 1.5: the design proposal (native px) is ~713 logical wide -> min clamp
        // -> logical 940x657 -> native 1410x986 (940*1.5, round(657*1.5)=round(985.5)).
        snap (1.5, 1069, 747, w, h); check (w == 1410 && h == 986, "snap 1.5: native design proposal -> min -> 1410x986");

        // scale 2.0: a huge square proposal -> logical max 1320x922 -> native 2640x1844.
        snap (2.0, 5000, 5000, w, h); check (w == 2640 && h == 1844, "snap 2.0: huge -> native max 2640x1844");

        // Fixed point at each scale: re-snapping the snap's own output is a no-op.
        for (double sc : { 1.0, 1.5, 2.0 })
        {
            std::uint32_t w2, h2;
            snap (sc, 5000, 5000, w, h);  w2 = w; h2 = h; rs_shell::snapEditorSizeForScale (sc, w2, h2);
            check (w2 == w && h2 == h, "snap fixed point (max) at scale 1.0/1.5/2.0");
            snap (sc, 1, 1, w, h);        w2 = w; h2 = h; rs_shell::snapEditorSizeForScale (sc, w2, h2);
            check (w2 == w && h2 == h, "snap fixed point (min) at scale 1.0/1.5/2.0");
        }
    }

    if (g_failures == 0) std::printf ("clap_shell_test: ALL PASS\n");
    else                 std::printf ("clap_shell_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
