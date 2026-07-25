#pragma once

#include "factory_params/ParamStore.h"
#include "factory_ui_visage/Theme.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>

// Shared window.ui bridge for real plugin editors. A new editor only supplies
// these small callbacks; the parameter/theme/font/dropdown C ABI stays uniform.
namespace ui_dev_harness
{
    struct Target
    {
        factory_params::ParamStore* store = nullptr;
        factory_ui_visage::Theme* theme = nullptr;
        visage::Frame* root = nullptr;

        std::function<void (bool)> freeze;
        std::function<void (double)> feedSpectrum;
        std::function<bool (const std::string&, float&, float&, float&, float&)> widgetRect;
        std::function<bool (int)> openDropdown;
        std::function<bool()> dropdownOpen;
        std::function<int()> dropdownCount;
        std::function<bool (int, float&, float&)> dropdownRowCentre;
        std::function<int()> presetIndex;
    };

    // Convention adapter used by ordinary plugin editors. The editor supplies
    // store(), widgetRectInWindow(), openNamedDropdown(), dropdown(), and
    // presetIndex(); callers then add only freeze/feed-specific callbacks.
    template <class Editor>
    Target makeEditorTarget (Editor* editor, factory_ui_visage::Theme* theme)
    {
        Target target;
        target.store = editor ? &editor->store() : nullptr;
        target.theme = theme;
        target.root = editor;
        target.widgetRect = [editor] (const std::string& key, float& x, float& y, float& w, float& h) {
            return editor && editor->widgetRectInWindow (key, x, y, w, h);
        };
        target.openDropdown = [editor] (int which) { return editor && editor->openNamedDropdown (which); };
        target.dropdownOpen = [editor] { return editor && editor->dropdown() && editor->dropdown()->isOpen(); };
        target.dropdownCount = [editor] { return editor && editor->dropdown() ? editor->dropdown()->itemCount() : 0; };
        target.dropdownRowCentre = [editor] (int row, float& x, float& y) {
            return editor && editor->dropdown() && editor->dropdown()->rowCentreInWindow (row, x, y);
        };
        target.presetIndex = [editor] { return editor ? editor->presetIndex() : -1; };
        return target;
    }

    void attach (Target target);
}
