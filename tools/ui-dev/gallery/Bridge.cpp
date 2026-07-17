#include "Bridge.h"
#include "GalleryFrame.h"

#include "factory_params/ParamDesc.h"
#include "factory_ui_visage/Fonts.h"

#include <sstream>
#include <string>

#ifdef __EMSCRIPTEN__
 #include <emscripten/emscripten.h>
 #define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
 #define KEEPALIVE
#endif

//
// Bridge — the thin C surface the page JS / Playwright driver calls via
// Module.ccall. Every function is a no-op-safe wrapper around the live
// GalleryFrame + its ParamStore. Returned strings live in file-static buffers
// (valid until the next call to the same getter), which ccall copies out.
//
namespace
{
    GalleryFrame* g_gallery = nullptr;
    std::string   g_listBuffer;   // ui_list_params() result
    std::string   g_errorBuffer;  // ui_reload_theme() / ui_last_error() message
    std::string   g_rectBuffer;   // ui_widget_rect() result

    std::string jsonEscape (const std::string& s)
    {
        std::string out;
        out.reserve (s.size() + 2);
        for (char c : s)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:   out += c;      break;
            }
        }
        return out;
    }

    const char* typeName (factory_params::ParamType t)
    {
        switch (t)
        {
            case factory_params::ParamType::Float:  return "float";
            case factory_params::ParamType::Bool:   return "bool";
            case factory_params::ParamType::Choice: return "choice";
        }
        return "float";
    }
}

namespace gallery
{
    void setBridgeTarget (GalleryFrame* gallery) { g_gallery = gallery; }
}

