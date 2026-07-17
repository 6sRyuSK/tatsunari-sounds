#include "factory_ui_visage/Theme.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

//
// A tiny, dependency-free, strict JSON reader/writer for the theme document.
// Only the subset the theme needs is supported: objects, arrays, strings,
// numbers, booleans and null. Any syntax error, wrong type, unknown key, or
// malformed colour is reported through the `error` string (no exceptions on the
// parse path, so the wasm bridge can call it safely).
//
namespace factory_ui_visage
{
    namespace
    {
        struct JsonValue
        {
            enum class Type { Null, Bool, Number, String, Array, Object };
            Type type = Type::Null;
            bool   b   = false;
            double num = 0.0;
            std::string str;
            std::vector<JsonValue> arr;
            std::vector<std::pair<std::string, JsonValue>> obj;

            const JsonValue* find (const std::string& key) const
            {
                for (const auto& kv : obj)
                    if (kv.first == key)
                        return &kv.second;
                return nullptr;
            }
        };

        // Recursive-descent parser over a null-safe string view.
        class JsonParser
        {
        public:
            JsonParser (const std::string& text) : s_ (text), i_ (0) {}

            bool parse (JsonValue& out)
            {
                skipWs();
                if (! parseValue (out))
                    return false;
                skipWs();
                if (i_ != s_.size())
                    return fail ("trailing characters after top-level value");
                return true;
            }

            const std::string& error() const { return error_; }

        private:
            const std::string& s_;
            std::size_t i_;
            std::string error_;

            bool fail (const std::string& msg)
            {
                // Report a 1-based offset so hand-edited files are easy to fix.
                error_ = "JSON parse error at offset " + std::to_string (i_) + ": " + msg;
                return false;
            }

            void skipWs()
            {
                while (i_ < s_.size())
                {
                    const char c = s_[i_];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++i_;
                    else
                        break;
                }
            }

            bool parseValue (JsonValue& out)
            {
                if (i_ >= s_.size())
                    return fail ("unexpected end of input");

                const char c = s_[i_];
                switch (c)
                {
                    case '{': return parseObject (out);
                    case '[': return parseArray (out);
                    case '"': out.type = JsonValue::Type::String; return parseString (out.str);
                    case 't': case 'f': return parseBool (out);
                    case 'n': return parseNull (out);
                    default:
                        if (c == '-' || (c >= '0' && c <= '9'))
                            return parseNumber (out);
                        return fail (std::string ("unexpected character '") + c + "'");
                }
            }

            bool parseObject (JsonValue& out)
            {
                out.type = JsonValue::Type::Object;
                ++i_; // consume '{'
                skipWs();
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }

                while (true)
                {
                    skipWs();
                    if (i_ >= s_.size() || s_[i_] != '"')
                        return fail ("expected string key in object");

                    std::string key;
                    if (! parseString (key))
                        return false;

                    skipWs();
                    if (i_ >= s_.size() || s_[i_] != ':')
                        return fail ("expected ':' after object key");
                    ++i_;

                    skipWs();
                    JsonValue value;
                    if (! parseValue (value))
                        return false;
                    out.obj.emplace_back (std::move (key), std::move (value));

                    skipWs();
                    if (i_ >= s_.size())
                        return fail ("unterminated object");
                    if (s_[i_] == ',') { ++i_; continue; }
                    if (s_[i_] == '}') { ++i_; return true; }
                    return fail ("expected ',' or '}' in object");
                }
            }

            bool parseArray (JsonValue& out)
            {
                out.type = JsonValue::Type::Array;
                ++i_; // consume '['
                skipWs();
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }

                while (true)
                {
                    skipWs();
                    JsonValue value;
                    if (! parseValue (value))
                        return false;
                    out.arr.push_back (std::move (value));

                    skipWs();
                    if (i_ >= s_.size())
                        return fail ("unterminated array");
                    if (s_[i_] == ',') { ++i_; continue; }
                    if (s_[i_] == ']') { ++i_; return true; }
                    return fail ("expected ',' or ']' in array");
                }
            }

