//
// RsClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the RS CLAP
// plugin (chunk 3b). This is the SINGLE translation unit in the whole RS CLAP build
// that links Visage; it is compiled ONLY under FACTORY_RS_CLAP_GUI. It hosts the
// JUCE-free rs_ui::RsEditor (the full Phase-P3 port) inside a native
// visage::ApplicationWindow and implements the clap.gui (+ Linux posix-fd) surface
// the generic shell drives through IClapEditor.
//
// It is backed by the SAME live objects the CLAP shell owns:
//   * rs_core::RsCore            -> rs_ui::RsFeedFromCore (the real analyser feed:
//                                   zero-copy pointers into the core's published
//                                   lock-free spectra — identical hand-off to the
//                                   JUCE editor);
//   * factory_params::ParamStore -> every control binds by string id;
//   * factory_presets::PresetSession -> the REAL program list (Init + the six
//                                   factory presets) and apply path.
// A/B compare is a store-snapshot model (functional, but not yet the processor's
// setABSlot — see the header's tomorrow-list); preset apply relays a rescan +
// mark-dirty to the host so the change lands in the DAW.
//
// PARENT ATTACH (host-provided window):
//   * Linux  : clap_window.x11 is the X11 Window id; visage embeds via show(ptr)
//              and the host pumps the X11 connection fd through posix-fd-support.
//   * Windows: clap_window.win32 is the parent HWND; show(ptr) reparents (the
//              wrapper build declares CLAP_WINDOW_API_WIN32 — verified on Windows).
//   * macOS  : clap_window.cocoa is the parent NSView; show(ptr) adds the visage
//              view as a subview (declared CLAP_WINDOW_API_COCOA; mac uses the OS
//              run loop, not posix-fd). All three read the same clap_window union
//              pointer, matching visage's own ClapPlugin example.
//
// Threading: every method here is [main-thread] (the CLAP gui contract). The audio
// thread never touches this object; the only cross-thread data is the core's atomic
// spectra, read lock-free by the feed.
//
#include "RsClapEditor.h"

#include "RsEditor.h"
#include "RsFeedFromCore.h"
#include "RsTheme.h"
#include "RsModels.h"

#include "RsCore.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"

#include <visage/app.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // --- design geometry (parity with the JUCE editor) ------------------------
    // setSize(1069, 747); setResizeLimits(940, 657, 1320, 922); fixed aspect
    // 1069:747. Height is the layout driver (the editor scales by k()=height/747).
    constexpr int kDesignW = 1069, kDesignH = 747;
    constexpr int kMinW = 940, kMinH = 657, kMaxW = 1320, kMaxH = 922;

