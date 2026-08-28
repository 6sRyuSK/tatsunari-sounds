#include "factory_update/UpdatePrefs.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace factory_update
{
    namespace
    {
        std::filesystem::path appSupportRoot()
        {
#if defined(_WIN32)
            if (const char* appdata = std::getenv ("APPDATA"); appdata && *appdata)
                return std::filesystem::path (appdata) / "tatsunari-sounds";
#elif defined(__APPLE__)
            if (const char* home = std::getenv ("HOME"); home && *home)
                return std::filesystem::path (home) / "Library" / "Application Support" / "tatsunari-sounds";
#else
            if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg && *xdg)
                return std::filesystem::path (xdg) / "tatsunari-sounds";
            if (const char* home = std::getenv ("HOME"); home && *home)
                return std::filesystem::path (home) / ".config" / "tatsunari-sounds";
#endif
            return std::filesystem::temp_directory_path() / "tatsunari-sounds";
        }

        // Minimal JSON writer/reader for the prefs shape (no nested objects beyond
        // a string array). Kept local so UpdatePrefs stays free of Theme/visage.
        std::string escape (const std::string& s)
        {
            std::string o;
            o.reserve (s.size() + 8);
            for (char c : s)
            {
                switch (c)
                {
                    case '"':  o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n"; break;
                    case '\r': o += "\\r"; break;
                    case '\t': o += "\\t"; break;
                    default:   o.push_back (c); break;
                }
            }
            return o;
        }

        bool extractBool (const std::string& json, const char* key, bool& out)
        {
            const std::string pat = std::string ("\"") + key + "\"";
            auto pos = json.find (pat);
            if (pos == std::string::npos) return false;
            pos = json.find (':', pos);
            if (pos == std::string::npos) return false;
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
            if (json.compare (pos, 4, "true") == 0)  { out = true;  return true; }
            if (json.compare (pos, 5, "false") == 0) { out = false; return true; }
            return false;
        }

        bool extractInt64 (const std::string& json, const char* key, std::int64_t& out)
        {
            const std::string pat = std::string ("\"") + key + "\"";
            auto pos = json.find (pat);
            if (pos == std::string::npos) return false;
            pos = json.find (':', pos);
            if (pos == std::string::npos) return false;
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
            try
            {
                std::size_t n = 0;
                out = std::stoll (json.substr (pos), &n);
                return n > 0;
            }
            catch (...) { return false; }
        }

        bool extractString (const std::string& json, const char* key, std::string& out)
        {
            const std::string pat = std::string ("\"") + key + "\"";
            auto pos = json.find (pat);
            if (pos == std::string::npos) return false;
            pos = json.find (':', pos);
            if (pos == std::string::npos) return false;
            pos = json.find ('"', pos);
            if (pos == std::string::npos) return false;
            ++pos;
            std::string v;
            while (pos < json.size())
            {
                char c = json[pos++];
                if (c == '"') { out = std::move (v); return true; }
                if (c == '\\' && pos < json.size())
                {
                    char e = json[pos++];
                    if (e == 'n') v.push_back ('\n');
                    else if (e == 'r') v.push_back ('\r');
                    else if (e == 't') v.push_back ('\t');
                    else v.push_back (e);
                }
                else
                    v.push_back (c);
            }
            return false;
        }

        bool extractStringArray (const std::string& json, const char* key, std::vector<std::string>& out)
        {
            const std::string pat = std::string ("\"") + key + "\"";
            auto pos = json.find (pat);
            if (pos == std::string::npos) return false;
            pos = json.find ('[', pos);
            if (pos == std::string::npos) return false;
            ++pos;
            out.clear();
            while (pos < json.size())
            {
                while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\n'
                                             || json[pos] == '\r' || json[pos] == '\t'))
                    ++pos;
                if (pos < json.size() && json[pos] == ']')
                    return true;
                if (pos >= json.size() || json[pos] != '"')
                    return false;
                ++pos;
                std::string v;
                while (pos < json.size())
                {
                    char c = json[pos++];
                    if (c == '"') break;
                    if (c == '\\' && pos < json.size()) v.push_back (json[pos++]);
                    else v.push_back (c);
                }
                out.push_back (std::move (v));
            }
            return false;
        }
    } // namespace

    std::filesystem::path defaultUpdatePrefsPath()
    {
        return appSupportRoot() / "update-prefs.json";
    }

    bool loadUpdatePrefs (const std::filesystem::path& path, UpdatePrefs& out)
    {
        std::error_code ec;
        if (! std::filesystem::exists (path, ec))
            return false;
        std::ifstream in (path);
        if (! in)
            return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string json = ss.str();
        UpdatePrefs p;
        extractBool (json, "optedIn", p.optedIn);
        extractString (json, "etag", p.etag);
        extractInt64 (json, "lastSuccessUnix", p.lastSuccessUnix);
        extractString (json, "cachedSlug", p.cachedSlug);
        extractString (json, "cachedLatest", p.cachedLatest);
        extractStringArray (json, "cachedHighlights", p.cachedHighlights);
        extractString (json, "cachedChangelogUrl", p.cachedChangelogUrl);
        out = std::move (p);
        return true;
    }

    bool saveUpdatePrefs (const std::filesystem::path& path, const UpdatePrefs& prefs)
    {
        std::error_code ec;
        std::filesystem::create_directories (path.parent_path(), ec);
        std::ostringstream body;
        body << "{\n"
             << "  \"optedIn\": " << (prefs.optedIn ? "true" : "false") << ",\n"
             << "  \"etag\": \"" << escape (prefs.etag) << "\",\n"
             << "  \"lastSuccessUnix\": " << prefs.lastSuccessUnix << ",\n"
             << "  \"cachedSlug\": \"" << escape (prefs.cachedSlug) << "\",\n"
             << "  \"cachedLatest\": \"" << escape (prefs.cachedLatest) << "\",\n"
             << "  \"cachedHighlights\": [";
        for (std::size_t i = 0; i < prefs.cachedHighlights.size(); ++i)
        {
            if (i) body << ", ";
            body << "\"" << escape (prefs.cachedHighlights[i]) << "\"";
        }
        body << "],\n"
             << "  \"cachedChangelogUrl\": \"" << escape (prefs.cachedChangelogUrl) << "\"\n"
             << "}\n";

        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
            if (! out)
                return false;
            out << body.str();
            out.flush();
            if (! out)
                return false;
        }
        std::filesystem::rename (tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove (path, ec);
            std::filesystem::rename (tmp, path, ec);
        }
        return ! ec;
    }
}
