//
// PfClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the
// pitch-fix CLAP plugin. This is the SINGLE translation unit in the pitch-fix
// CLAP build that links Visage; it is compiled ONLY under FACTORY_PF_CLAP_GUI.
// It hosts the JUCE-free pf_ui::PfEditor inside a native
// visage::ApplicationWindow and implements the clap.gui (+ Linux posix-fd)
// surface the generic shell drives through IClapEditor.
//
// Backed by the SAME live objects the CLAP shell owns:
//   * pf_core::PfCore            -> the ui* atomics feed the latency/pitch badge;
//   * factory_params::ParamStore -> every control binds by string id;
//   * factory_presets::PresetSession -> the real program list (Init + the ten
//                                   factory presets) and apply path.
//
// FIXED-SIZE editor (920×560 logical) — the DSP-phase UI has no resize surface.
// PARENT ATTACH and the macOS-logical/other-native size split mirror the RS
// editor host (the proven pattern; see RsClapEditor.cpp for the full rationale).
//
// Threading: every method here is [main-thread] (the CLAP gui contract). The
// audio thread never touches this object; the only cross-thread data is the
// core's atomics, read lock-free by the badge.
//
#include "PfClapEditor.h"

#include "PfEditor.h"
#include "PfModels.h"

#include "PfCore.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"
#include "factory_ui_visage/Theme.h"

#include <visage/app.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr int kDesignW = pf_ui::PfEditor::kDesignW;   // 920
    constexpr int kDesignH = pf_ui::PfEditor::kDesignH;   // 560

