//
// DeqClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the dynamic-eq
// CLAP plugin. Derives the shared factory_ui_visage::ResizableVisageClapEditor (which owns
// the CLAP↔Visage host boilerplate + the uniform-zoom / aspect-lock / Logic-AU resize
// machinery), so this file keeps ONLY the Deq-specific pieces: constructing deq_ui::
// DeqEditor over the shell's live core/store/session, the real preset model, and the
// analyser feed. Compiled ONLY under FACTORY_DEQ_CLAP_GUI (the single visage-linking TU
// of the dynamic-eq CLAP build).
//
#include "DeqClapEditor.h"

#include "ui/DeqEditor.h"
#include "ui/DeqModels.h"
#include "ui/DeqFeedFromCore.h"

#include "DeqCore.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"
#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ClapEditorHost.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Design geometry (WINDOW units): reference 740x520, resize limits 620x440..1280x900
    // (the JUCE editor's setResizeLimits + 740:520 aspect). Opens at the design size.
    constexpr factory_shell::EditorGeometry kDeqGeometry {
        /*designW*/ 740.0, /*designH*/ 520.0, /*minW*/ 620.0, /*minH*/ 440.0,
        /*maxW*/ 1280.0, /*maxH*/ 900.0
    };
    constexpr int kDefaultW = 740, kDefaultH = 520;

    // REAL preset model over the shell's PresetSession.
    class SessionPresetModel final : public deq_ui::DeqPresetModel
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

    class DeqClapEditorImpl final : public factory_ui_visage::ResizableVisageClapEditor
    {
    public:
        DeqClapEditorImpl (deq_core::DeqCore& core, factory_params::ParamStore& store,
                           factory_presets::PresetSession& session, const clap_host_t* host)
            : ResizableVisageClapEditor (host, store, kDeqGeometry, kDefaultW, kDefaultH),
              theme_ (factory_ui_visage::Theme::defaults()),
              feed_ (core),
              presets_ (session, [this] { onPresetLoaded(); })
        {
        }

        ~DeqClapEditorImpl() override { destroy(); }

    protected:
        visage::Frame* buildEditor() override
        {
            editor_ = std::make_unique<deq_ui::DeqEditor> (theme_, store_, feed_, presets_);
            editor_->onResizeRequest = [this] (float w, float h) { requestResizeFromEditor (w, h); };
            editor_->setFrameTick ([this] { flushEditsIfInactive(); });
            app_->addChild (*editor_);
            return editor_.get();
        }

        visage::Frame* editorFrame() const override { return editor_.get(); }
        void resetEditor() override { editor_.reset(); }
        void setEditorWindowScale (float windowScale) override { if (editor_) editor_->setWindowScale (windowScale); }
        void onStateReplacedHook() override { if (editor_) editor_->onStateReplaced(); }

    private:
        // Bulk change (preset load): host re-pulls values/text + marks dirty (no
        // per-parameter automation), then the editor resyncs to the replaced state.
        void onPresetLoaded()
        {
            notifyHostEdited();
            if (editor_) editor_->onStateReplaced();
        }

        factory_ui_visage::Theme theme_;   // owned; the editor holds a const ref
        deq_ui::DeqFeedFromCore  feed_;     // real feed over the shell's DeqCore
        SessionPresetModel       presets_;

        std::unique_ptr<deq_ui::DeqEditor> editor_;
    };
} // namespace

namespace deq_shell
{
    std::unique_ptr<factory_shell::IClapEditor>
    makeDeqClapEditor (deq_core::DeqCore& core,
                       factory_params::ParamStore& store,
                       factory_presets::PresetSession& session,
                       const clap_host_t* host)
    {
        return std::make_unique<DeqClapEditorImpl> (core, store, session, host);
    }
}
