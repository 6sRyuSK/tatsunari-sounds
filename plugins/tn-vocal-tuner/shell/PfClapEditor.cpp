//
// PfClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the tn-vocal-tuner
// CLAP plugin. Since the 共通化 refactor it DERIVES the shared
// factory_ui_visage::FixedSizeVisageClapEditor (which owns the identical CLAP↔Visage
// host boilerplate + the fixed-size resize surface), so this file keeps ONLY the
// tn-vocal-tuner-specific pieces: constructing pf_ui::PfEditor over the shell's live
// core/store/session, the real preset model, the status feed, and the per-frame flush.
// Compiled ONLY under FACTORY_PF_CLAP_GUI (the single visage-linking TU of the tn-vocal-tuner
// CLAP build).
//
// FIXED-SIZE editor (920×560 design px) — the DSP-phase UI has no resize surface.
//
#include "PfClapEditor.h"

#include "PfEditor.h"
#include "PfModels.h"

#include "PfCore.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"
#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ClapEditorHost.h"
#include "factory_ui_visage/UpdateUiHost.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef PF_CLAP_VERSION
#  define PF_CLAP_VERSION "0.0.0"
#endif

namespace
{
    // REAL preset model over the shell's PresetSession: lists Init + the bank, applies
    // through the session, then notifies the host (rescan + mark-dirty — a bulk change
    // records no per-parameter automation).
    class SessionPresetModel final : public pf_ui::PfPresetModel
    {
    public:
        SessionPresetModel (factory_presets::PresetSession& session, std::function<void()> onLoaded)
            : session_ (session), onLoaded_ (std::move (onLoaded)) {}

        std::vector<std::string> names() const override
        {
            std::vector<std::string> v;
            const int n = session_.numPrograms();
            v.reserve ((std::size_t) n);
            for (int i = 0; i < n; ++i)
                v.push_back (session_.programName (i));
            return v;
        }

        int currentIndex() const override { return session_.currentProgram(); }

        bool load (int index) override
        {
            if (index < 0 || index >= session_.numPrograms())
                return false;
            session_.applyProgram (index);
            if (onLoaded_) onLoaded_();
            return true;
        }

    private:
        factory_presets::PresetSession& session_;
        std::function<void()>           onLoaded_;
    };

    class PfClapEditorImpl final : public factory_ui_visage::FixedSizeVisageClapEditor
    {
    public:
        PfClapEditorImpl (pf_core::PfCore& core, factory_params::ParamStore& store,
                          factory_presets::PresetSession& session, const clap_host_t* host)
            : FixedSizeVisageClapEditor (host, store, pf_ui::PfEditor::kDesignW, pf_ui::PfEditor::kDesignH),
              theme_ (factory_ui_visage::Theme::defaults()),
              presets_ (session, [this] { onPresetLoaded(); })
        {
            feed_.detectedHz     = &core.uiDetectedHz;
            feed_.targetHz       = &core.uiTargetHz;
            feed_.shiftCents     = &core.uiShiftCents;
            feed_.latencySamples = &core.uiLatencySamples;
            feed_.sampleRateHz   = &core.uiSampleRateHz;
        }

        ~PfClapEditorImpl() override { destroy(); }

    protected:
        visage::Frame* buildEditor() override
        {
            editor_ = std::make_unique<pf_ui::PfEditor> (theme_, store_, feed_, presets_);
            updates_ = std::make_unique<factory_ui_visage::UpdateUiHost> (
                theme_, "tn-vocal-tuner", PF_CLAP_VERSION);
            editor_->addChild (&updates_->badge());
            editor_->addChild (&updates_->dialog()); // frontmost
            // Once per drawn frame (the status badge self-redraws): flush pending GUI edits to
            // the host while the plugin is inactive (harmless while active).
            editor_->setFrameTick ([this]
            {
                flushEditsIfInactive();
                layoutUpdates();
                if (updates_)
                    updates_->tick();
            });
            app_->addChild (*editor_);
            return editor_.get();
        }

        visage::Frame* editorFrame() const override { return editor_.get(); }
        void resetEditor() override
        {
            // Drop the editor first (children are non-owning), then the update chrome.
            editor_.reset();
            updates_.reset();
        }
        void onStateReplacedHook() override { if (editor_) editor_->onStateReplaced(); }
        void onEditorCreated() override { if (updates_) updates_->onShown(); }
        void onEditorDestroying() override { if (updates_) updates_->onHidden(); }

    private:
        void layoutUpdates()
        {
            if (editor_ == nullptr || updates_ == nullptr)
                return;
            const float w = editor_->width(), h = editor_->height();
            const float k = w / (float) pf_ui::PfEditor::kDesignW;
            updates_->layoutDialog (w, h);
            // Left of the preset selector (preset at design x=600).
            updates_->layoutBadge (520.0f * k, 40.0f * k, 72.0f * k, 24.0f * k);
        }

        // Bulk change (preset load): the host re-pulls values/text + marks dirty (no
        // per-parameter automation), then the editor resyncs to the replaced state.
        void onPresetLoaded()
        {
            notifyHostEdited();
            if (editor_) editor_->onStateReplaced();
        }

        factory_ui_visage::Theme theme_;    // owned; the editor holds a const ref
        pf_ui::PfUiFeed          feed_;
        SessionPresetModel       presets_;

        std::unique_ptr<pf_ui::PfEditor> editor_;
        std::unique_ptr<factory_ui_visage::UpdateUiHost> updates_;
    };
} // namespace

namespace pf_shell
{
    std::unique_ptr<factory_shell::IClapEditor>
    makePfClapEditor (pf_core::PfCore& core,
                      factory_params::ParamStore& store,
                      factory_presets::PresetSession& session,
                      const clap_host_t* host)
    {
        return std::make_unique<PfClapEditorImpl> (core, store, session, host);
    }
}
