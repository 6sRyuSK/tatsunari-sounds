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
// A/B compare is the real rs_ui::AbCompareModel (RsAbState.h): two session slots of
// {full parameter state + program index}, the exact JUCE-2.1.0 setABSlot /
// copyActiveToOther scope. A bulk state change (preset apply / A-B switch) relays a
// host rescan + mark-dirty so the DAW reflects it WITHOUT recording per-parameter
// automation; individual knob edits DO reach the host as automation, via the shell's
// GUI-edit -> CLAP output-event relay (ClapShellPlugin::emitHostWrites).
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
#include "RsAbState.h"

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
    // --- design geometry ------------------------------------------------------
    // Reference layout 1069x747, resize limits 471x329..1320x922, fixed aspect
    // 1069:747. Height is the layout driver (the editor scales by k()=height/747).
    // The window OPENS at kDefault = 706x493 (75% of the old 940x657 default): the
    // reference 1069x747 plus host chrome overflowed common laptop displays (the
    // "default window too big" report), and the old default doubled as the minimum
    // so the user could not shrink it further. The new default is comfortably below
    // the reference, and the minimum drops to 471x329 (50% of the old default) so the
    // whole UI — uniform-scaled by k() — can be made much smaller on demand.
    constexpr int kDesignW = 1069, kDesignH = 747;
    // Minimum window (used for setMinimumDimensions); the max + aspect snap live in
    // rs_shell::snapEditorSizeForScale (RsClapEditor.h).
    constexpr int kMinW = 471, kMinH = 329;
    constexpr int kDefaultW = 706, kDefaultH = 493;

