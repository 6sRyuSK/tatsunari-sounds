#pragma once
//
// Minimal SemVer (MAJOR.MINOR.PATCH only). Prerelease / build metadata make a
// string incomparable for the stable-channel badge path (plan §1).
//
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace factory_update
{
    struct SemVer
    {
        int major = 0;
        int minor = 0;
        int patch = 0;

        friend bool operator== (const SemVer& a, const SemVer& b) noexcept
        {
            return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
        }
        friend bool operator< (const SemVer& a, const SemVer& b) noexcept
        {
            return std::tie (a.major, a.minor, a.patch)
                 < std::tie (b.major, b.minor, b.patch);
        }
        friend bool operator> (const SemVer& a, const SemVer& b) noexcept { return b < a; }
    };

    inline std::optional<SemVer> parseSemVer (std::string_view s)
    {
        if (s.empty())
            return std::nullopt;
        // Reject prerelease / build metadata for the stable badge path.
        for (char c : s)
            if (! (std::isdigit (static_cast<unsigned char> (c)) || c == '.'))
                return std::nullopt;

        SemVer v;
        int* parts[3] = { &v.major, &v.minor, &v.patch };
        std::size_t i = 0;
        for (int p = 0; p < 3; ++p)
        {
            if (i >= s.size() || ! std::isdigit (static_cast<unsigned char> (s[i])))
                return std::nullopt;
            int n = 0;
            while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])))
            {
                n = n * 10 + (s[i] - '0');
                ++i;
            }
            *parts[p] = n;
            if (p < 2)
            {
                if (i >= s.size() || s[i] != '.')
                    return std::nullopt;
                ++i;
            }
        }
        if (i != s.size())
            return std::nullopt;
        return v;
    }

    // True when latest is a strictly newer stable SemVer than current.
    inline bool isUpdateAvailable (std::string_view current, std::string_view latest)
    {
        const auto c = parseSemVer (current);
        const auto l = parseSemVer (latest);
        if (! c || ! l)
            return false;
        return *l > *c;
    }
}
