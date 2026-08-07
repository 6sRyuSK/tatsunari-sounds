#include "factory_update/LatestDocument.h"
#include "factory_update/Urls.h"

#include <cctype>
#include <utility>

namespace factory_update
{
    namespace
    {
        struct JsonValue
        {
            enum class Type { Null, Bool, Number, String, Array, Object };
            Type type = Type::Null;
            bool b = false;
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

        class JsonParser
        {
        public:
            explicit JsonParser (const std::string& text) : s_ (text) {}

            bool parse (JsonValue& out)
            {
                skip();
                if (! parseValue (out))
                    return false;
                skip();
                if (i_ != s_.size())
                {
                    error_ = "trailing junk after JSON value";
                    return false;
                }
                return true;
            }

            const std::string& error() const { return error_; }

        private:
            void fail (const std::string& msg)
            {
                error_ = "JSON parse error at offset " + std::to_string (i_) + ": " + msg;
            }

            void skip()
            {
                while (i_ < s_.size()
                       && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
                    ++i_;
            }

            bool parseValue (JsonValue& out)
            {
                skip();
                if (i_ >= s_.size()) { fail ("unexpected end"); return false; }
                switch (s_[i_])
                {
                    case '{': return parseObject (out);
                    case '[': return parseArray (out);
                    case '"': out.type = JsonValue::Type::String; return parseString (out.str);
                    case 't': case 'f': return parseBool (out);
                    case 'n': return parseNull (out);
                    default:
                        if (s_[i_] == '-' || std::isdigit (static_cast<unsigned char> (s_[i_])))
                            return parseNumber (out);
                        fail ("unexpected character");
                        return false;
                }
            }

            bool parseObject (JsonValue& out)
            {
                out.type = JsonValue::Type::Object;
                ++i_; // '{'
                skip();
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                for (;;)
                {
                    skip();
                    std::string key;
                    if (! parseString (key))
                        return false;
                    skip();
                    if (i_ >= s_.size() || s_[i_] != ':') { fail ("expected ':'"); return false; }
                    ++i_;
                    JsonValue value;
                    if (! parseValue (value))
                        return false;
                    out.obj.emplace_back (std::move (key), std::move (value));
                    skip();
                    if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                    fail ("expected ',' or '}'");
                    return false;
                }
            }

            bool parseArray (JsonValue& out)
            {
                out.type = JsonValue::Type::Array;
                ++i_;
                skip();
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                for (;;)
                {
                    JsonValue value;
                    if (! parseValue (value))
                        return false;
                    out.arr.push_back (std::move (value));
                    skip();
                    if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                    fail ("expected ',' or ']'");
                    return false;
                }
            }

            bool parseString (std::string& out)
            {
                if (i_ >= s_.size() || s_[i_] != '"') { fail ("expected string"); return false; }
                ++i_;
                out.clear();
                while (i_ < s_.size())
                {
                    char c = s_[i_++];
                    if (c == '"')
                        return true;
                    if (c == '\\')
                    {
                        if (i_ >= s_.size()) { fail ("truncated escape"); return false; }
                        char e = s_[i_++];
                        switch (e)
                        {
                            case '"': case '\\': case '/': out.push_back (e); break;
                            case 'b': out.push_back ('\b'); break;
                            case 'f': out.push_back ('\f'); break;
                            case 'n': out.push_back ('\n'); break;
                            case 'r': out.push_back ('\r'); break;
                            case 't': out.push_back ('\t'); break;
                            case 'u':
                                // Accept \uXXXX as four hex digits; emit '?'.
                                if (i_ + 4 > s_.size()) { fail ("bad unicode escape"); return false; }
                                i_ += 4;
                                out.push_back ('?');
                                break;
                            default: fail ("bad escape"); return false;
                        }
                    }
                    else
                        out.push_back (c);
                }
                fail ("unterminated string");
                return false;
            }

            bool parseNumber (JsonValue& out)
            {
                const std::size_t start = i_;
                if (s_[i_] == '-') ++i_;
                while (i_ < s_.size() && std::isdigit (static_cast<unsigned char> (s_[i_]))) ++i_;
                if (i_ < s_.size() && s_[i_] == '.')
                {
                    ++i_;
                    while (i_ < s_.size() && std::isdigit (static_cast<unsigned char> (s_[i_]))) ++i_;
                }
                if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E'))
                {
                    ++i_;
                    if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
                    while (i_ < s_.size() && std::isdigit (static_cast<unsigned char> (s_[i_]))) ++i_;
                }
                try { out.num = std::stod (s_.substr (start, i_ - start)); }
                catch (...) { fail ("bad number"); return false; }
                out.type = JsonValue::Type::Number;
                return true;
            }

            bool parseBool (JsonValue& out)
            {
                if (s_.compare (i_, 4, "true") == 0)
                { i_ += 4; out.type = JsonValue::Type::Bool; out.b = true; return true; }
                if (s_.compare (i_, 5, "false") == 0)
                { i_ += 5; out.type = JsonValue::Type::Bool; out.b = false; return true; }
                fail ("bad bool");
                return false;
            }

            bool parseNull (JsonValue& out)
            {
                if (s_.compare (i_, 4, "null") == 0)
                { i_ += 4; out.type = JsonValue::Type::Null; return true; }
                fail ("bad null");
                return false;
            }

            const std::string& s_;
            std::size_t i_ = 0;
            std::string error_;
        };
    } // namespace

