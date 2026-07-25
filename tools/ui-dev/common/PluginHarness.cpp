#include "PluginHarness.h"
#include "BridgeCommon.h"

#include "factory_ui_visage/Fonts.h"

#include <string>
#include <utility>

#ifdef __EMSCRIPTEN__
 #include <emscripten/emscripten.h>
 #define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
 #define KEEPALIVE
#endif

namespace
{
    ui_dev_harness::Target g_target;
    std::string g_list, g_error, g_rect;

    bool rectFor (const char* key, float& x, float& y, float& w, float& h)
    {
        return key != nullptr && g_target.widgetRect
            && g_target.widgetRect (key, x, y, w, h);
    }
}

namespace ui_dev_harness
{
    void attach (Target target) { g_target = std::move (target); }
}

extern "C"
{
    KEEPALIVE const char* ui_list_params()
    {
        g_list = g_target.store ? ui_dev_bridge::paramsListJson (*g_target.store) : "[]";
        return g_list.c_str();
    }

    KEEPALIVE double ui_get_param (const char* id)
    {
        if (! g_target.store || ! id) return 0.0;
        const int i = g_target.store->indexOf (id);
        return i >= 0 ? (double) g_target.store->value (i) : 0.0;
    }

    KEEPALIVE void ui_set_param (const char* id, double real)
    {
        if (! g_target.store || ! id) return;
        const int i = g_target.store->indexOf (id);
        if (i < 0) return;
        g_target.store->setFromHost (i, (float) real);
        if (g_target.root) g_target.root->redrawAll();
    }

    KEEPALIVE void ui_freeze (int frozen)
    {
        if (g_target.freeze) g_target.freeze (frozen != 0);
    }

    KEEPALIVE int ui_reload_theme (const char* jsonText)
    {
        if (! g_target.reloadTheme || ! jsonText) return 0;
        std::string error;
        if (! g_target.reloadTheme (jsonText, error))
        {
            g_error = std::move (error);
            return 0;
        }
        g_error.clear();
        if (g_target.root) g_target.root->redrawAll();
        return 1;
    }

    KEEPALIVE const char* ui_last_error() { return g_error.c_str(); }
    KEEPALIVE unsigned int ui_get_accent()
    {
        return g_target.accent ? g_target.accent() : 0u;
    }

    KEEPALIVE double ui_widget_x (const char* key)
    {
        float x = 0, y = 0, w = 0, h = 0;
        return rectFor (key, x, y, w, h) ? (double) (x + 0.5f * w) : -1.0;
    }

    KEEPALIVE double ui_widget_y (const char* key)
    {
        float x = 0, y = 0, w = 0, h = 0;
        return rectFor (key, x, y, w, h) ? (double) (y + 0.5f * h) : -1.0;
    }

    KEEPALIVE const char* ui_widget_rect (const char* key)
    {
        g_rect = "null";
        float x = 0, y = 0, w = 0, h = 0;
        if (rectFor (key, x, y, w, h))
            g_rect = ui_dev_bridge::jsonRect (x, y, w, h);
        return g_rect.c_str();
    }

    KEEPALIVE int ui_set_font (const char* name)
    {
        if (! factory_ui_visage::setFontFamilyByName (name)) return 0;
        if (g_target.root) g_target.root->redrawAll();
        return 1;
    }

    KEEPALIVE const char* ui_font() { return factory_ui_visage::fontFamilyName(); }
    KEEPALIVE void ui_feed_spectrum (double phase)
    {
        if (g_target.feedSpectrum) g_target.feedSpectrum (phase);
    }
    KEEPALIVE int ui_open_dropdown (const char* name)
    {
        return name != nullptr && g_target.openDropdown && g_target.openDropdown (name) ? 1 : 0;
    }
    KEEPALIVE int ui_dropdown_open()
    {
        return g_target.dropdownOpen && g_target.dropdownOpen() ? 1 : 0;
    }
    KEEPALIVE int ui_dropdown_item_count()
    {
        return g_target.dropdownCount ? g_target.dropdownCount() : 0;
    }
    KEEPALIVE double ui_dropdown_x (int row)
    {
        float x = 0, y = 0;
        return g_target.dropdownRowCentre && g_target.dropdownRowCentre (row, x, y) ? (double) x : -1.0;
    }
    KEEPALIVE double ui_dropdown_row_y (int row)
    {
        float x = 0, y = 0;
        return g_target.dropdownRowCentre && g_target.dropdownRowCentre (row, x, y) ? (double) y : -1.0;
    }
    KEEPALIVE int ui_preset_index()
    {
        return g_target.presetIndex ? g_target.presetIndex() : -1;
    }
}
