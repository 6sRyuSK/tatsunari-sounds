#pragma once
//
// factory_presets/UserPresetStoreFs.h — the std::filesystem successor to
// UserPresetStore.h, with NO framework dependency (the original rode the
// framework's File / XmlElement). It persists opaque per-preset state blobs (the
// bytes produced by StateCodec::encode) one file each, so end users can save and
// recall their own configurations alongside the read-only factory bank.
//
// Message-thread only: every call does synchronous, direct file I/O — never call
// it from the audio thread. There is no cache; list() re-enumerates each time.
//
// LAYOUT — <root>/TatsunariSounds/<PluginName>/Presets/<name>.state, matching the
// original per-plugin flat directory. The OS root mirrors the framework's
// user-application-data location:
//   * Windows: %APPDATA%
//   * macOS:   ~/Library/Application Support
//   * Linux:   $XDG_CONFIG_HOME, else ~/.config
// (falling back to the system temp directory if the environment is unset).
//
// FILENAME SANITISATION — sanitizeFileName() is a createLegalFileName-equivalent.
// DOCUMENTED DIVERGENCES from the framework's createLegalFileName:
//   * Removes the SAME illegal set  "#@,;:<>*^|?\/  (characters are deleted, not
//     replaced), so a name round-trips through the same rule on save and load.
//   * ADDITIONALLY strips ASCII control characters (< 0x20) — the framework keeps
//     them; dropping them avoids un-listable / unsafe on-disk names.
//   * TRIMS leading/trailing spaces AND dots — the framework does not trim (its
//     caller trimmed separately); folding it in also guards hidden/"..''-style
//     names. Consequence: a name that is only spaces/dots/illegal chars sanitises
//     to empty and every op fails soft (returns false / nullopt).
//   * Truncates to 128 characters with a plain cut — the framework preserves a
//     trailing extension across truncation; that refinement is dropped (user
//     preset names are short in practice).
//   * No special handling of Windows reserved device names (CON, PRN, …) — same
//     gap as the framework.
// As with the original, two names that sanitise to the same legal filename collide
// on disk (an accepted minor limitation).
//
// FAIL-SOFT — every operation swallows filesystem errors via std::error_code and
// never throws: a missing directory, an unwritable disk, a vanished file, or an
// unusable (empty-after-sanitise) name yields false / nullptr / an empty list, for
// the caller to react to.
//
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace factory_presets
{
    inline std::string sanitizeFileName (std::string_view name)
    {
        static constexpr std::string_view illegal = "\"#@,;:<>*^|?\\/";
        constexpr std::size_t maxLen = 128;

        std::string out;
        out.reserve (name.size());
        for (char c : name)
        {
            const unsigned char uc = static_cast<unsigned char> (c);
            if (uc < 0x20) continue;                                  // control char
            if (illegal.find (c) != std::string_view::npos) continue; // illegal set
            out.push_back (c);
        }

        std::size_t b = 0, e = out.size();
        while (b < e && (out[b] == ' ' || out[b] == '.')) ++b;
        while (e > b && (out[e - 1] == ' ' || out[e - 1] == '.')) --e;
        out = out.substr (b, e - b);

        if (out.size() > maxLen)
            out.resize (maxLen);
        return out;
    }

    inline std::filesystem::path userPresetsRoot (std::string_view pluginName)
    {
        namespace fs = std::filesystem;
        fs::path base;

    #if defined(_WIN32)
        if (const char* appdata = std::getenv ("APPDATA"); appdata != nullptr && appdata[0] != '\0')
            base = appdata;
    #elif defined(__APPLE__)
        if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0')
            base = fs::path (home) / "Library" / "Application Support";
    #else
        if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0')
            base = xdg;
        else if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0')
            base = fs::path (home) / ".config";
    #endif

        if (base.empty())
        {
            std::error_code ec;
            base = fs::temp_directory_path (ec);
        }

        return base / "TatsunariSounds" / sanitizeFileName (pluginName) / "Presets";
    }

    class UserPresetStoreFs
    {
    public:
        using Blob = std::vector<unsigned char>;
        static constexpr std::string_view kExtension = ".state";

        // Real usage: derive the directory from the plugin's display name.
        explicit UserPresetStoreFs (std::string_view pluginName)
            : presetsDir (userPresetsRoot (pluginName)) {}

        // Testability: point straight at the presets directory to use (already the
        // final directory, no further nesting) — keeps I/O off the real user data.
        explicit UserPresetStoreFs (std::filesystem::path presetsDirectoryOverride)
            : presetsDir (std::move (presetsDirectoryOverride)) {}

        const std::filesystem::path& directory() const noexcept { return presetsDir; }

        // Sorted (byte-wise; divergence: the framework sorted case-insensitively),
        // re-enumerated every call. Empty — not an error — if nothing is saved yet.
        std::vector<std::string> list() const
        {
            std::vector<std::string> names;
            std::error_code ec;
            if (! std::filesystem::is_directory (presetsDir, ec))
                return names;

            for (std::filesystem::directory_iterator it (presetsDir, ec), end; it != end; it.increment (ec))
            {
                if (ec) break;
                std::error_code fec;
                if (it->is_regular_file (fec) && it->path().extension() == kExtension)
                    names.push_back (it->path().stem().string());
            }
            std::sort (names.begin(), names.end());
            return names;
        }

        bool exists (std::string_view name) const
        {
            const auto f = fileFor (name);
            if (f.empty()) return false;
            std::error_code ec;
            return std::filesystem::is_regular_file (f, ec);
        }

        // Creates the directory on demand; overwrite allowed. False on an unusable
        // name (sanitises to empty) or a write failure.
        bool save (std::string_view name, const Blob& blob) const
        {
            const auto f = fileFor (name);
            if (f.empty()) return false;

            std::error_code ec;
            std::filesystem::create_directories (presetsDir, ec);
            if (! std::filesystem::is_directory (presetsDir, ec))
                return false;

            std::ofstream os (f, std::ios::binary | std::ios::trunc);
            if (! os) return false;
            if (! blob.empty())
                os.write (reinterpret_cast<const char*> (blob.data()),
                          static_cast<std::streamsize> (blob.size()));
            return static_cast<bool> (os);
        }

        // nullopt on a missing file, an unusable name, or a read failure.
        std::optional<Blob> load (std::string_view name) const
        {
            const auto f = fileFor (name);
            if (f.empty()) return std::nullopt;

            std::error_code ec;
            if (! std::filesystem::is_regular_file (f, ec))
                return std::nullopt;

            std::ifstream is (f, std::ios::binary);
            if (! is) return std::nullopt;

            Blob blob ((std::istreambuf_iterator<char> (is)), std::istreambuf_iterator<char>());
            if (is.bad()) return std::nullopt;
            return blob;
        }

        // True (no-op) if the preset doesn't exist; false only on an actual delete
        // failure or an unusable name.
        bool remove (std::string_view name) const
        {
            const auto f = fileFor (name);
            if (f.empty()) return false;

            std::error_code ec;
            if (! std::filesystem::exists (f, ec))
                return true;
            return std::filesystem::remove (f, ec) && ! ec;
        }

    private:
        std::filesystem::path fileFor (std::string_view name) const
        {
            const std::string legal = sanitizeFileName (name);
            if (legal.empty()) return {};
            return presetsDir / (legal + std::string (kExtension));
        }

        std::filesystem::path presetsDir;
    };
}