    bool parseLatestDocument (const std::string& json, LatestDocument& out, std::string& error)
    {
        if (json.size() > kMaxLatestBytes)
        {
            error = "latest.json exceeds size limit";
            return false;
        }
        JsonParser p (json);
        JsonValue root;
        if (! p.parse (root) || root.type != JsonValue::Type::Object)
        {
            error = p.error().empty() ? "root must be object" : p.error();
            return false;
        }
        const JsonValue* schema = root.find ("schema");
        const JsonValue* generated = root.find ("generated");
        const JsonValue* plugins = root.find ("plugins");
        if (schema == nullptr || schema->type != JsonValue::Type::Number
            || static_cast<int> (schema->num) != kSchemaVersion)
        {
            error = "schema must be 1";
            return false;
        }
        if (generated == nullptr || generated->type != JsonValue::Type::String || generated->str.empty())
        {
            error = "generated is required";
            return false;
        }
        if (plugins == nullptr || plugins->type != JsonValue::Type::Array)
        {
            error = "plugins must be an array";
            return false;
        }

        LatestDocument doc;
        doc.schema = kSchemaVersion;
        doc.generated = generated->str;
        for (const auto& row : plugins->arr)
        {
            if (row.type != JsonValue::Type::Object)
                continue;
            const JsonValue* slug = row.find ("slug");
            const JsonValue* latest = row.find ("latest");
            if (slug == nullptr || slug->type != JsonValue::Type::String || slug->str.empty())
                continue;
            if (latest == nullptr || latest->type != JsonValue::Type::String || latest->str.empty())
                continue;
            LatestPlugin plug;
            plug.slug = slug->str;
            plug.latest = latest->str;
            if (const JsonValue* hl = row.find ("highlights");
                hl != nullptr && hl->type == JsonValue::Type::Array)
            {
                for (const auto& h : hl->arr)
                    if (h.type == JsonValue::Type::String)
                        plug.highlights.push_back (h.str);
            }
            if (const JsonValue* cu = row.find ("changelogUrl");
                cu != nullptr && cu->type == JsonValue::Type::String)
                plug.changelogUrl = cu->str;
            doc.plugins.push_back (std::move (plug));
        }
        out = std::move (doc);
        error.clear();
        return true;
    }

    const LatestPlugin* findPlugin (const LatestDocument& doc, const std::string& slug)
    {
        for (const auto& p : doc.plugins)
            if (p.slug == slug)
                return &p;
        return nullptr;
    }
}
