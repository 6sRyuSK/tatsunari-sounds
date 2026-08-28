#pragma once
//
// User-scoped update preferences (opt-in, ETag, last success, cached latest).
// NEVER stored in plugin state — lives under Application Support / APPDATA
// next to the installer receipt (plan §1).
//
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace factory_update
{
    struct UpdatePrefs
    {
        bool optedIn = false;
        std::string etag;
        std::int64_t lastSuccessUnix = 0; // UTC seconds; 0 = never
        std::string cachedSlug;
        std::string cachedLatest;
        std::vector<std::string> cachedHighlights;
        std::string cachedChangelogUrl;
    };

    // ~/Library/Application Support/tatsunari-sounds/update-prefs.json  (macOS)
    // %APPDATA%\tatsunari-sounds\update-prefs.json                       (Windows)
    // $XDG_CONFIG_HOME/tatsunari-sounds/update-prefs.json                (Linux/tests)
    std::filesystem::path defaultUpdatePrefsPath();

    bool loadUpdatePrefs (const std::filesystem::path& path, UpdatePrefs& out);
    bool saveUpdatePrefs (const std::filesystem::path& path, const UpdatePrefs& prefs);
}
