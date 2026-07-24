#pragma once
//
// factory_ui_visage/ClapEditorHost.h — the shared Visage-backed factory_shell::
// IClapEditor host that every clap-first plugin's editor derives from, so the
// identical CLAP↔Visage boilerplate (the ~200 lines RS and pitch-fix used to copy)
// lives in ONE place. Three layers:
//
//   VisageClapEditorHost         — the truly-identical base: kNativeApi selection,
//     is_api_supported / get_preferred_api / set_scale / show / hide / posix-fd, the
//     clap.gui create/destroy/setParent skeletons, host-extension access
//     (fetchHostExts / notifyHostEdited), the macOS-logical/native window-size split,
//     and the inactive-edit param flush. Owns the visage::ApplicationWindow.
//   FixedSizeVisageClapEditor    — a non-resizable editor (pitch-fix): the design-size
//     resize surface + layoutEditorToWindow.
//   ResizableVisageClapEditor    — a uniform-zoom, aspect-locked, resizable editor
//     (resonance-suppressor, dynamic-eq): syncWindowScale + dynamicMaxWindowUnits + the
//     Logic-AU resize-loop fix, parameterised by an EditorGeometry. EXACT code motion of
//     RsClapEditor.cpp's proven logic — the runtime behaviour is preserved.
//
// A plugin's concrete editor (RsClapEditorImpl / DeqClapEditor / PfClapEditorImpl)
// derives the matching layer and implements only the plugin-specific hooks: build its
// visage editor Frame, expose it, and (resizable) push the window scale into it. This
// header is compiled ONLY in a clap-first GUI build (the factory_ui_visage_clap_host
// target links factory_shell + clap), never in the WASM gallery / headless tests, so
// factory_ui_visage itself stays CLAP-free.
//
// Threading: every method is [main-thread] (the CLAP gui contract). The audio thread
// never touches this object.
//
#include "factory_shell/ClapEditor.h"
#include "factory_shell/ResizableEditorGeometry.h"

#include "factory_params/ParamStore.h"

#include <clap/clap.h>
#include <visage/app.h>

#include <cstdint>
#include <memory>

namespace factory_ui_visage
{
    // ── base: the identical IClapEditor boilerplate + ApplicationWindow ownership ──
    class VisageClapEditorHost : public factory_shell::IClapEditor
    {
    public:
        // `store` backs the inactive-edit flush (hasPendingHostWrites); it is the SAME
        // ParamStore the CLAP shell owns and the concrete editor binds its controls to.
        VisageClapEditorHost (const clap_host_t* host, factory_params::ParamStore& store)
            : host_ (host), store_ (store) {}
        ~VisageClapEditorHost() override = default; // derived dtor calls destroy()

        bool isApiSupported (const char* api, bool isFloating) const noexcept override;
        bool getPreferredApi (const char** api, bool* isFloating) const noexcept override;
        bool setScale (double) noexcept override { return false; } // visage resolves OS DPI itself

        bool create (const char* api, bool isFloating) noexcept override;   // template method
        void destroy() noexcept override;                                    // template method
        bool setParent (const clap_window_t* window) noexcept override;

        bool show() noexcept override;
        bool hide() noexcept override;
        void onHostStateRestored() noexcept override;

        int  posixFd() const noexcept override;
        void onPosixFd (clap_posix_fd_flags_t flags) noexcept override;

    protected:
        // ── plugin hooks (the concrete editor implements) ──
        // Construct the concrete visage editor Frame, add it to app_ (addChild), wire its
        // per-frame callbacks, and return it. Return nullptr on failure. Called by create()
        // after app_ exists.
        virtual visage::Frame* buildEditor() = 0;
        virtual visage::Frame* editorFrame() const = 0; // the current editor, or nullptr
        virtual void resetEditor() = 0;                 // drop the derived's editor ptr
        // Post-editor window setup (dimensions + resize policy). Fixed vs Resizable differ.
        virtual void configureWindowOnCreate() = 0;
        virtual void onWindowContentsResizedHandler() = 0; // the OS "contents resized" relay

        // Optional plugin hooks (default no-ops):
        virtual void afterParentAttached() {}    // resizable: syncWindowScale; fixed: none
        virtual void onStateReplacedHook() {}    // editorFrame()->onStateReplaced() (typed)
        virtual void onEditorCreated() {}        // e.g. feed.setDisplayActive(true)
        virtual void onEditorDestroying() {}     // e.g. feed.setDisplayActive(false)

