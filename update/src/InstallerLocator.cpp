#include "factory_update/InstallerLocator.h"
#include "factory_update/Urls.h"

#include <cstdlib>
#include <string>
#include <system_error>

#if ! defined(_WIN32)
#  include <cstdlib> // std::system
#endif

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

namespace factory_update
{
    namespace
    {
#if defined(_WIN32)
        std::filesystem::path userInstallerRoot()
        {
            if (const char* local = std::getenv ("LOCALAPPDATA"); local && *local)
                return std::filesystem::path (local) / "tatsunari-sounds" / "bin";
            return {};
        }
        std::filesystem::path systemInstallerRoot()
        {
            if (const char* pf = std::getenv ("ProgramFiles"); pf && *pf)
                return std::filesystem::path (pf) / "tatsunari-sounds";
            return std::filesystem::path ("C:/Program Files") / "tatsunari-sounds";
        }
        const char* binaryName() { return "tatsunari-sounds-installer.exe"; }
#elif defined(__APPLE__)
        std::filesystem::path userInstallerRoot()
        {
            if (const char* home = std::getenv ("HOME"); home && *home)
                return std::filesystem::path (home) / "Library" / "Application Support"
                     / "tatsunari-sounds" / "bin";
            return {};
        }
        std::filesystem::path systemInstallerRoot()
        {
            return "/Library/Application Support/tatsunari-sounds/bin";
        }
        const char* binaryName() { return "tatsunari-sounds-installer"; }
#else
        std::filesystem::path userInstallerRoot()
        {
            if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg && *xdg)
                return std::filesystem::path (xdg) / "tatsunari-sounds" / "bin";
            if (const char* home = std::getenv ("HOME"); home && *home)
                return std::filesystem::path (home) / ".config" / "tatsunari-sounds" / "bin";
            return {};
        }
        std::filesystem::path systemInstallerRoot()
        {
            return "/usr/local/lib/tatsunari-sounds/bin";
        }
        const char* binaryName() { return "tatsunari-sounds-installer"; }
#endif
    } // namespace

    std::filesystem::path installerBinaryPath (InstallerScope scope)
    {
        const auto root = (scope == InstallerScope::User) ? userInstallerRoot()
                                                          : systemInstallerRoot();
        return root / binaryName();
    }

    std::optional<InstallerLocation> findInstallerBinary()
    {
        for (auto scope : { InstallerScope::User, InstallerScope::System })
        {
            const auto p = installerBinaryPath (scope);
            std::error_code ec;
            if (! p.empty() && std::filesystem::is_regular_file (p, ec))
                return InstallerLocation { p, scope };
        }
        return std::nullopt;
    }

    bool launchInstaller (const std::filesystem::path& path, const std::string& slug)
    {
        if (path.empty() || slug.empty())
            return false;
#if defined(_WIN32)
        const std::wstring wpath = path.wstring();
        const std::wstring args = L"--plugin " + std::wstring (slug.begin(), slug.end());
        const HINSTANCE hi = ShellExecuteW (nullptr, L"open", wpath.c_str(), args.c_str(),
                                            nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t> (hi) > 32;
#elif defined(__APPLE__)
        // open -a Terminal <path> — TUI needs a controlling tty (plan §4.3).
        const std::string cmd = "open -a Terminal \"" + path.string() + "\"";
        // Also pass --plugin via a tiny wrapper is hard with `open -a Terminal`;
        // invoke via osascript so args survive.
        const std::string osa =
            "osascript -e 'tell application \"Terminal\" to do script "
            "\"" + path.string() + " --plugin " + slug + "\"'";
        return std::system (osa.c_str()) == 0 || std::system (cmd.c_str()) == 0;
#else
        (void) path;
        (void) slug;
        return false; // Linux is not a shipping target
#endif
    }

    bool openDistributePageAndCopyOneLiner()
    {
#if defined(_WIN32)
        const std::string url (kHumanUpdatesURL);
        ShellExecuteA (nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (OpenClipboard (nullptr))
        {
            const std::string line (kBootstrapOneLinerWin);
            EmptyClipboard();
            HGLOBAL h = GlobalAlloc (GMEM_MOVEABLE, line.size() + 1);
            if (h)
            {
                memcpy (GlobalLock (h), line.c_str(), line.size() + 1);
                GlobalUnlock (h);
                SetClipboardData (CF_TEXT, h);
            }
            CloseClipboard();
        }
        return true;
#elif defined(__APPLE__)
        const std::string openCmd = "open \"" + std::string (kHumanUpdatesURL) + "\"";
        std::system (openCmd.c_str());
        const std::string pb =
            "printf %s " + std::string (kBootstrapOneLinerMac) + " | pbcopy";
        std::system (pb.c_str());
        return true;
#else
        return false;
#endif
    }
}
