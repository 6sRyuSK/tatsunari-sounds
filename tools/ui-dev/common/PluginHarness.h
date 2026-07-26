#pragma once

#include "factory_params/ParamStore.h"
#include "factory_ui_visage/Theme.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>
#include <utility>

// Shared window.ui bridge for real plugin editors. A new editor only supplies
// these small callbacks; the parameter/theme/font/dropdown C ABI stays uniform.
// EVERY plugin-editor app attaches one of these — a bridge that hand-rolls its own
// ui_* exports fragments the ABI the Playwright drivers assume is universal. (The
// widget gallery is not a plugin editor and keeps its own bridge, but it exports
// the same ui_* names with the same signatures.)
namespace ui_dev_harness
{
    struct Target
    {
        factory_params::ParamStore* store = nullptr;
        visage::Frame* root = nullptr;

        // Theme seam. `accent` reads the live palette; `reloadTheme` applies an
        // overlay document. useSharedTheme() wires both for editors that sit on a
        // plain factory_ui_visage::Theme; an editor owning a plugin extras block
        // (RS folds in its "rs" keys) overrides them instead.
        std::function<unsigned int()> accent;
        std::function<bool (const char*, std::string&)> reloadTheme;

        std::function<void (bool)> freeze;
        std::function<void (double)> feedSpectrum;
        std::function<bool (const std::string&, float&, float&, float&, float&)> widgetRect;
        // Dropdowns are addressed by NAME ("preset", "type", …), never by a
        // per-plugin magic index: the ABI is shared, so its arguments must be too.
        std::function<bool (const std::string&)> openDropdown;
        std::function<bool()> dropdownOpen;
        std::function<int()> dropdownCount;
        std::function<bool (int, float&, float&)> dropdownRowCentre;
        std::function<int()> presetIndex;
    };

    // Plain shared-Theme wiring. Hot reload overwrites the live instance in place,
    // so widgets holding `const Theme&` stay valid (the house rule).
    inline void useSharedTheme (Target& target, factory_ui_visage::Theme* theme)
    {
        if (theme == nullptr) return;
        target.accent = [theme] { return theme->palette.accent; };
        target.reloadTheme = [theme] (const char* json, std::string& error)
        {
            auto next = *theme;
            if (! next.applyOverlay (json, error)) return false;
            *theme = std::move (next);
            return true;
        };
    }

    // Convention adapter used by ordinary plugin editors. The editor supplies
    // store(), widgetRectInWindow(), openNamedDropdown(), dropdown(), and
    // presetIndex(); callers then add only freeze/feed-specific callbacks. Pass a
    // null `theme` when the editor owns its own theme document (see RsBridge).
    template <class Editor>
    Target makeEditorTarget (Editor* editor, factory_ui_visage::Theme* theme = nullptr)
    {
        Target target;
        target.store = editor ? &editor->store() : nullptr;
        target.root = editor;
        target.widgetRect = [editor] (const std::string& key, float& x, float& y, float& w, float& h) {
            return editor && editor->widgetRectInWindow (key, x, y, w, h);
        };
        target.openDropdown = [editor] (const std::string& name) {
            return editor && editor->openNamedDropdown (name);
        };
        target.dropdownOpen = [editor] { return editor && editor->dropdown() && editor->dropdown()->isOpen(); };
        target.dropdownCount = [editor] { return editor && editor->dropdown() ? editor->dropdown()->itemCount() : 0; };
        target.dropdownRowCentre = [editor] (int row, float& x, float& y) {
            return editor && editor->dropdown() && editor->dropdown()->rowCentreInWindow (row, x, y);
        };
        target.presetIndex = [editor] { return editor ? editor->presetIndex() : -1; };
        useSharedTheme (target, theme);
        return target;
    }

    void attach (Target target);
}
