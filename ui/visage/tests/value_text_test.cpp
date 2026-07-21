//
// ui/visage/tests/value_text_test.cpp — headless unit test for the visage-free
// direct value-entry helpers in factory_ui_visage/ValueText.h (stripLeadingNumber,
// entryPrefillText, commitEntryText).
//
// Compiles headers only with a plain host compiler (no visage, no JUCE) — this
// build also PROVES ValueText.h is visage-free; a stray visage include would fail
// to link/compile here. Conventions mirror params/tests/params_test.cpp:
// accumulate failures, return 1 at the end. Rate-independent, one case.
//
// Manual run:
//   c++ -std=c++17 -Iui/visage/include -Iparams/include \
//       ui/visage/tests/value_text_test.cpp -o value_text_test && ./value_text_test
//
#include "factory_ui_visage/ValueText.h"

#include "factory_params/ParamDesc.h"
#include "factory_params/Range.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace factory_ui_visage;

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

bool bitEqual (float a, float b)
{
    return std::memcmp (&a, &b, sizeof (float)) == 0;
}

// A tiny store: one continuous float with a unit, one stepped float.
factory_params::ParamStore makeStore()
{
    std::vector<factory_params::ParamDesc> descs;
    descs.push_back (factory_params::floatParam ("depth", "Depth", 0.0f, 100.0f, 1.0f, 30.0f, " %", 1));
    descs.push_back (factory_params::floatParam ("out", "Out", -24.0f, 24.0f, 0.0f, 0.0f, " dB", 1));
    return factory_params::ParamStore (std::move (descs));
}
} // namespace

// ---------------------------------------------------------------------------
void checkStripLeadingNumber()
{
    std::printf ("1. stripLeadingNumber\n");
    check (stripLeadingNumber ("62%") == "62",       "strips a trailing %% unit");
    check (stripLeadingNumber ("2.6kHz") == "2.6",   "strips a kHz suffix");
    check (stripLeadingNumber ("-24.0dB") == "-24.0","keeps the sign, strips dB");
    check (stripLeadingNumber ("  12") == "12",      "leading whitespace trimmed");
    check (stripLeadingNumber ("abc") == "",         "no leading number -> empty");
    check (stripLeadingNumber ("") == "",            "empty stays empty");
    check (stripLeadingNumber ("+.5x") == "+.5",     "sign + bare fraction kept");
    check (stripLeadingNumber ("1.2.3") == "1.2",    "second dot ends the number");
}

// ---------------------------------------------------------------------------
void checkEntryPrefill()
{
    std::printf ("2. entryPrefillText\n");
    factory_params::ParamStore store = makeStore();
    // The prefill is the drawn read-out (formatValue, spaces stripped) reduced to a
    // bare number — same reduction Knob::draw + openValueEntry apply.
    check (entryPrefillText (store.desc (0), 62.0f, 0) == "62",
           "depth 62 -> \"62\" (unit stripped)");
    check (entryPrefillText (store.desc (1), -24.0f, 1) == "-24.0",
           "out -24 dB, 1 decimal -> \"-24.0\"");
    check (entryPrefillText (store.desc (1), 0.5f, 2) == "0.50",
           "out 0.5 dB, 2 decimals -> \"0.50\"");
}

// ---------------------------------------------------------------------------
void checkCommitEntryText()
{
    std::printf ("3. commitEntryText\n");
    using factory_params::HostWrite;

    // Invalid / empty input REVERTS: false, no write, no queue event (round #4).
    {
        factory_params::ParamStore store = makeStore();
        const float before = store.value (0);
        check (! commitEntryText (store, 0, "abc"), "\"abc\" -> false (revert)");
        check (! commitEntryText (store, 0, ""),    "\"\" -> false (revert)");
        check (bitEqual (store.value (0), before),  "reverted commit leaves the value");
        int events = 0;
        store.drainHostWrites ([&] (const HostWrite&) { ++events; });
        check (events == 0, "reverted commit enqueues nothing");
    }

    // A valid number commits through the gesture path, snapped + clamped.
    {
        factory_params::ParamStore store = makeStore();
        const std::uint32_t g0 = store.gestureEndCount();
        check (commitEntryText (store, 0, "62"), "\"62\" commits");
        const factory_params::RangeSpec r = factory_params::makeRange (store.desc (0));
        check (bitEqual (store.value (0), factory_params::snapToLegalValue (r, 62.0f)),
               "committed value is snapped");
        check (store.gestureEndCount() == g0 + 1, "commit is one full gesture");
        std::vector<HostWrite::Kind> kinds;
        store.drainHostWrites ([&] (const HostWrite& w) { kinds.push_back (w.kind); });
        check (kinds.size() == 3
               && kinds[0] == HostWrite::Kind::GestureBegin
               && kinds[1] == HostWrite::Kind::Value
               && kinds[2] == HostWrite::Kind::GestureEnd,
               "commit enqueues Begin/Value/End");
    }

    // Out-of-range still commits + clamps (deviation is only for INVALID input).
    {
        factory_params::ParamStore store = makeStore();
        check (commitEntryText (store, 1, "999"), "out-of-range commits");
        check (bitEqual (store.value (1), 24.0f), "..and clamps to the max");
    }
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf ("factory_ui_visage ValueText — headless checks\n");
    checkStripLeadingNumber();
    checkEntryPrefill();
    checkCommitEntryText();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
