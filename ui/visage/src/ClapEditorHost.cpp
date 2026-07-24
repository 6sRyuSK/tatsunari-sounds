//
// ClapEditorHost.cpp — the Visage-backed IClapEditor host base implementations (see
// ClapEditorHost.h). The resizable layer is EXACT code motion of RsClapEditor.cpp's
// syncWindowScale / dynamicMaxWindowUnits / setSize loop-fix; the fixed layer is
// pitch-fix's layoutEditorToWindow path. The single visage-linked home of the shared
// host logic — a plugin's concrete editor derives it and links this via
// factory_ui_visage_clap_host.
//
#include "factory_ui_visage/ClapEditorHost.h"

#include <cmath>
#include <cstring>

namespace factory_ui_visage
{
    namespace
    {
        // CLAP/VST3 GUI sizes are LOGICAL points on macOS but PHYSICAL pixels on
        // Windows/X11, so the visage window is sized/read through the matching API per
        // platform (upstream ClapPlugin example's #if __APPLE__ split).
#if defined(_WIN32)
        constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
        constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
        constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif
    }

    // ═══════════════════════════ VisageClapEditorHost ═══════════════════════════

    bool VisageClapEditorHost::isApiSupported (const char* api, bool isFloating) const noexcept
    {
        if (isFloating) return false;
        return api != nullptr && std::strcmp (api, kNativeApi) == 0;
    }

    bool VisageClapEditorHost::getPreferredApi (const char** api, bool* isFloating) const noexcept
    {
        if (api == nullptr || isFloating == nullptr) return false;
        *api = kNativeApi;
        *isFloating = false;
        return true;
    }

    bool VisageClapEditorHost::create (const char* /*api*/, bool isFloating) noexcept
    {
        if (isFloating) return false;
        if (app_) return true;

        app_ = std::make_unique<visage::ApplicationWindow>();
        if (buildEditor() == nullptr) { app_.reset(); return false; }

        configureWindowOnCreate();
        onEditorCreated();
        return true;
    }

    void VisageClapEditorHost::destroy() noexcept
    {
        onEditorDestroying();
        if (app_) app_->close();
        resetEditor();
        app_.reset();
    }

    bool VisageClapEditorHost::setParent (const clap_window_t* window) noexcept
    {
        if (! app_ || window == nullptr) return false;
        fetchHostExts();
        // X11 window id / HWND / NSView all live in the same union pointer, read exactly
        // as visage's own ClapPlugin example does.
        app_->show (window->ptr);
        afterParentAttached();
        return true;
    }

    bool VisageClapEditorHost::show() noexcept { if (auto* e = editorFrame()) e->setVisible (true);  return true; }
    bool VisageClapEditorHost::hide() noexcept { if (auto* e = editorFrame()) e->setVisible (false); return true; }

    void VisageClapEditorHost::onHostStateRestored() noexcept { onStateReplacedHook(); }

    int VisageClapEditorHost::posixFd() const noexcept
    {
#ifdef __linux__
        return (app_ && app_->window()) ? app_->window()->posixFd() : -1;
#else
        return -1;
#endif
    }

    void VisageClapEditorHost::onPosixFd (clap_posix_fd_flags_t /*flags*/) noexcept
    {
#ifdef __linux__
        if (app_ && app_->window())
            app_->window()->processPluginFdEvents();
#endif
    }

    void VisageClapEditorHost::fetchHostExts()
    {
        if (host_ == nullptr) return;
        if (hostGui_ == nullptr)
            hostGui_ = static_cast<const clap_host_gui_t*> (host_->get_extension (host_, CLAP_EXT_GUI));
        if (hostParams_ == nullptr)
            hostParams_ = static_cast<const clap_host_params_t*> (host_->get_extension (host_, CLAP_EXT_PARAMS));
        if (hostState_ == nullptr)
            hostState_ = static_cast<const clap_host_state_t*> (host_->get_extension (host_, CLAP_EXT_STATE));
    }

