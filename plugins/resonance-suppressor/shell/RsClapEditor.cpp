//
// RsClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the RS CLAP
// plugin. Since the 共通化 refactor it DERIVES the shared
// factory_ui_visage::ResizableVisageClapEditor (which owns the identical CLAP↔Visage
// host boilerplate + the uniform-zoom / aspect-lock / Logic-AU resize-loop machinery),
// so this file keeps ONLY the RS-specific pieces: constructing rs_ui::RsEditor over the
// shell's live core/store/session, the real preset + A/B models, and the per-frame
// gesture pump. Compiled ONLY under FACTORY_RS_CLAP_GUI (the single visage-linking TU of
// the RS CLAP build).
//
// Backed by the SAME live objects the CLAP shell owns:
//   * rs_core::RsCore            -> rs_ui::RsFeedFromCore (real analyser feed, zero-copy);
//   * factory_params::ParamStore -> every control binds by string id;
//   * factory_presets::PresetSession -> the REAL program list + apply path.
// A/B compare is the real rs_ui::AbCompareModel (RsAbState.h). A bulk state change
// (preset apply / A-B switch / undo) relays rescan + mark-dirty (no per-parameter
// automation); individual knob edits reach the host as automation via the shell's
// GUI-edit -> CLAP output-event relay.
//
#include "RsClapEditor.h"

#include "RsEditor.h"
#include "RsFeedFromCore.h"
#include "RsTheme.h"
#include "RsModels.h"
#include "RsAbState.h"

#include "RsCore.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"

#include "factory_ui_visage/ClapEditorHost.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    // Design geometry: reference 1069x747, resize limits 471x329..1320x922 (rs_shell::
    // kRsGeometry), fixed aspect. The window OPENS at 706x493 (below the reference so host
    // chrome fits a laptop; the minimum drops to 471x329 so the k()-scaled UI can shrink).
    constexpr int kDefaultW = 706, kDefaultH = 493;

    // --- REAL preset model over the shell's PresetSession ---------------------
    // Lists Init + the six factory presets. load() applies the real program values through
    // the session, then fires onLoaded so the host re-pulls values + marks state dirty.
    // Returning true tells the editor a preset LOADED, so it clears its undo timeline.
    class SessionPresetModel final : public rs_ui::RsPresetModel
    {
    public:
        SessionPresetModel (factory_presets::PresetSession& session, std::function<void()> onLoaded)
            : session_ (session), onLoaded_ (std::move (onLoaded)) {}

        std::vector<rs_ui::RsPresetItem> items() const override
        {
            std::vector<rs_ui::RsPresetItem> v;
            const int n = session_.numPrograms();
            v.reserve (static_cast<std::size_t> (n));
            for (int i = 0; i < n; ++i)
                v.push_back ({ session_.programName (i), /*steppable*/ true, /*isAction*/ false });
            return v;
        }

        int currentIndex() const override { return session_.currentProgram(); }

        bool load (int index) override
        {
            if (index < 0 || index >= session_.numPrograms())
                return false;
            session_.applyProgram (index); // writes real values via ParamStore::setFromHost
            if (onLoaded_) onLoaded_();
            return true;
        }

    private:
        factory_presets::PresetSession& session_;
        std::function<void()>           onLoaded_;
    };

    // --- the IClapEditor implementation (resizable) ---------------------------
    class RsClapEditorImpl final : public factory_ui_visage::ResizableVisageClapEditor
    {
    public:
        RsClapEditorImpl (rs_core::RsCore& core, factory_params::ParamStore& store,
                          factory_presets::PresetSession& session, const clap_host_t* host)
            : ResizableVisageClapEditor (host, store, rs_shell::kRsGeometry, kDefaultW, kDefaultH),
              theme_ (rs_ui::RsTheme::defaults()),
              feed_ (core),
              presets_ (session, [this] { notifyHostEdited(); }),
              ab_ (store,
                   [&session] { return session.currentProgram(); },
                   [&session] (int i) { session.setCurrentProgramClean (i); },
                   [this] { notifyHostEdited(); })
        {
        }

        ~RsClapEditorImpl() override { destroy(); }

    protected:
        // Build rs_ui::RsEditor over the shell's live objects and wire its per-frame hooks.
        visage::Frame* buildEditor() override
        {
            editor_ = std::make_unique<rs_ui::RsEditor> (theme_, store_, feed_, presets_, ab_);

            // The editor's corner grip proposes a WINDOW size (its drag runs in the design
            // plane, converted to window units via setEditorWindowScale); the shared base
            // snaps it to the aspect + static + dynamic display limits and relays it. This is
            // the ONLY resize path a host with no window resize edge (Logic's AU) offers.
            editor_->onResizeRequest = [this] (float w, float h) { requestResizeFromEditor (w, h); };

            // Per analyser frame: drain the editor's gesture queue into its undo timeline
            // (the REAL feed refreshes itself from the audio thread), then flush pending GUI
            // edits to the host while the plugin is inactive.
            editor_->curve().onTick = [this]
            {
                if (editor_ == nullptr) return;
                editor_->pumpGestures();
                flushEditsIfInactive();
            };

            // Undo/redo applies a bulk parameter change; relay it like a preset / A-B switch
            // (rescan + mark-dirty, no per-parameter automation).
            editor_->onHistoryApplied = [this] { notifyHostEdited(); };

            app_->addChild (*editor_);
            return editor_.get();
        }

        visage::Frame* editorFrame() const override { return editor_.get(); }
        void resetEditor() override { editor_.reset(); }
        void setEditorWindowScale (float windowScale) override { if (editor_) editor_->setWindowScale (windowScale); }
        void onStateReplacedHook() override { if (editor_) editor_->onStateReplaced(); }

        // The analyser is live: let the core publish display spectra + run display-time
        // smoothing (both skipped while no editor is attached). Cleared on destroy.
        void onEditorCreated() override    { feed_.setDisplayActive (true); }
        void onEditorDestroying() override { feed_.setDisplayActive (false); }

    private:
        rs_ui::RsTheme        theme_;   // owned; the editor holds a const ref
        rs_ui::RsFeedFromCore feed_;    // real feed over the shell's RsCore
        SessionPresetModel    presets_; // real, over PresetSession
        rs_ui::AbCompareModel ab_;      // real A/B: params + program index (RsAbState.h)

        std::unique_ptr<rs_ui::RsEditor> editor_;
    };
} // namespace

namespace rs_shell
{
    std::unique_ptr<factory_shell::IClapEditor>
    makeRsClapEditor (rs_core::RsCore& core,
                      factory_params::ParamStore& store,
                      factory_presets::PresetSession& session,
                      const clap_host_t* host)
    {
        return std::make_unique<RsClapEditorImpl> (core, store, session, host);
    }
}
