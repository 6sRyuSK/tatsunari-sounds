#pragma once
//
// factory_update::Urls — baked public origin (plan §6.1). Must stay byte-aligned
// with tools/installer/internal/updates/hosts.go. tools/tests/test_update_url_constants.py
// gates drift between the two.
//
#include <string_view>

namespace factory_update
{
    inline constexpr std::string_view kPublicHost      = "6sryusk.com";
    inline constexpr std::string_view kPathBrandRoot   = "/tatsunarisounds/";
    inline constexpr std::string_view kPathUpdatesV1   = "/tatsunarisounds/updates/v1/";
    inline constexpr std::string_view kPathArtifacts   = "/tatsunarisounds/artifacts/";
    inline constexpr std::string_view kPathNotes       = "/tatsunarisounds/notes/";
    inline constexpr std::string_view kPathInstallSH   = "/tatsunarisounds/install.sh";
    inline constexpr std::string_view kPathInstallPS1  = "/tatsunarisounds/install.ps1";

    inline constexpr std::string_view kLatestJSONURL   =
        "https://6sryusk.com/tatsunarisounds/updates/v1/latest.json";
    inline constexpr std::string_view kCatalogJSONURL  =
        "https://6sryusk.com/tatsunarisounds/updates/v1/catalog.json";
    inline constexpr std::string_view kHumanUpdatesURL =
        "https://6sryusk.com/tatsunarisounds/updates/";
    inline constexpr std::string_view kBrandHomeURL    =
        "https://6sryusk.com/tatsunarisounds/";

    // Bootstrap one-liners shown when the installer binary is not on disk yet.
    inline constexpr std::string_view kBootstrapOneLinerMac =
        "curl -fsSL https://6sryusk.com/tatsunarisounds/install.sh | bash";
    inline constexpr std::string_view kBootstrapOneLinerWin =
        "irm https://6sryusk.com/tatsunarisounds/install.ps1 | iex";

    inline constexpr std::size_t kMaxLatestBytes = 256 * 1024;
    inline constexpr int kSchemaVersion = 1;
}