            bool parseString (std::string& out)
            {
                ++i_; // consume opening quote
                out.clear();
                while (i_ < s_.size())
                {
                    const char c = s_[i_++];
                    if (c == '"')
                        return true;
                    if (c == '\\')
                    {
                        if (i_ >= s_.size())
                            return fail ("unterminated escape in string");
                        const char e = s_[i_++];
                        switch (e)
                        {
                            case '"':  out.push_back ('"');  break;
                            case '\\': out.push_back ('\\'); break;
                            case '/':  out.push_back ('/');  break;
                            case 'n':  out.push_back ('\n'); break;
                            case 't':  out.push_back ('\t'); break;
                            case 'r':  out.push_back ('\r'); break;
                            case 'b':  out.push_back ('\b'); break;
                            case 'f':  out.push_back ('\f'); break;
                            default:   return fail ("unsupported string escape");
                        }
                    }
                    else
                    {
                        out.push_back (c);
                    }
                }
                return fail ("unterminated string");
            }

            bool parseNumber (JsonValue& out)
            {
                const std::size_t start = i_;
                if (i_ < s_.size() && s_[i_] == '-') ++i_;
                while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.'
                                          || s_[i_] == 'e' || s_[i_] == 'E'
                                          || s_[i_] == '+' || s_[i_] == '-'))
                    ++i_;
                const std::string tok = s_.substr (start, i_ - start);
                try
                {
                    std::size_t consumed = 0;
                    out.num = std::stod (tok, &consumed);
                    if (consumed != tok.size())
                        return fail ("malformed number '" + tok + "'");
                }
                catch (...)
                {
                    return fail ("malformed number '" + tok + "'");
                }
                out.type = JsonValue::Type::Number;
                return true;
            }

            bool parseBool (JsonValue& out)
            {
                if (s_.compare (i_, 4, "true") == 0)  { i_ += 4; out.type = JsonValue::Type::Bool; out.b = true;  return true; }
                if (s_.compare (i_, 5, "false") == 0) { i_ += 5; out.type = JsonValue::Type::Bool; out.b = false; return true; }
                return fail ("malformed literal (expected true/false)");
            }

            bool parseNull (JsonValue& out)
            {
                if (s_.compare (i_, 4, "null") == 0) { i_ += 4; out.type = JsonValue::Type::Null; return true; }
                return fail ("malformed literal (expected null)");
            }
        };

        // ---- typed extraction from JsonValue (all strict) --------------------

        bool asNumber (const JsonValue& v, float& out, const std::string& where, std::string& err)
        {
            if (v.type != JsonValue::Type::Number)
                return (err = "expected number for '" + where + "'"), false;
            out = static_cast<float> (v.num);
            return true;
        }

        bool asColour (const JsonValue& v, std::uint32_t& out, const std::string& where, std::string& err)
        {
            if (v.type != JsonValue::Type::String)
                return (err = "expected colour string for '" + where + "'"), false;

            const std::string& s = v.str;
            if (s.empty() || s[0] != '#')
                return (err = "colour '" + where + "' must start with '#'"), false;

            const std::string hex = s.substr (1);
            if (hex.size() != 8 && hex.size() != 6)
                return (err = "colour '" + where + "' must be #rrggbb or #aarrggbb"), false;

            std::uint32_t value = 0;
            for (char c : hex)
            {
                int d;
                if      (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return (err = "colour '" + where + "' has a non-hex digit"), false;
                value = (value << 4) | static_cast<std::uint32_t> (d);
            }
            if (hex.size() == 6)
                value |= 0xff000000u; // opaque
            out = value;
            return true;
        }

        // Map an offset "[x, y]" array onto two float fields.
        bool asOffset (const JsonValue& v, float& x, float& y, const std::string& where, std::string& err)
        {
            if (v.type != JsonValue::Type::Array || v.arr.size() != 2)
                return (err = "expected [x, y] array for '" + where + "'"), false;
            return asNumber (v.arr[0], x, where + "[0]", err)
                && asNumber (v.arr[1], y, where + "[1]", err);
        }

        // Reject any object key not in the allow-list (strict schema).
        bool checkNoUnknownKeys (const JsonValue& obj, const std::vector<std::string>& allowed,
                                 const std::string& where, std::string& err)
        {
            for (const auto& kv : obj.obj)
            {
                bool ok = false;
                for (const auto& a : allowed)
                    if (kv.first == a) { ok = true; break; }
                if (! ok)
                    return (err = "unknown key '" + kv.first + "' in '" + where + "'"), false;
            }
            return true;
        }

        // Number formatter: shortest round-trippable form (integers stay integral).
        std::string num (float f)
        {
            char buf[48];
            std::snprintf (buf, sizeof buf, "%.9g", static_cast<double> (f));
            return buf;
        }

        std::string colour (std::uint32_t c)
        {
            char buf[16];
            std::snprintf (buf, sizeof buf, "\"#%08x\"", c);
            return buf;
        }
    } // namespace

    Theme Theme::defaults()
    {
        return Theme {};
    }

    namespace
    {
    // Apply a parsed theme document `root` onto `t`, overriding only the keys that
    // are present (every key optional). Shared by Theme::tryParse (seeded from
    // defaults()) and Theme::applyOverlay (seeded from the caller's theme). When
    // `allowPluginExtras` is true a top-level "rs" object is permitted and ignored
    // here — a plugin overlay carries its own extras under that key, consumed by
    // the plugin's own theme model (see plugins/*/ui/RsTheme).
    bool applyRootInto (const JsonValue& root, Theme& t, bool allowPluginExtras, std::string& error)
    {
        std::vector<std::string> topLevel { "palette", "knob", "toggle", "card", "font",
                                            "segmented", "dropdown", "iconButton", "valueSetting",
                                            "linkSlider", "spectrum" };
        if (allowPluginExtras)
            topLevel.push_back ("rs");
        if (! checkNoUnknownKeys (root, topLevel, "theme", error))
            return false;

        if (const JsonValue* p = root.find ("palette"))
        {
            if (p->type != JsonValue::Type::Object)
                return (error = "'palette' must be an object"), false;
            if (! checkNoUnknownKeys (*p, { "background", "backgroundLo", "panel", "panelLo", "track",
                                            "accent", "accentDim", "text", "textSecondary", "textDim",
                                            "shadow", "bandColours" }, "palette", error))
                return false;

            const auto col = [&] (const char* key, std::uint32_t& dst) -> bool
            {
                if (const JsonValue* v = p->find (key))
                    return asColour (*v, dst, std::string ("palette.") + key, error);
                return true;
            };
            if (! col ("background",   t.palette.background))   return false;
            if (! col ("backgroundLo", t.palette.backgroundLo)) return false;
            if (! col ("panel",        t.palette.panel))        return false;
            if (! col ("panelLo",      t.palette.panelLo))      return false;
            if (! col ("track",        t.palette.track))        return false;
            if (! col ("accent",       t.palette.accent))       return false;
            if (! col ("accentDim",    t.palette.accentDim))    return false;
            if (! col ("text",         t.palette.text))          return false;
            if (! col ("textSecondary",t.palette.textSecondary)) return false;
            if (! col ("textDim",      t.palette.textDim))       return false;
            if (! col ("shadow",       t.palette.shadow))       return false;

            if (const JsonValue* bc = p->find ("bandColours"))
            {
                if (bc->type != JsonValue::Type::Array || bc->arr.size() != 6)
                    return (error = "'palette.bandColours' must be an array of 6 colours"), false;
                for (std::size_t i = 0; i < 6; ++i)
                    if (! asColour (bc->arr[i], t.palette.bandColours[i],
                                    "palette.bandColours[" + std::to_string (i) + "]", error))
                        return false;
            }
        }

        if (const JsonValue* k = root.find ("knob"))
        {
            if (k->type != JsonValue::Type::Object)
                return (error = "'knob' must be an object"), false;
            if (! checkNoUnknownKeys (*k, { "boundsInset", "lineWidthRatio", "glowWidthFactor",
                                            "glowAlpha", "bodyInsetFactor", "shadowBlurFactor",
                                            "shadowOffset", "pointerDotFactor", "pointerPosFactor",
                                            "arcStart", "arcEnd" }, "knob", error))
                return false;

            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = k->find (key))
                    return asNumber (*v, dst, std::string ("knob.") + key, error);
                return true;
            };
            if (! n ("boundsInset",      t.knob.boundsInset))      return false;
            if (! n ("lineWidthRatio",   t.knob.lineWidthRatio))   return false;
            if (! n ("glowWidthFactor",  t.knob.glowWidthFactor))  return false;
            if (! n ("glowAlpha",        t.knob.glowAlpha))        return false;
            if (! n ("bodyInsetFactor",  t.knob.bodyInsetFactor))  return false;
            if (! n ("shadowBlurFactor", t.knob.shadowBlurFactor)) return false;
            if (! n ("pointerDotFactor", t.knob.pointerDotFactor)) return false;
            if (! n ("pointerPosFactor", t.knob.pointerPosFactor)) return false;
            if (! n ("arcStart",         t.knob.arcStart))         return false;
            if (! n ("arcEnd",           t.knob.arcEnd))           return false;
            if (const JsonValue* off = k->find ("shadowOffset"))
                if (! asOffset (*off, t.knob.shadowOffsetX, t.knob.shadowOffsetY, "knob.shadowOffset", error))
                    return false;
        }

        if (const JsonValue* g = root.find ("toggle"))
        {
            if (g->type != JsonValue::Type::Object)
                return (error = "'toggle' must be an object"), false;
            if (! checkNoUnknownKeys (*g, { "height", "widthFactor", "knobInset",
                                            "cornerRadiusFactor", "textGap" }, "toggle", error))
                return false;

            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = g->find (key))
                    return asNumber (*v, dst, std::string ("toggle.") + key, error);
                return true;
            };
            if (! n ("height",             t.toggle.height))             return false;
            if (! n ("widthFactor",        t.toggle.widthFactor))        return false;
            if (! n ("knobInset",          t.toggle.knobInset))          return false;
            if (! n ("cornerRadiusFactor", t.toggle.cornerRadiusFactor)) return false;
            if (! n ("textGap",            t.toggle.textGap))            return false;
        }

        if (const JsonValue* c = root.find ("card"))
        {
            if (c->type != JsonValue::Type::Object)
                return (error = "'card' must be an object"), false;
            if (! checkNoUnknownKeys (*c, { "cornerRadius", "outlineWidth", "shadowBlur",
                                            "shadowOffset" }, "card", error))
                return false;

            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = c->find (key))
                    return asNumber (*v, dst, std::string ("card.") + key, error);
                return true;
            };
            if (! n ("cornerRadius", t.card.cornerRadius)) return false;
            if (! n ("outlineWidth", t.card.outlineWidth)) return false;
            if (! n ("shadowBlur",   t.card.shadowBlur))   return false;
            if (const JsonValue* off = c->find ("shadowOffset"))
                if (! asOffset (*off, t.card.shadowOffsetX, t.card.shadowOffsetY, "card.shadowOffset", error))
                    return false;
        }

        if (const JsonValue* f = root.find ("font"))
        {
            if (f->type != JsonValue::Type::Object)
                return (error = "'font' must be an object"), false;
            if (! checkNoUnknownKeys (*f, { "label", "labelBold", "title", "callout", "caption" }, "font", error))
                return false;

            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = f->find (key))
                    return asNumber (*v, dst, std::string ("font.") + key, error);
                return true;
            };
            if (! n ("label",     t.font.label))     return false;
            if (! n ("labelBold", t.font.labelBold)) return false;
            if (! n ("title",     t.font.title))     return false;
            if (! n ("callout",   t.font.callout))   return false;
            if (! n ("caption",   t.font.caption))   return false;
        }

        if (const JsonValue* s = root.find ("segmented"))
        {
            if (s->type != JsonValue::Type::Object)
                return (error = "'segmented' must be an object"), false;
            if (! checkNoUnknownKeys (*s, { "height", "cornerRadius", "pillInset",
                                            "pillCornerRadius" }, "segmented", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = s->find (key))
                    return asNumber (*v, dst, std::string ("segmented.") + key, error);
                return true;
            };
            if (! n ("height",           t.segmented.height))           return false;
            if (! n ("cornerRadius",     t.segmented.cornerRadius))     return false;
            if (! n ("pillInset",        t.segmented.pillInset))        return false;
            if (! n ("pillCornerRadius", t.segmented.pillCornerRadius)) return false;
        }

        if (const JsonValue* d = root.find ("dropdown"))
        {
            if (d->type != JsonValue::Type::Object)
                return (error = "'dropdown' must be an object"), false;
            if (! checkNoUnknownKeys (*d, { "rowHeight", "cornerRadius", "paddingX", "paddingY",
                                            "separatorInset", "shadowBlur", "shadowOffsetY" }, "dropdown", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = d->find (key))
                    return asNumber (*v, dst, std::string ("dropdown.") + key, error);
                return true;
            };
            if (! n ("rowHeight",      t.dropdown.rowHeight))      return false;
            if (! n ("cornerRadius",   t.dropdown.cornerRadius))   return false;
            if (! n ("paddingX",       t.dropdown.paddingX))       return false;
            if (! n ("paddingY",       t.dropdown.paddingY))       return false;
            if (! n ("separatorInset", t.dropdown.separatorInset)) return false;
            if (! n ("shadowBlur",     t.dropdown.shadowBlur))     return false;
            if (! n ("shadowOffsetY",  t.dropdown.shadowOffsetY))  return false;
        }

        if (const JsonValue* b = root.find ("iconButton"))
        {
            if (b->type != JsonValue::Type::Object)
                return (error = "'iconButton' must be an object"), false;
            if (! checkNoUnknownKeys (*b, { "cornerRadius", "glyphInsetFactor" }, "iconButton", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = b->find (key))
                    return asNumber (*v, dst, std::string ("iconButton.") + key, error);
                return true;
            };
            if (! n ("cornerRadius",     t.iconButton.cornerRadius))     return false;
            if (! n ("glyphInsetFactor", t.iconButton.glyphInsetFactor)) return false;
        }

        if (const JsonValue* vs = root.find ("valueSetting"))
        {
            if (vs->type != JsonValue::Type::Object)
                return (error = "'valueSetting' must be an object"), false;
            if (! checkNoUnknownKeys (*vs, { "cornerRadius", "paddingX", "iconSize" }, "valueSetting", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = vs->find (key))
                    return asNumber (*v, dst, std::string ("valueSetting.") + key, error);
                return true;
            };
            if (! n ("cornerRadius", t.valueSetting.cornerRadius)) return false;
            if (! n ("paddingX",     t.valueSetting.paddingX))     return false;
            if (! n ("iconSize",     t.valueSetting.iconSize))     return false;
        }

        if (const JsonValue* ls = root.find ("linkSlider"))
        {
            if (ls->type != JsonValue::Type::Object)
                return (error = "'linkSlider' must be an object"), false;
            if (! checkNoUnknownKeys (*ls, { "cornerRadius", "paddingX", "trackHeight", "trackCorner",
                                             "captionColumn", "valueColumn", "glyphSize" }, "linkSlider", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = ls->find (key))
                    return asNumber (*v, dst, std::string ("linkSlider.") + key, error);
                return true;
            };
            if (! n ("cornerRadius",  t.linkSlider.cornerRadius))  return false;
            if (! n ("paddingX",      t.linkSlider.paddingX))      return false;
            if (! n ("trackHeight",   t.linkSlider.trackHeight))   return false;
            if (! n ("trackCorner",   t.linkSlider.trackCorner))   return false;
            if (! n ("captionColumn", t.linkSlider.captionColumn)) return false;
            if (! n ("valueColumn",   t.linkSlider.valueColumn))   return false;
            if (! n ("glyphSize",     t.linkSlider.glyphSize))     return false;
        }

        if (const JsonValue* sp = root.find ("spectrum"))
        {
            if (sp->type != JsonValue::Type::Object)
                return (error = "'spectrum' must be an object"), false;
            if (! checkNoUnknownKeys (*sp, { "cornerRadius", "topDb", "bottomDb", "traceWidth",
                                             "peakWidth", "fillTopAlpha", "fillBottomAlpha" }, "spectrum", error))
                return false;
            const auto n = [&] (const char* key, float& dst) -> bool
            {
                if (const JsonValue* v = sp->find (key))
                    return asNumber (*v, dst, std::string ("spectrum.") + key, error);
                return true;
            };
            if (! n ("cornerRadius",    t.spectrum.cornerRadius))    return false;
            if (! n ("topDb",           t.spectrum.topDb))           return false;
            if (! n ("bottomDb",        t.spectrum.bottomDb))        return false;
            if (! n ("traceWidth",      t.spectrum.traceWidth))      return false;
            if (! n ("peakWidth",       t.spectrum.peakWidth))       return false;
            if (! n ("fillTopAlpha",    t.spectrum.fillTopAlpha))    return false;
            if (! n ("fillBottomAlpha", t.spectrum.fillBottomAlpha)) return false;
        }

        return true;
    }
    } // namespace

    bool Theme::tryParse (const std::string& jsonText, Theme& out, std::string& error)
    {
        JsonParser parser (jsonText);
        JsonValue root;
        if (! parser.parse (root))
        {
            error = parser.error();
            return false;
        }
        if (root.type != JsonValue::Type::Object)
        {
            error = "theme document must be a JSON object";
            return false;
        }
        Theme t = Theme::defaults();
        if (! applyRootInto (root, t, /*allowPluginExtras*/ false, error))
            return false;
        out = t;
        return true;
    }

    bool Theme::applyOverlay (const std::string& jsonText, std::string& error)
    {
        JsonParser parser (jsonText);
        JsonValue root;
        if (! parser.parse (root))
        {
            error = parser.error();
            return false;
        }
        if (root.type != JsonValue::Type::Object)
        {
            error = "theme overlay must be a JSON object";
            return false;
        }
        Theme t = *this;
        if (! applyRootInto (root, t, /*allowPluginExtras*/ true, error))
            return false;
        *this = t;
        return true;
    }

    Theme Theme::fromJsonText (const std::string& jsonText)
    {
        Theme t;
        std::string error;
        if (! tryParse (jsonText, t, error))
            throw std::runtime_error (error);
        return t;
    }

    Theme Theme::fromJsonFile (const std::string& path)
    {
        std::ifstream in (path, std::ios::binary);
        if (! in)
            throw std::runtime_error ("could not open theme file: " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return fromJsonText (ss.str());
    }

    std::string Theme::toJson() const
    {
        std::ostringstream o;
        o << "{\n";
        o << "  \"palette\": {\n";
        o << "    \"background\": "   << colour (palette.background)   << ",\n";
        o << "    \"backgroundLo\": " << colour (palette.backgroundLo) << ",\n";
        o << "    \"panel\": "        << colour (palette.panel)        << ",\n";
        o << "    \"panelLo\": "      << colour (palette.panelLo)      << ",\n";
        o << "    \"track\": "        << colour (palette.track)        << ",\n";
        o << "    \"accent\": "       << colour (palette.accent)       << ",\n";
        o << "    \"accentDim\": "    << colour (palette.accentDim)    << ",\n";
        o << "    \"text\": "          << colour (palette.text)          << ",\n";
        o << "    \"textSecondary\": " << colour (palette.textSecondary) << ",\n";
        o << "    \"textDim\": "        << colour (palette.textDim)       << ",\n";
        o << "    \"shadow\": "       << colour (palette.shadow)       << ",\n";
        o << "    \"bandColours\": [";
        for (std::size_t i = 0; i < palette.bandColours.size(); ++i)
            o << (i ? ", " : "") << colour (palette.bandColours[i]);
        o << "]\n  },\n";

        o << "  \"knob\": {\n";
        o << "    \"boundsInset\": "      << num (knob.boundsInset)      << ",\n";
        o << "    \"lineWidthRatio\": "   << num (knob.lineWidthRatio)   << ",\n";
        o << "    \"glowWidthFactor\": "  << num (knob.glowWidthFactor)  << ",\n";
        o << "    \"glowAlpha\": "        << num (knob.glowAlpha)        << ",\n";
        o << "    \"bodyInsetFactor\": "  << num (knob.bodyInsetFactor)  << ",\n";
        o << "    \"shadowBlurFactor\": " << num (knob.shadowBlurFactor) << ",\n";
        o << "    \"shadowOffset\": [" << num (knob.shadowOffsetX) << ", " << num (knob.shadowOffsetY) << "],\n";
        o << "    \"pointerDotFactor\": " << num (knob.pointerDotFactor) << ",\n";
        o << "    \"pointerPosFactor\": " << num (knob.pointerPosFactor) << ",\n";
        o << "    \"arcStart\": "         << num (knob.arcStart)         << ",\n";
        o << "    \"arcEnd\": "           << num (knob.arcEnd)           << "\n  },\n";

        o << "  \"toggle\": {\n";
        o << "    \"height\": "             << num (toggle.height)             << ",\n";
        o << "    \"widthFactor\": "        << num (toggle.widthFactor)        << ",\n";
        o << "    \"knobInset\": "          << num (toggle.knobInset)          << ",\n";
        o << "    \"cornerRadiusFactor\": " << num (toggle.cornerRadiusFactor) << ",\n";
        o << "    \"textGap\": "            << num (toggle.textGap)            << "\n  },\n";

        o << "  \"card\": {\n";
        o << "    \"cornerRadius\": " << num (card.cornerRadius) << ",\n";
        o << "    \"outlineWidth\": " << num (card.outlineWidth) << ",\n";
        o << "    \"shadowBlur\": "   << num (card.shadowBlur)   << ",\n";
        o << "    \"shadowOffset\": [" << num (card.shadowOffsetX) << ", " << num (card.shadowOffsetY) << "]\n  },\n";

        o << "  \"font\": {\n";
        o << "    \"label\": "     << num (font.label)     << ",\n";
        o << "    \"labelBold\": " << num (font.labelBold) << ",\n";
        o << "    \"title\": "     << num (font.title)     << ",\n";
        o << "    \"callout\": "   << num (font.callout)   << ",\n";
        o << "    \"caption\": "   << num (font.caption)   << "\n  },\n";

        o << "  \"segmented\": {\n";
        o << "    \"height\": "           << num (segmented.height)           << ",\n";
        o << "    \"cornerRadius\": "     << num (segmented.cornerRadius)     << ",\n";
        o << "    \"pillInset\": "        << num (segmented.pillInset)        << ",\n";
        o << "    \"pillCornerRadius\": " << num (segmented.pillCornerRadius) << "\n  },\n";

        o << "  \"dropdown\": {\n";
        o << "    \"rowHeight\": "      << num (dropdown.rowHeight)      << ",\n";
        o << "    \"cornerRadius\": "   << num (dropdown.cornerRadius)   << ",\n";
        o << "    \"paddingX\": "       << num (dropdown.paddingX)       << ",\n";
        o << "    \"paddingY\": "       << num (dropdown.paddingY)       << ",\n";
        o << "    \"separatorInset\": " << num (dropdown.separatorInset) << ",\n";
        o << "    \"shadowBlur\": "     << num (dropdown.shadowBlur)     << ",\n";
        o << "    \"shadowOffsetY\": "  << num (dropdown.shadowOffsetY)  << "\n  },\n";

        o << "  \"iconButton\": {\n";
        o << "    \"cornerRadius\": "     << num (iconButton.cornerRadius)     << ",\n";
        o << "    \"glyphInsetFactor\": " << num (iconButton.glyphInsetFactor) << "\n  },\n";

        o << "  \"valueSetting\": {\n";
        o << "    \"cornerRadius\": " << num (valueSetting.cornerRadius) << ",\n";
        o << "    \"paddingX\": "     << num (valueSetting.paddingX)     << ",\n";
        o << "    \"iconSize\": "     << num (valueSetting.iconSize)     << "\n  },\n";

        o << "  \"linkSlider\": {\n";
        o << "    \"cornerRadius\": "  << num (linkSlider.cornerRadius)  << ",\n";
        o << "    \"paddingX\": "      << num (linkSlider.paddingX)      << ",\n";
        o << "    \"trackHeight\": "   << num (linkSlider.trackHeight)   << ",\n";
        o << "    \"trackCorner\": "   << num (linkSlider.trackCorner)   << ",\n";
        o << "    \"captionColumn\": " << num (linkSlider.captionColumn) << ",\n";
        o << "    \"valueColumn\": "   << num (linkSlider.valueColumn)   << ",\n";
        o << "    \"glyphSize\": "     << num (linkSlider.glyphSize)     << "\n  },\n";

        o << "  \"spectrum\": {\n";
        o << "    \"cornerRadius\": "    << num (spectrum.cornerRadius)    << ",\n";
        o << "    \"topDb\": "           << num (spectrum.topDb)           << ",\n";
        o << "    \"bottomDb\": "        << num (spectrum.bottomDb)        << ",\n";
        o << "    \"traceWidth\": "      << num (spectrum.traceWidth)      << ",\n";
        o << "    \"peakWidth\": "       << num (spectrum.peakWidth)       << ",\n";
        o << "    \"fillTopAlpha\": "    << num (spectrum.fillTopAlpha)    << ",\n";
        o << "    \"fillBottomAlpha\": " << num (spectrum.fillBottomAlpha) << "\n  }\n";
        o << "}\n";
        return o.str();
    }

    bool Theme::operator== (const Theme& o) const
    {
        return palette.background == o.palette.background
            && palette.backgroundLo == o.palette.backgroundLo
            && palette.panel == o.palette.panel
            && palette.panelLo == o.palette.panelLo
            && palette.track == o.palette.track
            && palette.accent == o.palette.accent
            && palette.accentDim == o.palette.accentDim
            && palette.text == o.palette.text
            && palette.textSecondary == o.palette.textSecondary
            && palette.textDim == o.palette.textDim
            && palette.shadow == o.palette.shadow
            && palette.bandColours == o.palette.bandColours
            && knob.boundsInset == o.knob.boundsInset
            && knob.lineWidthRatio == o.knob.lineWidthRatio
            && knob.glowWidthFactor == o.knob.glowWidthFactor
            && knob.glowAlpha == o.knob.glowAlpha
            && knob.bodyInsetFactor == o.knob.bodyInsetFactor
            && knob.shadowBlurFactor == o.knob.shadowBlurFactor
            && knob.shadowOffsetX == o.knob.shadowOffsetX
            && knob.shadowOffsetY == o.knob.shadowOffsetY
            && knob.pointerDotFactor == o.knob.pointerDotFactor
            && knob.pointerPosFactor == o.knob.pointerPosFactor
            && knob.arcStart == o.knob.arcStart
            && knob.arcEnd == o.knob.arcEnd
            && toggle.height == o.toggle.height
            && toggle.widthFactor == o.toggle.widthFactor
            && toggle.knobInset == o.toggle.knobInset
            && toggle.cornerRadiusFactor == o.toggle.cornerRadiusFactor
            && toggle.textGap == o.toggle.textGap
            && card.cornerRadius == o.card.cornerRadius
            && card.outlineWidth == o.card.outlineWidth
            && card.shadowBlur == o.card.shadowBlur
            && card.shadowOffsetX == o.card.shadowOffsetX
            && card.shadowOffsetY == o.card.shadowOffsetY
            && font.label == o.font.label
            && font.labelBold == o.font.labelBold
            && font.title == o.font.title
            && font.callout == o.font.callout
            && font.caption == o.font.caption
            && segmented.height == o.segmented.height
            && segmented.cornerRadius == o.segmented.cornerRadius
            && segmented.pillInset == o.segmented.pillInset
            && segmented.pillCornerRadius == o.segmented.pillCornerRadius
            && dropdown.rowHeight == o.dropdown.rowHeight
            && dropdown.cornerRadius == o.dropdown.cornerRadius
            && dropdown.paddingX == o.dropdown.paddingX
            && dropdown.paddingY == o.dropdown.paddingY
            && dropdown.separatorInset == o.dropdown.separatorInset
            && dropdown.shadowBlur == o.dropdown.shadowBlur
            && dropdown.shadowOffsetY == o.dropdown.shadowOffsetY
            && iconButton.cornerRadius == o.iconButton.cornerRadius
            && iconButton.glyphInsetFactor == o.iconButton.glyphInsetFactor
            && valueSetting.cornerRadius == o.valueSetting.cornerRadius
            && valueSetting.paddingX == o.valueSetting.paddingX
            && valueSetting.iconSize == o.valueSetting.iconSize
            && linkSlider.cornerRadius == o.linkSlider.cornerRadius
            && linkSlider.paddingX == o.linkSlider.paddingX
            && linkSlider.trackHeight == o.linkSlider.trackHeight
            && linkSlider.trackCorner == o.linkSlider.trackCorner
            && linkSlider.captionColumn == o.linkSlider.captionColumn
            && linkSlider.valueColumn == o.linkSlider.valueColumn
            && linkSlider.glyphSize == o.linkSlider.glyphSize
            && spectrum.cornerRadius == o.spectrum.cornerRadius
            && spectrum.topDb == o.spectrum.topDb
            && spectrum.bottomDb == o.spectrum.bottomDb
            && spectrum.traceWidth == o.spectrum.traceWidth
            && spectrum.peakWidth == o.spectrum.peakWidth
            && spectrum.fillTopAlpha == o.spectrum.fillTopAlpha
            && spectrum.fillBottomAlpha == o.spectrum.fillBottomAlpha;
    }
}
