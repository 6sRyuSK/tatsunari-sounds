//
// plugins/resonance-suppressor/ui/tests/rs_ui_pure_test.cpp — headless unit tests
// for the RS editor's pure (visage-free, JUCE-free) UI functions: the FREQ Hz/kHz
// entry parser (RsFreqEntry.h) and — as they are extracted — the pure layout
// computations. Host compiler only, same conventions as rs_theme_roundtrip_test
// (accumulate failures, return 1 at the end). Rate-independent, one case.
//
// Manual run:
//   c++ -std=c++17 -Iplugins/resonance-suppressor/ui -Iui/visage/include \
//       -Iparams/include -Icore/include \
//       plugins/resonance-suppressor/ui/tests/rs_ui_pure_test.cpp -o rs_ui_pure && ./rs_ui_pure
//
#include "RsFreqEntry.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int g_failures = 0;

void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("  FAIL: %s\n", msg.c_str());
}

void check (bool cond, const std::string& msg)
{
    if (! cond)
        fail (msg);
}

// RS freq parameter range (Hz) — the panel passes desc.minValue/maxValue.
constexpr double kLo = 20.0, kHi = 20000.0;

void checkFreq (const std::string& text, float expected, const std::string& what)
{
    float out = 0.0f;
    if (! rs_ui::parseFreqEntry (text, kLo, kHi, out))
    {
        fail (what + ": unexpectedly reverted");
        return;
    }
    if (std::abs (out - expected) > 1.0e-3f)
        fail (what + ": got " + std::to_string (out) + ", expected " + std::to_string (expected));
}

void checkRevert (const std::string& text, const std::string& what)
{
    float out = 0.0f;
    if (rs_ui::parseFreqEntry (text, kLo, kHi, out))
        fail (what + ": expected revert (false), got " + std::to_string (out));
}
} // namespace

// ---------------------------------------------------------------------------
void checkParseFreqEntry()
{
    std::printf ("1. parseFreqEntry (Hz/kHz, revert-on-invalid)\n");

    // Explicit kHz suffix (case/space tolerant) and bare-k shorthand.
    checkFreq ("2.6kHz",  2600.0f, "\"2.6kHz\"");
    checkFreq ("2.6 kHz", 2600.0f, "\"2.6 kHz\" (space before suffix)");
    checkFreq ("2.6KHZ",  2600.0f, "\"2.6KHZ\" (case tolerant)");
    checkFreq ("2.6k",    2600.0f, "\"2.6k\"");
    checkFreq (" 440 ",   440.0f,  "\" 440 \" (trimmed Hz)");

    // kHz inference: a bare number below the minimum whose *1000 lands in range
    // is read as kHz (the display shows "2.6kHz", the user types "2.6").
    checkFreq ("2.6", 2600.0f, "\"2.6\" inferred as kHz (below min, *1000 in range)");
    checkFreq ("250", 250.0f,  "\"250\" stays Hz (already in range)");
    checkFreq ("21",  21.0f,   "\"21\" stays Hz (in range, no inference)");
    // Below min AND *1000 out of range: no inference, raw value (setFromUi clamps).
    checkFreq ("0.001", 0.001f, "\"0.001\" no inference (1 Hz *1000 == 1 < min)");

    // Trailing junk after the number is ignored by tryParseNumber (leading-number
    // contract, mirroring JUCE getValueFromText).
    checkFreq ("12abc", 12000.0f, "\"12abc\" -> leading 12, inferred kHz");

    // Invalid input REVERTS (round #4: no gesture, no write, not clamp-to-min).
    checkRevert ("abc", "\"abc\"");
    checkRevert ("",    "empty");
    checkRevert ("kHz", "\"kHz\" (suffix only)");
    checkRevert ("-",   "\"-\"");
    checkRevert ("k",   "\"k\" (bare shorthand)");
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf ("resonance-suppressor pure-UI helpers — headless checks\n");
    checkParseFreqEntry();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
