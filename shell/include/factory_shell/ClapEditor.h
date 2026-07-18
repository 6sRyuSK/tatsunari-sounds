#pragma once
//
// factory_shell/ClapEditor.h — the framework-free editor seam between the generic
// ClapShellPlugin and a plugin's concrete GUI. The shell drives the CLAP gui
// extension (and, on Linux, posix-fd-support) entirely through this abstract
// interface, so the shell header — and every headless-validatable chunk-3a build —
// stays free of Visage/JUCE. The visage-backed implementation lives in the plugin
// and is compiled ONLY when that plugin's GUI build is enabled (RS: under
// FACTORY_RS_CLAP_GUI, which links native visage). A plugin that supplies no editor
// (chunk-3a policies) is unaffected: the shell simply never advertises clap.gui.
//
// Threading: every method is [main-thread] (the CLAP gui contract). No audio thread
// ever touches an IClapEditor.
//
// The methods map 1:1 onto clap_plugin_gui_t, minus the host round-trips the shell
// owns (posix-fd register/unregister live in the shell; the editor only exposes its
// fd + an on-fd pump). Return false wherever the corresponding CLAP method would.
//
#include <clap/clap.h>

#include <cstdint>
#include <type_traits>

namespace factory_shell
{
    class IClapEditor
    {
    public:
        virtual ~IClapEditor() = default;

        // ── clap.gui ─────────────────────────────────────────────────────────
        // is_api_supported / get_preferred_api are queried BEFORE create(), so they
        // must answer without a live window.
        virtual bool isApiSupported (const char* api, bool isFloating) const noexcept = 0;
        virtual bool getPreferredApi (const char** api, bool* isFloating) const noexcept = 0;

        // Allocate the native window + editor (embedded, non-floating). The window
        // is not attached to a parent yet (see setParent) and may not be visible
        // yet (see show).
        virtual bool create (const char* api, bool isFloating) noexcept = 0;
        virtual void destroy() noexcept = 0;

        virtual bool setScale (double scale) noexcept = 0;
        virtual bool getSize (std::uint32_t* width, std::uint32_t* height) noexcept = 0;
        virtual bool canResize() const noexcept = 0;
        virtual bool getResizeHints (clap_gui_resize_hints_t* hints) noexcept = 0;
        virtual bool adjustSize (std::uint32_t* width, std::uint32_t* height) noexcept = 0;
        virtual bool setSize (std::uint32_t width, std::uint32_t height) noexcept = 0;

        // Embed into the host-provided parent window (X11 on Linux, HWND on Windows,
        // NSView on macOS — the concrete impl reads the matching clap_window union
        // member for the api it created with).
        virtual bool setParent (const clap_window_t* window) noexcept = 0;

        virtual bool show() noexcept = 0;
        virtual bool hide() noexcept = 0;

        // ── Linux posix-fd-support ───────────────────────────────────────────
        // The editor's event fd, or -1 when unavailable (no window / non-Linux).
        // The shell registers/unregisters it with the host; the host then calls
        // back onPosixFd() to let the editor pump its native event queue.
        virtual int  posixFd() const noexcept { return -1; }
        virtual void onPosixFd (clap_posix_fd_flags_t flags) noexcept { (void) flags; }
    };

    // Detect whether a shell Policy provides a GUI: it must declare a truthy
    // `static constexpr bool kHasEditor` (and, when true, a matching makeEditor
    // factory the shell calls under `if constexpr`). Policies without it — every
    // headless chunk-3a plugin — resolve to false and the shell never advertises
    // clap.gui for them, so they remain fully headless-validatable.
    template <class P, class = void>
    struct PolicyHasEditor : std::false_type {};

    template <class P>
    struct PolicyHasEditor<P, std::void_t<decltype (P::kHasEditor)>>
        : std::bool_constant<P::kHasEditor> {};
}
