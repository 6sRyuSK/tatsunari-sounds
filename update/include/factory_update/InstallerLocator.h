#pragma once
//
// Locate the permanently-installed tatsunari binary (plan §4.3).
// Search order: user → system. PATH is never consulted.
// Paths must match tools/installer/internal/install/selfinstall.go.
//
#include <filesystem>
#include <optional>
#include <string>

namespace factory_update
{
    enum class InstallerScope { User, System };

    struct InstallerLocation
    {
        std::filesystem::path path;
        InstallerScope scope = InstallerScope::User;
    };

    std::optional<InstallerLocation> findInstallerBinary();

    // Canonical paths (may not exist on disk).
    std::filesystem::path installerBinaryPath (InstallerScope scope);

    // Launch the TUI with --plugin <slug>. UI thread only. Returns false on failure.
    bool launchInstaller (const std::filesystem::path& path, const std::string& slug);

    // Open the human updates page in the default browser and copy the OS
    // bootstrap one-liner to the clipboard. Soft-fail; returns false if neither
    // action could be attempted on this platform.
    bool openDistributePageAndCopyOneLiner();
}