#if defined(_WIN32)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
    constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif

    // --- REAL preset model over the shell's PresetSession ---------------------
    // Lists Init + the six factory presets (real names). load() applies the real
    // program values through the session, then fires onLoaded so the host re-pulls
    // values + marks state dirty. Returning true tells the editor a preset LOADED,
    // so it clears its undo timeline (JUCE apvts.replaceState() semantics).
    //
    // No trailing "Save As…" action row: until a user-preset store (UserPresetStoreFs)
    // is wired into the CLAP shell there is nothing for it to do, so we do not surface
    // a dead menu entry. (The tools/ui-dev harness keeps its mock action row to
    // exercise the selector's action-row skipping, and is intentionally left as is.)
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
                return false; // out of range
            session_.applyProgram (index); // writes real values via ParamStore::setFromHost
            if (onLoaded_) onLoaded_();
            return true;
        }

    private:
        factory_presets::PresetSession& session_;
        std::function<void()>           onLoaded_;
    };

    // A/B compare is the real, JUCE-2.1.0-faithful rs_ui::AbCompareModel (RsAbState.h):
    // two session-only slots of {full parameter state + program index}, with the
    // shell's PresetSession backing the program hooks (see ctor). Its onSwitched
    // callback relays a host rescan + mark-dirty so the DAW reflects the switch.

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
              ab_ (store,
                   [&session] { return session.currentProgram(); },
                   [&session] (int i) { session.setCurrentProgramClean (i); },
                   [this] { notifyHostEdited(); })
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

            // The editor's corner grip already proposes a WINDOW size (its drag runs
            // in the fixed 1069x747 design plane and is converted to window units via
            // the shell-synced windowScale_ — see RsEditor::setWindowScale), snapped to
            // the aspect + limits. So we relay it to the host verbatim (host units ==
            // window units: logical points on macOS, physical px elsewhere). This is the
            // ONLY resize path a host with no window resize edge (Logic's AU) offers.
            editor_->onResizeRequest = [this] (float w, float h)
            {
                fetchHostExts();
                if (hostGui_ == nullptr || hostGui_->request_resize == nullptr) return;
                hostGui_->request_resize (host_, static_cast<std::uint32_t> (std::lround (w)),
                                          static_cast<std::uint32_t> (std::lround (h)));
            };

            app_->addChild (*editor_);
            setPluginDimensions (kDefaultW, kDefaultH);
            app_->setMinimumDimensions (static_cast<float> (kMinW), static_cast<float> (kMinH));
            syncWindowScale();               // dpi = physH/747, editor -> design bounds
            app_->setFixedAspectRatio (true); // captures the current 1069:747 ratio

            // Per analyser frame:
            //  * drain the editor's gesture queue into its undo timeline. The REAL feed
            //    refreshes itself from the audio thread, so (unlike the synthetic
            //    harness feed) there is nothing to advance here; and
            //  * while the plugin is INACTIVE there is no process()/params.flush() to
            //    drain the store's host-write queue, so a GUI knob edit made with the
            //    transport stopped would never reach the host. Ask the host to flush
            //    the params when edits are pending — the CLAP contract's inactive-edit
            //    path. While active this is harmless (the next process() drains the
            //    queue first, so the flush finds nothing). [main-thread].
            editor_->curve().onTick = [this] {
                if (editor_ == nullptr) return;
                editor_->pumpGestures();
                if (store_.hasPendingHostWrites())
                {
                    fetchHostExts();
                    if (hostParams_ != nullptr && hostParams_->request_flush != nullptr)
                        hostParams_->request_flush (host_);
                }
            };

            // Undo / redo applies a bulk parameter change (the whole snapshot), so
            // relay it to the host exactly like a preset / A-B switch: rescan(VALUES) +
            // mark-dirty, WITHOUT punching an automation point per parameter.
            editor_->onHistoryApplied = [this] { notifyHostEdited(); };

            // Content-driven resize (an OS DPI move, or the cascade our own writes set
            // off). Re-pin the editor to the fixed 1069x747 design plane via the
            // IDEMPOTENT syncWindowScale (writing the SAME size writes nothing), then
            // relay to the host ONLY when the window's host-facing size actually changed
            // and we are not already mid-setSize (shouldRelayHostResize).
            //
            // Relaying unconditionally here is exactly what closed the Logic AU stack-
            // overflow loop: request_resize -> host [NSView setFrame:] -> setSize ->
            // (re)resize -> windowContentsResized -> back here. Because syncWindowScale
            // sets the app frame's NATIVE bounds to the LIVE window client size, the
            // follow-up notifyContentsResized -> setInternalWindowSize sees no change and
            // early-returns, so this callback does not synchronously re-drive itself; and
            // the guarded relay never re-enters the host. The pair is a fixed point.
            app_->onWindowContentsResized() = [this] {
                if (app_ == nullptr) return;
                syncWindowScale();
                const std::uint32_t hw = static_cast<std::uint32_t> (hostFacingWidth());
                const std::uint32_t hh = static_cast<std::uint32_t> (hostFacingHeight());
                if (! rs_shell::shouldRelayHostResize (curW_, curH_, hw, hh, inSetSize_))
                    return;
                curW_ = hw;
                curH_ = hh;
                fetchHostExts();
                if (hostGui_ != nullptr && hostGui_->request_resize != nullptr)
                    hostGui_->request_resize (host_, hw, hh);
            };

            curW_ = kDefaultW;
            curH_ = kDefaultH;

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
            // Host-facing size = WINDOW size (logical points on macOS, physical px
            // elsewhere) — NOT the editor's logical bounds, which are now pinned at the
            // 1069x747 design regardless of window size. On non-mac the live physical
            // window is authoritative and moves under a DPI change; on macOS the logical
            // point size is fixed by the host and tracked in the cache (app_->width() is
            // now the design width, so it can't report the host size there).
            *width  = static_cast<std::uint32_t> (hostFacingWidth());
            *height = static_cast<std::uint32_t> (hostFacingHeight());
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
            // Snap to the 1069:747 aspect + the resize limits. The proposal AND the
            // limits (471x329..1320x922) are both in WINDOW units — physical px on
            // Windows/X11, logical points on macOS — so no logical<->native conversion
            // is needed here (the editor's own uniform zoom absorbs the OS DPI factor):
            // snap directly in window units (scale 1.0). The pure snapEditorSizeForScale
            // is still unit-tested across scales (clap_shell_test).
            rs_shell::snapEditorSizeForScale (1.0, *width, *height);
            return true;
        }

        bool setSize (std::uint32_t width, std::uint32_t height) noexcept override
        {
            if (! app_) return false;
            // IDEMPOTENT early-return: Logic re-sends the current size repeatedly during
            // its resize handshake. Re-entering the resize machinery for a size we are
            // already at is one arm of the stack-overflow loop (setSize -> resize ->
            // windowContentsResized -> request_resize -> host setFrame -> setSize), so a
            // no-op change must do nothing at all.
            if (width == curW_ && height == curH_) return true;

            // ORIGIN GUARD: the host is the source of this size, so while we apply it the
            // onWindowContentsResized handler must NOT echo it back via request_resize
            // (shouldRelayHostResize returns false when inSetSize_). This closes the
            // request_resize -> setFrame -> setSize edge of the loop.
            inSetSize_ = true;
            // Host-driven resize (window units). Set the native/logical window size per
            // platform, cache it as the host-facing size, then re-assert the uniform
            // zoom (dpi = physicalHeight/747) so the editor stays the 1069x747 design.
            setPluginDimensions (static_cast<int> (width), static_cast<int> (height));
            curW_ = width;
            curH_ = height;
            syncWindowScale();
            inSetSize_ = false;
            return true;
        }

        bool setParent (const clap_window_t* window) noexcept override
        {
            if (! app_ || window == nullptr) return false;
            fetchHostExts();
            // X11 window id / HWND / NSView all live in the same union pointer, read
            // exactly as visage's own ClapPlugin example does.
            app_->show (window->ptr);
            // Attaching creates the native window; addToWindow() seeds the frame tree
            // from the OS DPI factor. Re-assert our uniform zoom so the editor opens at
            // the design layout scaled to the window, not the OS-DPI logical size.
            syncWindowScale();
            return true;
        }

        bool show() noexcept override { if (editor_) editor_->setVisible (true);  return true; }
        bool hide() noexcept override { if (editor_) editor_->setVisible (false); return true; }

        // The shell finished a host state.load(): the restored values are in the store
        // already, so resync the editor to the replaced state (clear + re-seed undo,
        // rebuild the preset selector, redraw) — the JUCE setStateInformation ->
        // replaceState follow-through. No-op when the GUI was never created.
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
        // CLAP/VST3 GUI sizes are LOGICAL points on macOS but PHYSICAL pixels on
        // Windows/X11, so the visage window must be sized/read through the matching
        // API per platform (upstream ClapPlugin example's #if __APPLE__ split). Using
        // the native calls on a Retina mac halves the window (the Cubase 15 on macOS
        // bug), so macOS keeps the logical-point API.
        void setPluginDimensions (int width, int height)
        {
            if (! app_) return;
#if __APPLE__
            app_->setWindowDimensions (width, height);
#else
            app_->setNativeWindowDimensions (width, height);
#endif
        }

        // The HOST-FACING window size (what getSize / request_resize report). Window
        // units: logical points on macOS, physical px on Windows/X11. On non-mac the
        // live physical window (window()->clientWidth/Height) is authoritative and
        // moves under an OS DPI change; on macOS app_->width() is now the pinned design
        // width, so the host-set logical point size is read from the cache instead.
        int hostFacingWidth() const
        {
            if (! app_) return static_cast<int> (curW_);
#if __APPLE__
            return static_cast<int> (curW_);
#else
            visage::Window* w = app_->window();
            return (w && w->clientWidth() > 0) ? w->clientWidth() : static_cast<int> (curW_);
#endif
        }

        int hostFacingHeight() const
        {
            if (! app_) return static_cast<int> (curH_);
#if __APPLE__
            return static_cast<int> (curH_);
#else
            visage::Window* w = app_->window();
            return (w && w->clientHeight() > 0) ? w->clientHeight() : static_cast<int> (curH_);
#endif
        }

        // Re-assert the UNIFORM ZOOM: set the window's dpi_scale to physicalHeight/747
        // and pin the editor to the fixed 1069x747 design plane. Because visage routes
        // every logical<->native path (frame bounds = native/dpi, canvas dpi, mouse
        // convertToLogical, font rasterization) through the window dpi_scale, this makes
        // the whole editor render at the design layout uniformly scaled to the window —
        // no per-element scaling, and the OS Retina/HiDPI factor is composited in
        // automatically because we derive from the physical height.
        //
        // FIXED-POINT INVARIANT (this is what keeps the AU resize chain from recursing):
        // the app frame's native bounds are set to the LIVE window client size
        // (physW,physH). So the cascade this triggers — Frame::setBounds ->
        // TopLevelFrame::resized -> ApplicationEditor::notifyContentsResized ->
        // Window::setInternalWindowSize(nativeWidth, nativeHeight) — feeds
        // setInternalWindowSize a size equal to the current client size, which makes it
        // early-return WITHOUT calling windowContentsResized again. Every writable sink
        // here is idempotent: Frame::setBounds / Frame::setDpiScale early-return on an
        // unchanged value, and the one sink that does NOT (Window::setDpiScale is a plain
        // assignment) is guarded by an explicit compare. Therefore running this with an
        // unchanged window size writes nothing and starts no further callbacks — the
        // property the Logic-loop fix depends on. `inScaleSync_` additionally blocks
        // synchronous self-recursion within a single pass.
        void syncWindowScale()
        {
            if (inScaleSync_ || app_ == nullptr || editor_ == nullptr) return;
            visage::Window* win = app_->window();
            // Physical pixel size of the window. clientWidth/Height() are native px and
            // track the real drawable; before the window is attached, the app frame's
            // native size carries the pre-set default.
            const int physH = win ? win->clientHeight() : app_->nativeHeight();
            const int physW = win ? win->clientWidth()  : app_->nativeWidth();
            if (physH <= 0 || physW <= 0) return;
            const float dpi = static_cast<float> (physH) / static_cast<float> (kDesignH);

            inScaleSync_ = true;
            // Window::setDpiScale is a bare assignment (no unchanged-value guard), so
            // only write it when it actually moves — otherwise a same-size re-entry would
            // dirty nothing yet still churn.
            if (win && win->dpiScale() != dpi) win->setDpiScale (dpi);
            // Drive the frame tree from the corrected dpi: app_ (and its RsEditor child)
            // take the new dpi, the app's native (== physical client) maps back to logical
            // 1069x747, the canvas follows, and the editor fills it at the design size.
            app_->setDpiScale (dpi);
            app_->setNativeBounds (0, 0, physW, physH);
            if (win) app_->setCanvasDetails();
            editor_->setBounds (0.0f, 0.0f, static_cast<float> (kDesignW), static_cast<float> (kDesignH));
            // Tell the editor the current zoom so the resize grip converts its design-
            // space drag into window units.
            editor_->setWindowScale (static_cast<float> (hostFacingWidth()) / static_cast<float> (kDesignW));
            inScaleSync_ = false;
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

        // A GUI-driven BULK parameter change (preset load / A-B switch) went into the
        // store via setFromHost. Tell the host to re-pull the values + text and flag
        // the state dirty, so the DAW reflects it. This is the correct CLAP idiom for
        // a bulk change: rescan(VALUES) updates the host WITHOUT recording an
        // automation point per parameter (which an A/B flip must not do). Individual
        // knob edits take the other path — setFromUi -> the shell relays them to the
        // host as CLAP param/gesture output events (recorded automation).
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
        rs_ui::AbCompareModel  ab_;      // real A/B: params + program index (RsAbState.h)

        std::unique_ptr<visage::ApplicationWindow> app_;
        std::unique_ptr<rs_ui::RsEditor>           editor_;
        std::uint32_t curW_ = static_cast<std::uint32_t> (kDefaultW);
        std::uint32_t curH_ = static_cast<std::uint32_t> (kDefaultH);
        bool          inScaleSync_ = false; // synchronous re-entrancy guard for syncWindowScale()
        bool          inSetSize_   = false; // origin guard: suppress request_resize echo during a host setSize
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