    // A GUI-driven BULK parameter change (preset load / A-B switch / undo) went into the
    // store via setFromHost. Tell the host to re-pull the values + text and flag the state
    // dirty. rescan(VALUES) updates the host WITHOUT recording an automation point per
    // parameter (which a bulk change must not do); individual knob edits take the other
    // path (setFromUi -> the shell relays them as CLAP param/gesture output events).
    void VisageClapEditorHost::notifyHostEdited()
    {
        fetchHostExts();
        if (hostParams_ != nullptr && hostParams_->rescan != nullptr)
            hostParams_->rescan (host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        if (hostState_ != nullptr && hostState_->mark_dirty != nullptr)
            hostState_->mark_dirty (host_);
    }

    void VisageClapEditorHost::setPluginDimensions (int width, int height)
    {
        if (! app_) return;
#if __APPLE__
        app_->setWindowDimensions (width, height);
#else
        app_->setNativeWindowDimensions (width, height);
#endif
    }

    // While the plugin is INACTIVE there is no process()/params.flush() to drain the
    // store's host-write queue, so a GUI knob edit made with the transport stopped would
    // never reach the host. Ask the host to flush the params when edits are pending — the
    // CLAP contract's inactive-edit path. While active this is harmless (the next process()
    // drains the queue first, so the flush finds nothing).
    void VisageClapEditorHost::flushEditsIfInactive()
    {
        if (! store_.hasPendingHostWrites()) return;
        fetchHostExts();
        if (hostParams_ != nullptr && hostParams_->request_flush != nullptr)
            hostParams_->request_flush (host_);
    }

    void VisageClapEditorHost::installResizeCallback()
    {
        if (app_ == nullptr) return;
        app_->onWindowContentsResized() = [this] { onWindowContentsResizedHandler(); };
    }

    // ═══════════════════════════ FixedSizeVisageClapEditor ═══════════════════════

    double FixedSizeVisageClapEditor::dpiScale() const
    {
#if __APPLE__
        return 1.0;
#else
        return (app_ && app_->width() > 0)
                 ? (double) app_->nativeWidth() / (double) app_->width()
                 : 1.0;
#endif
    }

    int FixedSizeVisageClapEditor::pluginWidth() const
    {
        if (! app_) return 0;
#if __APPLE__
        return (int) app_->width();
#else
        return app_->nativeWidth();
#endif
    }

    int FixedSizeVisageClapEditor::pluginHeight() const
    {
        if (! app_) return 0;
#if __APPLE__
        return (int) app_->height();
#else
        return app_->nativeHeight();
#endif
    }

    void FixedSizeVisageClapEditor::layoutEditorToWindow()
    {
        if (auto* e = editorFrame(); e && app_)
            e->setBounds (0.0f, 0.0f, (float) app_->width(), (float) app_->height());
    }

    void FixedSizeVisageClapEditor::configureWindowOnCreate()
    {
        setPluginDimensions (designW_, designH_);
        layoutEditorToWindow();
        installResizeCallback();
        curW_ = (std::uint32_t) designW_;
        curH_ = (std::uint32_t) designH_;
    }

    void FixedSizeVisageClapEditor::onWindowContentsResizedHandler()
    {
        if (app_ == nullptr) return;
        layoutEditorToWindow();
        fetchHostExts();
        if (hostGui_ != nullptr && hostGui_->request_resize != nullptr)
            hostGui_->request_resize (host_, (std::uint32_t) pluginWidth(), (std::uint32_t) pluginHeight());
    }

    bool FixedSizeVisageClapEditor::getSize (std::uint32_t* width, std::uint32_t* height) noexcept
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

    bool FixedSizeVisageClapEditor::getResizeHints (clap_gui_resize_hints_t* hints) noexcept
    {
        if (hints == nullptr) return false;
        hints->can_resize_horizontally = false;
        hints->can_resize_vertically   = false;
        hints->preserve_aspect_ratio   = true;
        hints->aspect_ratio_width      = (std::uint32_t) designW_;
        hints->aspect_ratio_height     = (std::uint32_t) designH_;
        return true;
    }

    bool FixedSizeVisageClapEditor::adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept
    {
        if (width == nullptr || height == nullptr) return false;
        // Fixed-size surface: every proposal snaps to the design size in the host's units
        // (logical on macOS, native px elsewhere).
        const double s = dpiScale();
        *width  = (std::uint32_t) std::lround (designW_ * s);
        *height = (std::uint32_t) std::lround (designH_ * s);
        return true;
    }

    bool FixedSizeVisageClapEditor::setSize (std::uint32_t width, std::uint32_t height) noexcept
    {
        if (! app_) return false;
        setPluginDimensions ((int) width, (int) height);
        layoutEditorToWindow();
        curW_ = width;
        curH_ = height;
        return true;
    }

    // ═══════════════════════════ ResizableVisageClapEditor ═══════════════════════

    int ResizableVisageClapEditor::hostFacingWidth() const
    {
        if (! app_) return (int) curW_;
#if __APPLE__
        return (int) curW_;
#else
        visage::Window* w = app_->window();
        return (w && w->clientWidth() > 0) ? w->clientWidth() : (int) curW_;
#endif
    }

    int ResizableVisageClapEditor::hostFacingHeight() const
    {
        if (! app_) return (int) curH_;
#if __APPLE__
        return (int) curH_;
#else
        visage::Window* w = app_->window();
        return (w && w->clientHeight() > 0) ? w->clientHeight() : (int) curH_;
#endif
    }

    // Re-assert the UNIFORM ZOOM: set the window's dpi_scale to physicalHeight/designH and
    // pin the editor to the fixed design plane. Because visage routes every logical<->native
    // path through the window dpi_scale, this renders the whole editor at the design layout
    // uniformly scaled to the window, compositing the OS Retina/HiDPI factor in automatically
    // (we derive from the physical height).
    //
    // FIXED-POINT INVARIANT (what keeps the AU resize chain from recursing): the app frame's
    // native bounds are set to the LIVE window client size (physW,physH), so the cascade this
    // triggers feeds setInternalWindowSize a size equal to the current client size, which
    // early-returns WITHOUT calling windowContentsResized again. Every writable sink here is
    // idempotent (Frame::setBounds / setDpiScale early-return on unchanged; Window::setDpiScale
    // is guarded by an explicit compare), so running this with an unchanged window size writes
    // nothing and starts no further callbacks. `inScaleSync_` blocks synchronous re-entry.
    void ResizableVisageClapEditor::syncWindowScale()
    {
        auto* editor = editorFrame();
        if (inScaleSync_ || app_ == nullptr || editor == nullptr) return;
        visage::Window* win = app_->window();
        const int physH = win ? win->clientHeight() : app_->nativeHeight();
        const int physW = win ? win->clientWidth()  : app_->nativeWidth();
        if (physH <= 0 || physW <= 0) return;
        const float dpi = static_cast<float> (physH) / static_cast<float> (geometry_.designH);

        inScaleSync_ = true;
        // Window::setDpiScale is a bare assignment (no unchanged-value guard), so only write
        // it when it actually moves — otherwise a same-size re-entry would dirty nothing yet
        // still churn.
        if (win && win->dpiScale() != dpi) win->setDpiScale (dpi);
        // Drive the WHOLE frame tree from the corrected dpi FIRST, then recompute native
        // bounds, so every frame ends up re-derived at the SAME dpi. Order is load-bearing
        // (Frame::setDpiScale only assigns dpi_scale_; native_bounds_ updates only on a later
        // setBounds/setNativeBounds).
        app_->setDpiScale (dpi);
        app_->setNativeBounds (0, 0, physW, physH);
        if (win) app_->setCanvasDetails();

        // Fill the ACTUAL render surface (physW x physH == canvas == drawable), NOT the fixed
        // design rect — the host (Logic's AU) does NOT honour our aspect hint on resize and can
        // force a size below our minimum; treat that physical size as authoritative and paint it
        // edge to edge. setNativeBounds pins the native EXACTLY (setBounds would derive
        // native=round(logical*dpi), which drifts under an off-aspect dpi).
        editor->setDpiScale (dpi);
        editor->setNativeBounds (0, 0, physW, physH);
        // Tell the editor the current zoom so the resize grip converts its design-space drag
        // into window units.
        setEditorWindowScale (static_cast<float> (hostFacingWidth()) / static_cast<float> (geometry_.designW));
        // Re-raster the ENTIRE subtree: a dpi change moves every child, and visage's dirty-
        // region cache would otherwise keep the previous size's raster for any frame we did not
        // explicitly redraw.
        editor->redrawAll();
        inScaleSync_ = false;
    }

    bool ResizableVisageClapEditor::dynamicMaxWindowUnits (double& maxW, double& maxH) const
    {
        maxW = 0.0; maxH = 0.0;
        if (app_ == nullptr) return false;
        visage::Window* win = app_->window();
        if (win == nullptr) return false;
        const int physW = win->clientWidth();
        const int hostW = hostFacingWidth();
        if (physW <= 0 || hostW <= 0) return false;
        const visage::IPoint maxPhys = win->maxWindowDimensions(); // physical px, work area
        if (maxPhys.x <= 0 || maxPhys.y <= 0) return false;

        const double toWindowUnits = static_cast<double> (hostW) / static_cast<double> (physW);
        // Reserve headroom for host plugin-window chrome (title bar / toolbar) the screen work
        // area does not exclude, so the window + grip stay fully on-screen.
        constexpr double kHostChromeMargin = 80.0; // window units; conservative
        maxW = static_cast<double> (maxPhys.x) * toWindowUnits - kHostChromeMargin;
        maxH = static_cast<double> (maxPhys.y) * toWindowUnits - kHostChromeMargin;
        if (maxW <= 0.0 || maxH <= 0.0) { maxW = 0.0; maxH = 0.0; return false; }
        return true;
    }

    void ResizableVisageClapEditor::requestResizeFromEditor (float w, float h)
    {
        fetchHostExts();
        if (hostGui_ == nullptr || hostGui_->request_resize == nullptr) return;
        std::uint32_t rw = static_cast<std::uint32_t> (std::lround (w));
        std::uint32_t rh = static_cast<std::uint32_t> (std::lround (h));
        double maxW = 0.0, maxH = 0.0;
        dynamicMaxWindowUnits (maxW, maxH); // {0,0} if unavailable -> static cap only
        factory_shell::snapEditorSizeForScale (geometry_, 1.0, rw, rh, maxW, maxH);
        hostGui_->request_resize (host_, rw, rh);
    }

    void ResizableVisageClapEditor::configureWindowOnCreate()
    {
        setPluginDimensions (defaultW_, defaultH_);
        app_->setMinimumDimensions (static_cast<float> (geometry_.minW), static_cast<float> (geometry_.minH));
        syncWindowScale();                 // dpi = physH/designH, editor -> design bounds
        app_->setFixedAspectRatio (true);  // captures the current design aspect
        installResizeCallback();
        curW_ = (std::uint32_t) defaultW_;
        curH_ = (std::uint32_t) defaultH_;
    }

    void ResizableVisageClapEditor::afterParentAttached()
    {
        // Attaching creates the native window; addToWindow() seeds the frame tree from the OS
        // DPI factor. Re-assert our uniform zoom so the editor opens at the design layout scaled
        // to the window, not the OS-DPI logical size.
        syncWindowScale();
    }

    // Content-driven resize (an OS DPI move, or the cascade our own writes set off). Re-pin the
    // editor to the fixed design plane via the IDEMPOTENT syncWindowScale, then relay to the
    // host ONLY when the window's host-facing size actually changed and we are not already
    // mid-setSize (shouldRelayHostResize). This pair is a fixed point (see RsClapEditor.cpp's
    // history: relaying unconditionally is what closed the Logic AU stack-overflow loop).
    void ResizableVisageClapEditor::onWindowContentsResizedHandler()
    {
        if (app_ == nullptr) return;
        syncWindowScale();
        const std::uint32_t hw = static_cast<std::uint32_t> (hostFacingWidth());
        const std::uint32_t hh = static_cast<std::uint32_t> (hostFacingHeight());
        if (! factory_shell::shouldRelayHostResize (curW_, curH_, hw, hh, inSetSize_))
            return;
        curW_ = hw;
        curH_ = hh;
        fetchHostExts();
        if (hostGui_ != nullptr && hostGui_->request_resize != nullptr)
            hostGui_->request_resize (host_, hw, hh);
    }

    bool ResizableVisageClapEditor::getSize (std::uint32_t* width, std::uint32_t* height) noexcept
    {
        if (width == nullptr || height == nullptr) return false;
        // Host-facing size = WINDOW size (logical points on macOS, physical px elsewhere) — NOT
        // the editor's logical bounds, which are pinned at the design regardless of window size.
        *width  = static_cast<std::uint32_t> (hostFacingWidth());
        *height = static_cast<std::uint32_t> (hostFacingHeight());
        return true;
    }

    bool ResizableVisageClapEditor::getResizeHints (clap_gui_resize_hints_t* hints) noexcept
    {
        if (hints == nullptr) return false;
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically   = true;
        hints->preserve_aspect_ratio   = true;
        hints->aspect_ratio_width      = static_cast<std::uint32_t> (geometry_.designW);
        hints->aspect_ratio_height     = static_cast<std::uint32_t> (geometry_.designH);
        return true;
    }

    bool ResizableVisageClapEditor::adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept
    {
        if (width == nullptr || height == nullptr) return false;
        // Snap to the design aspect + resize limits (window units); also clamp to the DYNAMIC
        // display limit so a host-driven resize cannot exceed the usable screen.
        double maxW = 0.0, maxH = 0.0;
        dynamicMaxWindowUnits (maxW, maxH); // {0,0} if unavailable -> static cap only
        factory_shell::snapEditorSizeForScale (geometry_, 1.0, *width, *height, maxW, maxH);
        return true;
    }

    bool ResizableVisageClapEditor::setSize (std::uint32_t width, std::uint32_t height) noexcept
    {
        if (! app_) return false;
        // IDEMPOTENT early-return: Logic re-sends the current size repeatedly during its resize
        // handshake; re-entering the resize machinery for a size we are already at is one arm of
        // the stack-overflow loop, so a no-op change must do nothing at all.
        if (width == curW_ && height == curH_) return true;

        // ORIGIN GUARD: the host is the source of this size, so while we apply it the
        // onWindowContentsResized handler must NOT echo it back (shouldRelayHostResize returns
        // false when inSetSize_).
        inSetSize_ = true;
        setPluginDimensions (static_cast<int> (width), static_cast<int> (height));
        curW_ = width;
        curH_ = height;
        syncWindowScale();
        inSetSize_ = false;
        return true;
    }
} // namespace factory_ui_visage
