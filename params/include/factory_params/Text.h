#pragma once

#include "factory_params/ParamDesc.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

//
// factory_params::Text — minimal value<->text for the future parameter UI.
//
// formatValue reproduces the display convention of factory_ui::setSliderDecimals
// (see ui/include/factory_ui/FactoryChrome.h): a fixed number of decimal places
// plus the parameter's text-value suffix (its `unit`, verbatim, incl. any leading
// space). Choice renders the chosen label; Bool renders "On"/"Off".
//
namespace factory_params
{
    // Fixed `decimals` digits + the unit suffix. Choice -> the label; Bool -> On/Off.
    inline std::string formatValue (const ParamDesc& d, float realValue, int decimals)
    {
        if (d.type == ParamType::Bool)
            return realValue > 0.5f ? "On" : "Off";

        if (d.type == ParamType::Choice)
        {
            if (d.choices.empty())
                return {};
            int idx = static_cast<int> (realValue);
            if (idx < 0) idx = 0;
            if (idx >= static_cast<int> (d.choices.size())) idx = static_cast<int> (d.choices.size()) - 1;
            return d.choices[static_cast<std::size_t> (idx)];
        }

        if (decimals < 0) decimals = 0;
        char buf[64];
        std::snprintf (buf, sizeof buf, "%.*f", decimals, static_cast<double> (realValue));
        return std::string (buf) + d.unit;
    }

    // Parse display text back to a real value. Accepts the unit suffix present or
    // absent (the number is read leading, the suffix ignored). Choice: an exact
    // label match first, else a numeric index. Bool: On/Off/true/false/yes/no (any
    // case) or a number. Returns false only when nothing sensible parses.
    inline bool parseValue (const ParamDesc& d, std::string_view text, float& outReal)
    {
        std::size_t b = 0, e = text.size();
        while (b < e && std::isspace (static_cast<unsigned char> (text[b]))) ++b;
        while (e > b && std::isspace (static_cast<unsigned char> (text[e - 1]))) --e;
        const std::string_view t = text.substr (b, e - b);

        if (d.type == ParamType::Bool)
        {
            std::string low;
            low.reserve (t.size());
            for (char c : t) low.push_back (static_cast<char> (std::tolower (static_cast<unsigned char> (c))));
            if (low == "on"  || low == "true"  || low == "yes") { outReal = 1.0f; return true; }
            if (low == "off" || low == "false" || low == "no")  { outReal = 0.0f; return true; }

            const std::string s (t);
            char* endp = nullptr;
            const double n = std::strtod (s.c_str(), &endp);
            if (endp == s.c_str()) return false;
            outReal = n > 0.5 ? 1.0f : 0.0f;
            return true;
        }

        if (d.type == ParamType::Choice)
        {
            for (std::size_t i = 0; i < d.choices.size(); ++i)
                if (t == d.choices[i]) { outReal = static_cast<float> (i); return true; }

            const std::string s (t);
            char* endp = nullptr;
            const long idx = std::strtol (s.c_str(), &endp, 10);
            if (endp == s.c_str()) return false;
            outReal = static_cast<float> (idx);
            return true;
        }

        // Float: strtof reads the leading number and stops at the unit suffix.
        const std::string s (t);
        char* endp = nullptr;
        const float v = std::strtof (s.c_str(), &endp);
        if (endp == s.c_str()) return false;
        outReal = v;
        return true;
    }
}
