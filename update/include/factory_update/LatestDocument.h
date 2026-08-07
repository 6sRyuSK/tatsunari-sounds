#pragma once
//
// Badge-only latest.json (plan §3.2). Visage-free / JUCE-free.
//
#include <string>
#include <vector>

namespace factory_update
{
    struct LatestPlugin
    {
        std::string slug;
        std::string latest;
        std::vector<std::string> highlights;
        std::string changelogUrl;
    };

    struct LatestDocument
    {
        int schema = 0;
        std::string generated;
        std::vector<LatestPlugin> plugins;
    };

    // Parse a latest.json body. Returns false on hard envelope errors (bad JSON,
    // wrong schema, missing required fields). Unknown top-level keys are ignored.
    // Individual plugin rows with missing slug/latest are skipped (not fatal).
    bool parseLatestDocument (const std::string& json, LatestDocument& out, std::string& error);

    // Find the row for slug, or nullptr.
    const LatestPlugin* findPlugin (const LatestDocument& doc, const std::string& slug);
}
