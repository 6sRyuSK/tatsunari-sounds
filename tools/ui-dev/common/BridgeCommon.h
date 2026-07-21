#pragma once

#include "factory_params/ParamDesc.h"
#include "factory_params/ParamStore.h"

#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>

//
// ui_dev_bridge — the pieces the gallery and rs-editor C bridges share verbatim:
// JSON escaping/emission and the ui_* function bodies that are identical on both
// targets. Header-only on purpose: each bridge keeps its own extern "C" surface
// (the two targets export DIFFERENT function sets) and its own file-static string
// buffers; only the bodies live here. Numbers stream through the same
// ostringstream path as before (float streams as double), so the JSON text the
// Playwright driver parses is unchanged.
//
namespace ui_dev_bridge
{
    inline std::string jsonEscape (const std::string& s)
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

    inline const char* typeName (factory_params::ParamType t)
    {
        switch (t)
        {
            case factory_params::ParamType::Float:  return "float";
            case factory_params::ParamType::Bool:   return "bool";
            case factory_params::ParamType::Choice: return "choice";
        }
        return "float";
    }

    // {"x":..,"y":..,"w":..,"h":..} — the rect JSON every *_rect getter returns.
    inline std::string jsonRect (float x, float y, float w, float h)
    {
        std::ostringstream o;
        o << "{\"x\":" << x << ",\"y\":" << y << ",\"w\":" << w << ",\"h\":" << h << "}";
        return o.str();
    }

    // {"k":v,...} for ad-hoc numeric objects (mini-knob tip/dial geometry).
    inline std::string jsonNumObj (std::initializer_list<std::pair<const char*, double>> kv)
    {
        std::ostringstream o;
        o << "{";
        bool first = true;
        for (const auto& p : kv)
        {
            if (! first) o << ",";
            first = false;
            o << "\"" << p.first << "\":" << p.second;
        }
        o << "}";
        return o.str();
    }

    // The ui_list_params body: a JSON array describing every parameter — index,
    // id, name, type, range, default, live value and unit.
    inline std::string paramsListJson (factory_params::ParamStore& store)
    {
        std::ostringstream o;
        o << "[";
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
        o << "]";
        return o.str();
    }

    // The ui_reload_theme body: re-apply a theme document on the live target
    // (GalleryFrame / RsEditor — anything with reloadTheme(json, error)).
    template <class Target>
    inline int reloadThemeInto (Target* target, const char* jsonText, std::string& errorBuf)
    {
        if (target == nullptr || jsonText == nullptr)
            return 0;
        std::string error;
        const bool ok = target->reloadTheme (jsonText, error);
        errorBuf = ok ? std::string() : error;
        return ok ? 1 : 0;
    }
}