#if defined(_WIN32)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
    constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif

    // --- REAL preset model over the shell's PresetSession ---------------------
    // Lists Init + the six factory presets (real names) + a trailing "Save As…"
    // action row (parity with the P3 harness selector). load() applies the real
    // program values through the session, then fires onLoaded so the host re-pulls
    // values + marks state dirty. Returning true tells the editor a preset LOADED,
    // so it clears its undo timeline (JUCE apvts.replaceState() semantics).
    class SessionPresetModel final : public rs_ui::RsPresetModel
    {
    public:
        SessionPresetModel (factory_presets::PresetSession& session, std::function<void()> onLoaded)
            : session_ (session), onLoaded_ (std::move (onLoaded)) {}

        std::vector<rs_ui::RsPresetItem> items() const override
        {
            std::vector<rs_ui::RsPresetItem> v;
            const int n = session_.numPrograms();
            v.reserve (static_cast<std::size_t> (n) + 1);
            for (int i = 0; i < n; ++i)
                v.push_back ({ session_.programName (i), /*steppable*/ true, /*isAction*/ false });
            v.push_back ({ "Save As\xE2\x80\xA6", /*steppable*/ false, /*isAction*/ true });
            return v;
        }

        int currentIndex() const override { return session_.currentProgram(); }

        bool load (int index) override
        {
            if (index < 0 || index >= session_.numPrograms())
                return false; // the "Save As…" action row (or out of range)
            session_.applyProgram (index); // writes real values via ParamStore::setFromHost
            if (onLoaded_) onLoaded_();
            return true;
        }

    private:
        factory_presets::PresetSession& session_;
        std::function<void()>           onLoaded_;
    };

    // --- A/B compare (store-snapshot; functional mock — see header) -----------
    class StoreAbModel final : public rs_ui::RsAbModel
    {
    public:
        explicit StoreAbModel (factory_params::ParamStore& store, std::function<void()> onSwitched)
            : store_ (store), onSwitched_ (std::move (onSwitched)) {}

        int activeSlot() const override { return active_; }

        void setActiveSlot (int slot) override
        {
            if (slot == active_ || slot < 0 || slot > 1) return;
            slotState_[static_cast<std::size_t> (active_)] = snapshot();
            seeded_[static_cast<std::size_t> (active_)] = true;
            active_ = slot;
            if (seeded_[static_cast<std::size_t> (slot)])
                apply (slotState_[static_cast<std::size_t> (slot)]);
            else
            {
                slotState_[static_cast<std::size_t> (slot)] = snapshot();
                seeded_[static_cast<std::size_t> (slot)] = true;
            }
            if (onSwitched_) onSwitched_();
        }

        void copyActiveToOther() override
        {
            const int other = 1 - active_;
            slotState_[static_cast<std::size_t> (other)] = snapshot();
            seeded_[static_cast<std::size_t> (other)] = true;
        }

    private:
        std::vector<float> snapshot() const
        {
            std::vector<float> v (static_cast<std::size_t> (store_.size()));
            for (int i = 0; i < store_.size(); ++i) v[static_cast<std::size_t> (i)] = store_.value (i);
            return v;
        }
        void apply (const std::vector<float>& s)
        {
            for (int i = 0; i < store_.size() && i < static_cast<int> (s.size()); ++i)
                store_.setFromHost (i, s[static_cast<std::size_t> (i)]);
        }

        factory_params::ParamStore& store_;
        std::function<void()>       onSwitched_;
        int active_ = 0;
        std::array<std::vector<float>, 2> slotState_ {};
        std::array<bool, 2> seeded_ { { false, false } };
    };

    // --- the IClapEditor implementation ---------------------------------------
    class RsClapEditorImpl final : public factory_shell::IClapEditor
    {
    public:
        RsClapEditorImpl (rs_core::RsCore& core, factory_params::ParamStore& store,
                          factory_presets::PresetSession& session, const clap_host_t* host)
            : store_ (store),
              host_ (host),
              theme_ (rs_ui::RsTheme::defaults()),
              feed_ (core),
              presets_ (session, [this] { notifyHostEdited(); }),
              ab_ (store, [this] { notifyHostEdited(); })
        {
        }

        ~RsClapEditorImpl() override { destroy(); }

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
            editor_ = std::make_unique<rs_ui::RsEditor> (theme_, store_, feed_, presets_, ab_);

            app_->addChild (*editor_);
            app_->setNativeWindowDimensions (kDesignW, kDesignH);
            layoutEditor (kDesignW, kDesignH);
            app_->setMinimumDimensions (static_cast<float> (kMinW), static_cast<float> (kMinH));
            app_->setFixedAspectRatio (true); // captures the current 1069:747 ratio

            // Per analyser frame: drain the editor's gesture queue into its undo
            // timeline. The REAL feed refreshes itself from the audio thread, so
            // (unlike the synthetic harness feed) there is nothing to advance here.
            editor_->curve().onTick = [this] { if (editor_) editor_->pumpGestures(); };

            // Content-driven resize (e.g. a future DPI change) -> ask the host to
            // resize the host window to match, mirroring visage's ClapPlugin example.
            app_->onWindowContentsResized() = [this] {
                if (app_ == nullptr) return;
                if (hostGui_ == nullptr && host_ != nullptr)
                    hostGui_ = static_cast<const clap_host_gui_t*> (host_->get_extension (host_, CLAP_EXT_GUI));
                if (hostGui_ != nullptr && hostGui_->request_resize != nullptr)
                    hostGui_->request_resize (host_, static_cast<std::uint32_t> (app_->nativeWidth()),
                                              static_cast<std::uint32_t> (app_->nativeHeight()));
            };

            curW_ = kDesignW;
            curH_ = kDesignH;

            // The analyser is now live: let the core publish display spectra + run
            // display-time smoothing again (both skipped while no editor is attached
            // -- see RsCore::setDisplayActive). Cleared in destroy(). Main thread, as
            // the whole gui contract is; the core reads the flag on the audio thread.
            feed_.setDisplayActive (true);
            return true;
        }

        void destroy() noexcept override
        {
            // GUI going away: stop the core's display publish + smoothing (perf).
            feed_.setDisplayActive (false);
            if (app_) app_->close();
            editor_.reset();
            app_.reset();
        }

        bool setScale (double /*scale*/) noexcept override
        {
            // Visage works the scaling factor out from the OS itself, so (like the
            // JUCE editor, which never implemented host scaling) we decline and let
            // the host fall back to OS DPI.
            return false;
        }

        bool getSize (std::uint32_t* width, std::uint32_t* height) noexcept override
        {
            if (width == nullptr || height == nullptr) return false;
            *width  = curW_;
            *height = curH_;
            return true;
        }

        bool canResize() const noexcept override { return true; }

        bool getResizeHints (clap_gui_resize_hints_t* hints) noexcept override
        {
            if (hints == nullptr) return false;
            hints->can_resize_horizontally = true;
            hints->can_resize_vertically   = true;
            hints->preserve_aspect_ratio   = true;
            hints->aspect_ratio_width      = static_cast<std::uint32_t> (kDesignW);
            hints->aspect_ratio_height     = static_cast<std::uint32_t> (kDesignH);
            return true;
        }

        bool adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept override
        {
            if (width == nullptr || height == nullptr) return false;
            // Snap to the 1069:747 aspect (height-driven) and clamp to the JUCE
            // editor's resize limits [940x657 .. 1320x922].
            double h = static_cast<double> (*height);
            h = clampd (h, kMinH, kMaxH);
            double w = h * static_cast<double> (kDesignW) / static_cast<double> (kDesignH);
            if (w < kMinW) { w = kMinW; h = w * static_cast<double> (kDesignH) / static_cast<double> (kDesignW); }
            if (w > kMaxW) { w = kMaxW; h = w * static_cast<double> (kDesignH) / static_cast<double> (kDesignW); }
            *width  = static_cast<std::uint32_t> (std::lround (w));
            *height = static_cast<std::uint32_t> (std::lround (h));
            return true;
        }

        bool setSize (std::uint32_t width, std::uint32_t height) noexcept override
        {
            if (! app_) return false;
            app_->setNativeWindowDimensions (static_cast<int> (width), static_cast<int> (height));
            layoutEditor (static_cast<int> (width), static_cast<int> (height));
            curW_ = width;
            curH_ = height;
            return true;
        }

        bool setParent (const clap_window_t* window) noexcept override
        {
            if (! app_ || window == nullptr) return false;
            fetchHostExts();
            // X11 window id / HWND / NSView all live in the same union pointer, read
            // exactly as visage's own ClapPlugin example does.
            app_->show (window->ptr);
            return true;
        }

        bool show() noexcept override { if (editor_) editor_->setVisible (true);  return true; }
        bool hide() noexcept override { if (editor_) editor_->setVisible (false); return true; }

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
        static double clampd (double v, double lo, double hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        void layoutEditor (int w, int h)
        {
            if (editor_)
                editor_->setBounds (0.0f, 0.0f, static_cast<float> (w), static_cast<float> (h));
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

        // A GUI-driven bulk parameter change (preset load / A-B switch) went into the
        // store via setFromHost. Tell the host to re-pull the values + text and flag
        // the state dirty, so the DAW reflects it (full per-param automation output
        // is the tomorrow-list "preset real wiring" item).
        void notifyHostEdited()
        {
            fetchHostExts();
            if (hostParams_ != nullptr && hostParams_->rescan != nullptr)
                hostParams_->rescan (host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
            if (hostState_ != nullptr && hostState_->mark_dirty != nullptr)
                hostState_->mark_dirty (host_);
        }

        factory_params::ParamStore& store_;
        const clap_host_t*                  host_       = nullptr;
        const clap_host_gui_t*              hostGui_    = nullptr;
        const clap_host_params_t*           hostParams_ = nullptr;
        const clap_host_state_t*            hostState_  = nullptr;

        rs_ui::RsTheme         theme_;   // owned; the editor holds a const ref
        rs_ui::RsFeedFromCore  feed_;    // real feed over the shell's RsCore
        SessionPresetModel     presets_; // real, over PresetSession
        StoreAbModel           ab_;      // store-snapshot A/B

        std::unique_ptr<visage::ApplicationWindow> app_;
        std::unique_ptr<rs_ui::RsEditor>           editor_;
        std::uint32_t curW_ = static_cast<std::uint32_t> (kDesignW);
        std::uint32_t curH_ = static_cast<std::uint32_t> (kDesignH);
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
