// Host-side (no visage / no JUCE) unit check for the RS theme's analyser roles —
// the RS analog of ui/visage/tests/theme_roundtrip_test.cpp. It guards the
// v2.1.0 look (AnalyzerStyle.h kV201Style, the shipped JUCE editor): the reduction
// curtain is AreaFromZero teal (fill 0.28 / stroke 0.8 / 1px), PRE is a muted-taupe
// FILLED area (0.22->0.02, no line), POST is a thin SOLID coral line, the combined
// reduction PROFILE is a SOLID coral line (2.2px) + glow (0.22/5px), and each node
// adds a translucent fill (0.12) + stroke (0.7). The compiled RsExtras::defaults()
// is the source of truth (RsSuppressionCurveView reads it); theme-rs.json's
// rs.analyzer object is the human-review mirror and must stay in sync — this test
// asserts both, and that the OLD demo-look keys (dashed profile / from-top curtain)
// are gone.
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

    // 1) Compiled defaults carry the v2.1.0 (kV201Style) analyser roles.
    check("curtainFillAlpha == 0.28 (kV201 deltaFillAlpha)",   rs.curtainFillAlpha == 0.28f);
    check("curtainStrokeAlpha == 0.8 (kV201 deltaStrokeAlpha)", rs.curtainStrokeAlpha == 0.8f);
    check("curtainStrokeWidth == 1.0 (kV201 deltaStrokeWidth)", rs.curtainStrokeWidth == 1.0f);
    check("preColour is muted taupe #b9a39b",  rs.preColour == 0xffb9a39bu, hex(rs.preColour));
    check("preFillTopAlpha == 0.22",           rs.preFillTopAlpha == 0.22f);
    check("preFillBotAlpha == 0.02",           rs.preFillBotAlpha == 0.02f);
    check("postColour is solid coral (kV201)", rs.postColour == 0xffff7a6bu, hex(rs.postColour));
    check("postLineWidth == 1.4 (kV201)",      rs.postLineWidth == 1.4f);
    check("postLineAlpha == 0.85 (kV201)",     rs.postLineAlpha == 0.85f);
    check("profileColour is accent coral (kV201)", rs.profileColour == 0xffff7a6bu, hex(rs.profileColour));
    check("profileGlowAlpha == 0.22",          rs.profileGlowAlpha == 0.22f);
    check("profileGlowWidth == 5.0",           rs.profileGlowWidth == 5.0f);
    check("profileStrokeWidth == 2.2",         rs.profileStrokeWidth == 2.2f);
    check("profileStrokeAlpha == 1.0",         rs.profileStrokeAlpha == 1.0f);
    check("perNodeFillAlpha == 0.12",          rs.perNodeFillAlpha == 0.12f);
    check("perNodeStrokeAlpha == 0.7",         rs.perNodeStrokeAlpha == 0.7f);

    // 2) theme-rs.json parses (shared-schema parts) AND its rs.analyzer mirror
    //    matches the compiled defaults, with the old demo-look keys removed.
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
        check("json curtainFillAlpha == 0.28", valueOf(doc, "curtainFillAlpha") == "0.28", valueOf(doc, "curtainFillAlpha"));
        check("json curtainStrokeAlpha == 0.8", valueOf(doc, "curtainStrokeAlpha") == "0.8", valueOf(doc, "curtainStrokeAlpha"));
        check("json preColour == #ffb9a39b",   valueOf(doc, "preColour") == "#ffb9a39b", valueOf(doc, "preColour"));
        check("json preFillTopAlpha == 0.22",   valueOf(doc, "preFillTopAlpha") == "0.22", valueOf(doc, "preFillTopAlpha"));
        check("json postColour == #ffff7a6b",  valueOf(doc, "postColour") == "#ffff7a6b", valueOf(doc, "postColour"));
        check("json profileColour == #ffff7a6b", valueOf(doc, "profileColour") == "#ffff7a6b", valueOf(doc, "profileColour"));
        check("json profileGlowWidth == 5.0",   valueOf(doc, "profileGlowWidth") == "5.0", valueOf(doc, "profileGlowWidth"));
        check("json profileStrokeWidth == 2.2", valueOf(doc, "profileStrokeWidth") == "2.2", valueOf(doc, "profileStrokeWidth"));
        check("json perNodeStrokeAlpha == 0.7", valueOf(doc, "perNodeStrokeAlpha") == "0.7", valueOf(doc, "perNodeStrokeAlpha"));

        // Old demo-look keys must be GONE: dashed profile, from-top curtain clamp,
        // separate PRE/profile line widths.
        check("json drops profileDashOn",   valueOf(doc, "profileDashOn").empty(), valueOf(doc, "profileDashOn"));
        check("json drops profileDashOff",  valueOf(doc, "profileDashOff").empty(), valueOf(doc, "profileDashOff"));
        check("json drops curtainClampFrac", valueOf(doc, "curtainClampFrac").empty(), valueOf(doc, "curtainClampFrac"));
        check("json drops preLineWidth",    valueOf(doc, "preLineWidth").empty(), valueOf(doc, "preLineWidth"));
        check("json drops profileLineWidth", valueOf(doc, "profileLineWidth").empty(), valueOf(doc, "profileLineWidth"));
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