        // ── shared helpers ──
        void fetchHostExts();
        void notifyHostEdited(); // bulk change: rescan(VALUES|TEXT) + mark_dirty
        void setPluginDimensions (int width, int height); // macOS logical / native px split
        void flushEditsIfInactive(); // request_flush when GUI edits are pending + no process()
        void installResizeCallback(); // wire app_->onWindowContentsResized() -> the handler

        const clap_host_t*         host_       = nullptr;
        const clap_host_gui_t*     hostGui_    = nullptr;
        const clap_host_params_t*  hostParams_ = nullptr;
        const clap_host_state_t*   hostState_  = nullptr;
        factory_params::ParamStore& store_;

        std::unique_ptr<visage::ApplicationWindow> app_;
        std::uint32_t curW_ = 0, curH_ = 0;
    };

    // ── fixed-size editor (pitch-fix) ─────────────────────────────────────────
    class FixedSizeVisageClapEditor : public VisageClapEditorHost
    {
    public:
        FixedSizeVisageClapEditor (const clap_host_t* host, factory_params::ParamStore& store,
                                   int designW, int designH)
            : VisageClapEditorHost (host, store), designW_ (designW), designH_ (designH) {}

        bool getSize (std::uint32_t* width, std::uint32_t* height) noexcept override;
        bool canResize() const noexcept override { return false; }
        bool getResizeHints (clap_gui_resize_hints_t* hints) noexcept override;
        bool adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept override;
        bool setSize (std::uint32_t width, std::uint32_t height) noexcept override;

    protected:
        void configureWindowOnCreate() override;
        void onWindowContentsResizedHandler() override;

        int    pluginWidth() const;
        int    pluginHeight() const;
        double dpiScale() const;
        void   layoutEditorToWindow();

        int designW_, designH_;
    };

    // ── resizable, uniform-zoom, aspect-locked editor (RS, dynamic-eq) ─────────
    class ResizableVisageClapEditor : public VisageClapEditorHost
    {
    public:
        // geometry: design aspect + resize limits (window units). defaultW/H: the size the
        // window OPENS at (typically below the reference, so host chrome fits a laptop).
        ResizableVisageClapEditor (const clap_host_t* host, factory_params::ParamStore& store,
                                   const factory_shell::EditorGeometry& geometry, int defaultW, int defaultH)
            : VisageClapEditorHost (host, store), geometry_ (geometry),
              defaultW_ (defaultW), defaultH_ (defaultH) {}

        bool getSize (std::uint32_t* width, std::uint32_t* height) noexcept override;
        bool canResize() const noexcept override { return true; }
        bool getResizeHints (clap_gui_resize_hints_t* hints) noexcept override;
        bool adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept override;
        bool setSize (std::uint32_t width, std::uint32_t height) noexcept override;

    protected:
        // Plugin hook: tell the editor the current window scale so its resize grip converts
        // a design-space drag into window units (RS/Deq: editor_->setWindowScale).
        virtual void setEditorWindowScale (float windowScale) = 0;

        void configureWindowOnCreate() override;
        void onWindowContentsResizedHandler() override;
        void afterParentAttached() override; // re-assert the uniform zoom after embed

        // The corner-grip resize path (the editor proposes a WINDOW size): snap to the
        // aspect + static + dynamic display limits, then request_resize. The ONLY resize
        // path a host with no window edge (Logic's AU) offers.
        void requestResizeFromEditor (float w, float h);

        // Re-assert the UNIFORM ZOOM (window dpi = physicalHeight/designH) and pin the
        // editor to the fixed design plane. The fixed-point invariant that keeps the AU
        // resize chain from recursing (see the .cpp for the full rationale).
        void syncWindowScale();

        // Largest window (window units) that fits the current display's usable area, or
        // false / {0,0} before the window is attached (callers fall back to the static cap).
        bool dynamicMaxWindowUnits (double& maxW, double& maxH) const;

        int hostFacingWidth() const;  // the host-facing window size (logical pt on macOS,
        int hostFacingHeight() const; // physical px elsewhere)

        factory_shell::EditorGeometry geometry_;
        int  defaultW_, defaultH_;
        bool inScaleSync_ = false; // synchronous re-entrancy guard for syncWindowScale()
        bool inSetSize_   = false; // origin guard: suppress request_resize echo during setSize
    };
} // namespace factory_ui_visage
