// Host-side (no visage / no JUCE) unit check for the RS theme's analyser roles —
// the RS analog of ui/visage/tests/theme_roundtrip_test.cpp. It guards the
// "corrected roles" (design reference 2026-07-17): PRE + POST are thin SOLID
// coral lines, the combined reduction PROFILE is a dashed muted-coral line, and
// per-node fills are subtle. The compiled RsExtras::defaults() is the source of
// truth (RsSuppressionCurveView reads it); theme-rs.json's rs.analyzer object is
// the human-review mirror and must stay in sync — this test asserts both, and
// that the OLD (mislabelled) keys are gone.
//
//   c++ -std=c++17 -I ../.. -I ../../../../ui/visage/include \
//       -I ../../../../params/include -I ../../../../core/include \
//       rs_theme_roundtrip_test.cpp ../../../../ui/visage/src/Theme.cpp -o t \
//   && ./t ../theme-rs.json
//
#include "RsTheme.h"

#include <cstdint>
#include <cstdio>
#include <string>

static int g_failures = 0;
static void check(const char* name, bool ok, const std::string& detail = "")
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : "  -> ", detail.c_str());
    if (!ok) ++g_failures;
}

static std::string hex(std::uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", v); return b; }

// Raw token (number or "quoted string" without quotes) after `"key":` in `doc`;
// empty if the key is absent. Deliberately tiny — enough to compare the values.
static std::string valueOf(const std::string& doc, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    auto p = doc.find(needle);
    if (p == std::string::npos) return {};
    p = doc.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < doc.size() && (doc[p] == ' ' || doc[p] == '\t' || doc[p] == '\n' || doc[p] == '\r')) ++p;
    if (p < doc.size() && doc[p] == '"') { auto e = doc.find('"', p + 1); return doc.substr(p + 1, e - p - 1); }
    auto e = p;
    while (e < doc.size() && doc[e] != ',' && doc[e] != '}' && doc[e] != '\n') ++e;
    std::string s = doc.substr(p, e - p);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

int main(int argc, char** argv)
{
    using rs_ui::RsExtras;
    const RsExtras rs = RsExtras::defaults();

    // 1) Compiled defaults carry the corrected analyser roles.
    check("preColour is solid coral",      rs.preColour == 0xffff7a6bu, hex(rs.preColour));
    check("preLineWidth == 2.0",           rs.preLineWidth == 2.0f);
    check("postColour is solid coral (kV201)", rs.postColour == 0xffff7a6bu, hex(rs.postColour));
    check("postLineWidth == 1.4 (kV201)",  rs.postLineWidth == 1.4f);
    check("postLineAlpha == 0.85 (kV201)", rs.postLineAlpha == 0.85f);
    check("profileColour is muted coral",  rs.profileColour == 0xffe08a7fu, hex(rs.profileColour));
    check("profileLineWidth == 1.5",       rs.profileLineWidth == 1.5f);
    check("profileDashOn == 5.0",          rs.profileDashOn == 5.0f);
    check("profileDashOff == 4.0",         rs.profileDashOff == 4.0f);
    check("perNodeFillAlpha == 0.12",      rs.perNodeFillAlpha == 0.12f);
    check("curtainFillAlpha == 0.34",      rs.curtainFillAlpha == 0.34f);
    check("curtainClampFrac == 0.5",       rs.curtainClampFrac == 0.5f);

    // 2) theme-rs.json parses (shared-schema parts) AND its rs.analyzer mirror
    //    matches the compiled defaults, with the old mislabelled keys removed.
    const std::string path = argc > 1 ? argv[1] : "../theme-rs.json";
    std::string doc;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) doc.append(buf, n);
        std::fclose(f);
    }
    check("theme-rs.json is readable", !doc.empty(), path);
    if (!doc.empty())
    {
        // NOTE: theme-rs.json's rs.analyzer is the human-review MIRROR of the
        // compiled RsExtras above (the runtime draws from RsExtras::defaults(), not
        // from this file — RsTheme::load ignores the whole "rs" object). So we check
        // the JSON's values match the compiled defaults, not that the file round-
        // trips through the strict shared parser (it intentionally carries top-level
        // "_comment" documentation the shared schema doesn't recognise).
        check("json postColour == #ffff7a6b",  valueOf(doc, "postColour") == "#ffff7a6b", valueOf(doc, "postColour"));
        check("json postLineWidth == 1.4",      valueOf(doc, "postLineWidth") == "1.4", valueOf(doc, "postLineWidth"));
        check("json postLineAlpha == 0.85",     valueOf(doc, "postLineAlpha") == "0.85", valueOf(doc, "postLineAlpha"));
        check("json profileColour == #ffe08a7f", valueOf(doc, "profileColour") == "#ffe08a7f", valueOf(doc, "profileColour"));
        check("json profileDashOn == 5.0",      valueOf(doc, "profileDashOn") == "5.0", valueOf(doc, "profileDashOn"));
        check("json profileDashOff == 4.0",     valueOf(doc, "profileDashOff") == "4.0", valueOf(doc, "profileDashOff"));
        check("json perNodeFillAlpha == 0.12",  valueOf(doc, "perNodeFillAlpha") == "0.12", valueOf(doc, "perNodeFillAlpha"));

        // Old mislabelled roles must be gone (POST no longer dashed; no per-node stroke).
        check("json drops postDashOn",       valueOf(doc, "postDashOn").empty());
        check("json drops postDashOff",      valueOf(doc, "postDashOff").empty());
        check("json drops perNodeStrokeAlpha", valueOf(doc, "perNodeStrokeAlpha").empty());
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