#if defined(_WIN32)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
    constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif

    // REAL preset model over the shell's PresetSession: lists Init + the bank,
    // applies through the session, then notifies the host (rescan + mark-dirty —
    // a bulk change records no per-parameter automation).
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

    class PfClapEditorImpl final : public factory_shell::IClapEditor
    {
    public:
        PfClapEditorImpl (pf_core::PfCore& core, factory_params::ParamStore& store,
                          factory_presets::PresetSession& session, const clap_host_t* host)
            : store_ (store),
              host_ (host),
              theme_ (factory_ui_visage::Theme::defaults()),
              presets_ (session, [this] { notifyHostEdited(); })
        {
            feed_.detectedHz     = &core.uiDetectedHz;
            feed_.targetHz       = &core.uiTargetHz;
            feed_.shiftCents     = &core.uiShiftCents;
            feed_.latencySamples = &core.uiLatencySamples;
            feed_.sampleRateHz   = &core.uiSampleRateHz;
        }

        ~PfClapEditorImpl() override { destroy(); }

        bool isApiSupported (const char* api, bool isFloating) const noexcept override
        {
            if (isFloating) return false;
            return api != nullptr && std::strcmp (api, kNativeApi) == 0;
        }

        bool getPreferredApi (const char** api, bool* isFloating) const noexcept override
        {
            if (api == nullptr || isFloating == nullptr) return false;
            *api = kNativeApi;
            *isFloating = false;
            return true;
        }

        bool create (const char* /*api*/, bool isFloating) noexcept override
        {
            if (isFloating) return false;
            if (app_) return true;

            app_    = std::make_unique<visage::ApplicationWindow>();
            editor_ = std::make_unique<pf_ui::PfEditor> (theme_, store_, feed_, presets_);

            app_->addChild (*editor_);
            setPluginDimensions (kDesignW, kDesignH);
            layoutEditorToWindow();

            // Once per drawn frame (the status badge self-redraws): while the
            // plugin is INACTIVE there is no process()/params.flush() to drain
            // the store's host-write queue, so a GUI knob edit made with the
            // transport stopped would never reach the host — ask the host to
            // flush when edits are pending. Harmless while active. [main-thread]
            editor_->setFrameTick ([this]
            {
                if (store_.hasPendingHostWrites())
                {
                    fetchHostExts();
                    if (hostParams_ != nullptr && hostParams_->request_flush != nullptr)
                        hostParams_->request_flush (host_);
                }
            });

            // Content-driven resize (e.g. a DPI change once attached): re-flow the
            // editor and tell the host the (fixed) size in its units.
            app_->onWindowContentsResized() = [this]
            {
                if (app_ == nullptr) return;
                layoutEditorToWindow();
                fetchHostExts();
                if (hostGui_ != nullptr && hostGui_->request_resize != nullptr)
                    hostGui_->request_resize (host_, (std::uint32_t) pluginWidth(),
                                              (std::uint32_t) pluginHeight());
            };

            curW_ = (std::uint32_t) kDesignW;
            curH_ = (std::uint32_t) kDesignH;
            return true;
        }

        void destroy() noexcept override
        {
            if (app_) app_->close();
            editor_.reset();
            app_.reset();
        }

        bool setScale (double /*scale*/) noexcept override
        {
            return false;   // visage resolves OS DPI itself (same as RS)
        }

        bool getSize (std::uint32_t* width, std::uint32_t* height) noexcept override
        {
            if (width == nullptr || height == nullptr) return false;
            if (app_ != nullptr && pluginWidth() > 0 && pluginHeight() > 0)
            {
                *width  = (std::uint32_t) pluginWidth();
                *height = (std::uint32_t) pluginHeight();
                return true;
            }
            *width  = curW_;
            *height = curH_;
            return true;
        }

        bool canResize() const noexcept override { return false; }

        bool getResizeHints (clap_gui_resize_hints_t* hints) noexcept override
        {
            if (hints == nullptr) return false;
            hints->can_resize_horizontally = false;
            hints->can_resize_vertically   = false;
            hints->preserve_aspect_ratio   = true;
            hints->aspect_ratio_width      = (std::uint32_t) kDesignW;
            hints->aspect_ratio_height     = (std::uint32_t) kDesignH;
            return true;
        }

        bool adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept override
        {
            if (width == nullptr || height == nullptr) return false;
            // Fixed-size surface: every proposal snaps to the design size in the
            // host's units (logical on macOS, native px elsewhere).
            const double s = dpiScale();
            *width  = (std::uint32_t) std::lround (kDesignW * s);
            *height = (std::uint32_t) std::lround (kDesignH * s);
            return true;
        }

        bool setSize (std::uint32_t width, std::uint32_t height) noexcept override
        {
            if (! app_) return false;
            setPluginDimensions ((int) width, (int) height);
            layoutEditorToWindow();
            curW_ = width;
            curH_ = height;
            return true;
        }

        bool setParent (const clap_window_t* window) noexcept override
        {
            if (! app_ || window == nullptr) return false;
            fetchHostExts();
            app_->show (window->ptr);   // X11 id / HWND / NSView share the union ptr
            return true;
        }

        bool show() noexcept override { if (editor_) editor_->setVisible (true);  return true; }
        bool hide() noexcept override { if (editor_) editor_->setVisible (false); return true; }

        void onHostStateRestored() noexcept override
        {
            if (editor_) editor_->onStateReplaced();
        }

        int posixFd() const noexcept override
        {
#ifdef __linux__
            return (app_ && app_->window()) ? app_->window()->posixFd() : -1;
#else
            return -1;
#endif
        }

        void onPosixFd (clap_posix_fd_flags_t /*flags*/) noexcept override
        {
#ifdef __linux__
            if (app_ && app_->window())
                app_->window()->processPluginFdEvents();
#endif
        }

    private:
        double dpiScale() const
        {
#if __APPLE__
            return 1.0;
#else
            return (app_ && app_->width() > 0)
                     ? (double) app_->nativeWidth() / (double) app_->width()
                     : 1.0;
#endif
        }

        // CLAP/VST3 GUI sizes are LOGICAL points on macOS but PHYSICAL pixels on
        // Windows/X11 (see RsClapEditor.cpp for the Retina bug this split fixes).
        void setPluginDimensions (int width, int height)
        {
            if (! app_) return;
#if __APPLE__
            app_->setWindowDimensions (width, height);
#else
            app_->setNativeWindowDimensions (width, height);
#endif
        }

        int pluginWidth() const
        {
            if (! app_) return 0;
#if __APPLE__
            return (int) app_->width();
#else
            return app_->nativeWidth();
#endif
        }

        int pluginHeight() const
        {
            if (! app_) return 0;
#if __APPLE__
            return (int) app_->height();
#else
            return app_->nativeHeight();
#endif
        }

        void layoutEditorToWindow()
        {
            if (editor_ && app_)
                editor_->setBounds (0.0f, 0.0f,
                                    (float) app_->width(), (float) app_->height());
        }

        void fetchHostExts()
        {
            if (host_ == nullptr) return;
            if (hostGui_ == nullptr)
                hostGui_ = static_cast<const clap_host_gui_t*> (host_->get_extension (host_, CLAP_EXT_GUI));
            if (hostParams_ == nullptr)
                hostParams_ = static_cast<const clap_host_params_t*> (host_->get_extension (host_, CLAP_EXT_PARAMS));
            if (hostState_ == nullptr)
                hostState_ = static_cast<const clap_host_state_t*> (host_->get_extension (host_, CLAP_EXT_STATE));
        }

        // Bulk change (preset load): host re-pulls values/text + marks dirty,
        // without punching per-parameter automation (the CLAP bulk idiom).
        void notifyHostEdited()
        {
            fetchHostExts();
            if (hostParams_ != nullptr && hostParams_->rescan != nullptr)
                hostParams_->rescan (host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
            if (hostState_ != nullptr && hostState_->mark_dirty != nullptr)
                hostState_->mark_dirty (host_);
            if (editor_)
                editor_->onStateReplaced();
        }

        factory_params::ParamStore& store_;
        const clap_host_t*          host_       = nullptr;
        const clap_host_gui_t*      hostGui_    = nullptr;
        const clap_host_params_t*   hostParams_ = nullptr;
        const clap_host_state_t*    hostState_  = nullptr;

        factory_ui_visage::Theme theme_;    // owned; the editor holds a const ref
        pf_ui::PfUiFeed          feed_;
        SessionPresetModel       presets_;

        std::unique_ptr<visage::ApplicationWindow> app_;
        std::unique_ptr<pf_ui::PfEditor>           editor_;
        std::uint32_t curW_ = (std::uint32_t) kDesignW;
        std::uint32_t curH_ = (std::uint32_t) kDesignH;
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