extern "C"
{
    // JSON array describing every parameter: index, id, name, type, range,
    // default, live value and unit — the JS side's map of the surface.
    KEEPALIVE const char* ui_list_params()
    {
        std::ostringstream o;
        o << "[";
        if (g_gallery != nullptr)
        {
            factory_params::ParamStore& store = g_gallery->store();
            for (int i = 0; i < store.size(); ++i)
            {
                const factory_params::ParamDesc& d = store.desc (i);
                if (i != 0)
                    o << ",";
                o << "{\"index\":" << i
                  << ",\"id\":\"" << jsonEscape (d.id) << "\""
                  << ",\"name\":\"" << jsonEscape (d.name) << "\""
                  << ",\"type\":\"" << typeName (d.type) << "\""
                  << ",\"min\":" << d.minValue
                  << ",\"max\":" << d.maxValue
                  << ",\"default\":" << d.defaultValue
                  << ",\"value\":" << store.value (i)
                  << ",\"unit\":\"" << jsonEscape (d.unit) << "\"}";
            }
        }
        o << "]";
        g_listBuffer = o.str();
        return g_listBuffer.c_str();
    }

    // Live value of a parameter by id (real units). Unknown id -> 0.
    KEEPALIVE double ui_get_param (const char* id)
    {
        if (g_gallery == nullptr || id == nullptr)
            return 0.0;
        const int idx = g_gallery->store().indexOf (id);
        return idx < 0 ? 0.0 : static_cast<double> (g_gallery->store().value (idx));
    }

    // Set a parameter by id, driving the store exactly like the host would
    // (setFromHost), then redraw so the change reaches pixels.
    KEEPALIVE void ui_set_param (const char* id, double real)
    {
        if (g_gallery == nullptr || id == nullptr)
            return;
        const int idx = g_gallery->store().indexOf (id);
        if (idx < 0)
            return;
        g_gallery->store().setFromHost (idx, static_cast<float> (real));
        g_gallery->redrawAll();
    }

    // Stop continuous animation for deterministic screenshots (see GalleryFrame).
    KEEPALIVE void ui_freeze (int frozen)
    {
        if (g_gallery != nullptr)
            g_gallery->setFrozen (frozen != 0);
    }

    // Re-apply a theme at runtime (hot reload). Returns 1 on success, 0 on a
    // malformed document (message retrievable via ui_last_error).
    KEEPALIVE int ui_reload_theme (const char* jsonText)
    {
        if (g_gallery == nullptr || jsonText == nullptr)
            return 0;
        std::string error;
        const bool ok = g_gallery->reloadTheme (jsonText, error);
        g_errorBuffer = ok ? std::string() : error;
        return ok ? 1 : 0;
    }

    KEEPALIVE const char* ui_last_error() { return g_errorBuffer.c_str(); }

    // Current theme accent (0xAARRGGBB) — lets the driver verify a reload landed.
    KEEPALIVE unsigned int ui_get_accent()
    {
        return g_gallery != nullptr ? g_gallery->theme().palette.accent : 0u;
    }

    // Centre of a bound widget in window pixels, so the driver can aim real mouse
    // events. Returns -1 when the id has no widget.
    KEEPALIVE double ui_widget_x (const char* id)
    {
        if (g_gallery == nullptr || id == nullptr)
            return -1.0;
        const int idx = g_gallery->store().indexOf (id);
        float x = 0.0f, y = 0.0f;
        if (idx < 0 || ! g_gallery->widgetCentreInWindow (idx, x, y))
            return -1.0;
        return static_cast<double> (x);
    }

    KEEPALIVE double ui_widget_y (const char* id)
    {
        if (g_gallery == nullptr || id == nullptr)
            return -1.0;
        const int idx = g_gallery->store().indexOf (id);
        float x = 0.0f, y = 0.0f;
        if (idx < 0 || ! g_gallery->widgetCentreInWindow (idx, x, y))
            return -1.0;
        return static_cast<double> (y);
    }

    // --- P2b additions --------------------------------------------------------

    // Rect (window px) of a control keyed by param id ("mode"/"link"/…) or a
    // special name ("preset"/"spectrum"/"valueSetting"). JSON, or "null".
    KEEPALIVE const char* ui_widget_rect (const char* key)
    {
        g_rectBuffer = "null";
        if (g_gallery != nullptr && key != nullptr)
        {
            float x = 0, y = 0, w = 0, h = 0;
            if (g_gallery->widgetRectInWindow (key, x, y, w, h))
            {
                std::ostringstream o;
                o << "{\"x\":" << x << ",\"y\":" << y << ",\"w\":" << w << ",\"h\":" << h << "}";
                g_rectBuffer = o.str();
            }
        }
        return g_rectBuffer.c_str();
    }

    // Switch the active typeface ("quicksand" | "nunito" | "mplus"). 1 on success.
    KEEPALIVE int ui_set_font (const char* name)
    {
        if (! factory_ui_visage::setFontFamilyByName (name))
            return 0;
        if (g_gallery != nullptr)
            g_gallery->redrawAll();
        return 1;
    }

    KEEPALIVE const char* ui_font() { return factory_ui_visage::fontFamilyName(); }

    // Inject a FIXED synthetic spectrum frame at `phase` and converge the model,
    // so a frozen screenshot is deterministic (repeated calls -> identical pixels).
    KEEPALIVE void ui_feed_spectrum (double phase)
    {
        if (g_gallery != nullptr)
            g_gallery->feedSpectrum (phase);
    }

    // Open a control's dropdown (0 = preset selector, 1 = value setting). 1 on ok.
    KEEPALIVE int ui_open_dropdown (int which)
    {
        return (g_gallery != nullptr && g_gallery->openNamedDropdown (which)) ? 1 : 0;
    }

    KEEPALIVE int ui_dropdown_open()
    {
        return (g_gallery != nullptr && g_gallery->dropdownOpen()) ? 1 : 0;
    }

    KEEPALIVE int ui_dropdown_item_count()
    {
        return (g_gallery != nullptr && g_gallery->dropdown() != nullptr)
                   ? g_gallery->dropdown()->itemCount() : 0;
    }

    // Centre (window px) of an open dropdown's item row, so the driver can click it.
    KEEPALIVE double ui_dropdown_x (int itemIndex)
    {
        float x = 0.0f, y = 0.0f;
        if (g_gallery != nullptr && g_gallery->dropdown() != nullptr
            && g_gallery->dropdown()->rowCentreInWindow (itemIndex, x, y))
            return static_cast<double> (x);
        return -1.0;
    }

    KEEPALIVE double ui_dropdown_row_y (int itemIndex)
    {
        float x = 0.0f, y = 0.0f;
        if (g_gallery != nullptr && g_gallery->dropdown() != nullptr
            && g_gallery->dropdown()->rowCentreInWindow (itemIndex, x, y))
            return static_cast<double> (y);
        return -1.0;
    }

    // Current preset-selector item-row index (for verifying next/prev onChange).
    KEEPALIVE int ui_preset_index()
    {
        return g_gallery != nullptr ? g_gallery->presetIndex() : -1;
    }

    // Where visage delivered the last mouse-down (window px) — coordinate probe.
    KEEPALIVE double ui_last_mouse_x() { return g_gallery ? (double) g_gallery->lastMouseX() : -1.0; }
    KEEPALIVE double ui_last_mouse_y() { return g_gallery ? (double) g_gallery->lastMouseY() : -1.0; }
}
