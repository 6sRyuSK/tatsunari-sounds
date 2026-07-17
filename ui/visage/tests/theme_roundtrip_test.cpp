// Host-side (no visage / no JUCE) unit check for the theme model + JSON parser.
//
//   c++ -std=c++17 -I ../include theme_roundtrip_test.cpp ../src/Theme.cpp -o t && ./t <factory-default.json>
//
// Verifies: the factory-default document parses, matches the compiled-in
// defaults, round-trips through toJson()->parse bit-for-bit, carries the exact
// palette from FactoryLookAndFeel, and that malformed input HARD-FAILS.
#include "factory_ui_visage/Theme.h"

#include <cstdint>
#include <cstdio>
#include <string>

using factory_ui_visage::Theme;

static int g_failures = 0;
static void check(const char* name, bool ok, const std::string& detail = "")
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : "  -> ", detail.c_str());
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "../theme/factory-default.json";

    // 1) The shipped default document parses.
    std::string err;
    Theme fileTheme;
    {
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            char buf[4096]; size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        check("factory-default.json is readable", !text.empty(), path);
        const bool ok = Theme::tryParse(text, fileTheme, err);
        check("factory-default.json parses", ok, err);
        if (!ok) return 1;
    }

    // 2) It matches the compiled-in design system verbatim.
    check("file == compiled-in defaults", fileTheme == Theme::defaults());

    // 3) Round-trips through toJson() -> parse bit-for-bit.
    {
        Theme reparsed;
        const bool ok = Theme::tryParse(fileTheme.toJson(), reparsed, err);
        check("toJson() re-parses", ok, err);
        check("round-trip is idempotent (parse == reparse)", ok && reparsed == fileTheme);
    }

    // 4) Spot-check the exact palette transcribed from FactoryLookAndFeel.h.
    const auto& p = fileTheme.palette;
    auto hex = [](std::uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", v); return std::string(b); };
    check("palette.background", p.background == 0xfffdf6f2u, hex(p.background));
    check("palette.accent",     p.accent     == 0xffff7a6bu, hex(p.accent));
    check("palette.text",       p.text       == 0xff6b5750u, hex(p.text));
    check("palette.shadow",     p.shadow     == 0x33d6a89au, hex(p.shadow));
    check("bandColours[0]",     p.bandColours[0] == 0xffff6f91u, hex(p.bandColours[0]));
    check("bandColours[5]",     p.bandColours[5] == 0xffb79be8u, hex(p.bandColours[5]));
    check("knob.arcStart≈1.2pi", fileTheme.knob.arcStart > 3.7699f && fileTheme.knob.arcStart < 3.7700f,
          std::to_string(fileTheme.knob.arcStart));
    check("card.cornerRadius==10", fileTheme.card.cornerRadius == 10.0f);
    check("font.title==20",        fileTheme.font.title == 20.0f);

    // 5) Malformed input HARD-FAILS with a message (no exceptions on tryParse).
    Theme dummy;
    check("reject truncated JSON",  !Theme::tryParse("{", dummy, err), err);
    check("reject bad colour",      !Theme::tryParse("{\"palette\":{\"accent\":\"zzzzzzzz\"}}", dummy, err), err);
    check("reject wrong type",      !Theme::tryParse("{\"knob\":{\"glowAlpha\":\"x\"}}", dummy, err), err);
    check("reject unknown key",     !Theme::tryParse("{\"palette\":{\"nope\":\"#ffffffff\"}}", dummy, err), err);
    check("reject non-object root", !Theme::tryParse("[1,2,3]", dummy, err), err);
    check("accept partial override (accent only)",
          Theme::tryParse("{\"palette\":{\"accent\":\"#ff112233\"}}", dummy, err) && dummy.palette.accent == 0xff112233u);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
