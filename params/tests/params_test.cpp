//
// params/tests/params_test.cpp — headless unit tests for the shared factory_params
// model (ParamDesc / Range / Text / ParamStore / UndoStack).
//
// Conventions mirror core/tests/primitives_test.cpp:
//   * links ONLY factory_params (no JUCE) — this build also proves the headers are
//     JUCE-free; a stray JUCE include would fail to compile here.
//   * accumulate failures in g_failures / fail(), return 1 at the end.
//   * a single case (nothing here is sample-rate dependent).
//
// Oracles are independent of the code under test where possible: analytic
// normalised values, hand-computed FNV goldens, and formula-independent Range
// invariants (unity at the ends, 0.5 at a setSkewForCentre range's centre, strict
// monotonicity). Range's BIT-EXACT parity with live juce objects is additionally
// gated in resonance-suppressor's preset_test ("paramdesc parity").
//
#include "factory_params/ParamDesc.h"
#include "factory_params/Range.h"
#include "factory_params/Text.h"
#include "factory_params/ParamStore.h"
#include "factory_params/UndoStack.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace factory_params;

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

void checkNear (double actual, double expected, double tol, const std::string& what)
{
    if (! (std::abs (actual - expected) <= tol))
        fail (what + ": got " + std::to_string (actual) + ", expected "
              + std::to_string (expected) + " +/- " + std::to_string (tol));
}

bool bitEqual (float a, float b)
{
    return std::memcmp (&a, &b, sizeof (float)) == 0;
}

// ULP distance between two same-sign finite floats.
int ulpDist (float a, float b)
{
    if (a == b) return 0;
    std::int32_t ia, ib;
    std::memcpy (&ia, &a, sizeof ia);
    std::memcpy (&ib, &b, sizeof ib);
    if ((ia < 0) != (ib < 0)) return 1 << 30; // opposite sides of zero
    return std::abs (ia - ib);
}

bool vecEq (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (! bitEqual (a[i], b[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------
void checkFnv()
{
    std::printf ("1. fnv1a32 golden constants + uid uniqueness\n");

    // Goldens computed once by an independent oracle (a standalone FNV-1a
    // implementation). These are the STABILITY CONTRACT — the uid for a given id
    // must never change, so if a future edit alters fnv1a32 these must be
    // revisited with a human (not silently updated).
    check (fnv1a32 ("depth")  == 0xfe759eeau, "fnv1a32(\"depth\") golden");
    check (fnv1a32 ("attack") == 0x4595b8fdu, "fnv1a32(\"attack\") golden");
    check (fnv1a32 ("bypass") == 0x5dcb1cadu, "fnv1a32(\"bypass\") golden");

    // Empty string hashes to the offset basis.
    check (fnv1a32 ("") == 0x811c9dc5u, "fnv1a32(\"\") == offset basis");

    // Uniqueness across a sample of resonance-suppressor ids (the real table is
    // checked exhaustively in preset_test; this is a self-contained sample).
    const std::vector<std::string> ids = {
        "depth", "sharpness", "attack", "release", "mix", "delta", "link", "bypass",
        "mode", "selectivity", "detail", "out", "tilt", "quality", "linkAmt",
        "channelMode", "scEnable", "scListen", "lc_on", "lc_freq", "lc_slope",
        "hc_on", "hc_freq", "hc_slope", "b0_on", "b0_freq", "b0_type", "b0_sens",
        "b7_width"
    };
    for (std::size_t i = 0; i < ids.size(); ++i)
        for (std::size_t j = i + 1; j < ids.size(); ++j)
            if (fnv1a32 (ids[i]) == fnv1a32 (ids[j]))
                fail ("uid collision between '" + ids[i] + "' and '" + ids[j] + "'");
}

// ---------------------------------------------------------------------------
void checkRange()
{
    std::printf ("2. Range conversion invariants (JUCE-free NormalisableRange math)\n");

    const ParamDesc depth  = floatParam ("depth", "Depth", 0.0f, 100.0f, 0.1f, 30.0f, " %", 1);
    const ParamDesc attack = floatParam ("attack", "Attack", 1.0f, 200.0f, 0.0f, 20.0f, " ms", 1, 20.0f);
    const ParamDesc freq   = floatParam ("freq", "Freq", 20.0f, 20000.0f, 0.0f, 650.0f, " Hz", 1, 650.0f);
    const ParamDesc slope  = choiceParam ("slope", "Slope",
                                          { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 2, 1);

    const RangeSpec rDepth  = makeRange (depth);
    const RangeSpec rAttack = makeRange (attack);
    const RangeSpec rFreq   = makeRange (freq);
    const RangeSpec rSlope  = makeRange (slope);

    // Unity at the ends (formula-independent): to0to1(min)==0, to0to1(max)==1.
    auto ends = [] (const char* nm, const RangeSpec& r)
    {
        check (bitEqual (convertTo0to1 (r, r.start), 0.0f), std::string ("to0to1(min)==0 for ") + nm);
        check (bitEqual (convertTo0to1 (r, r.end),   1.0f), std::string ("to0to1(max)==1 for ") + nm);
    };
    ends ("depth", rDepth);
    ends ("attack", rAttack);
    ends ("freq", rFreq);

    // A setSkewForCentre range maps its centre to 0.5, exactly or within 1 ulp.
    check (ulpDist (convertTo0to1 (rAttack, 20.0f), 0.5f) <= 1, "to0to1(centre)==0.5 (attack) within 1 ulp");
    check (ulpDist (convertTo0to1 (rFreq, 650.0f), 0.5f) <= 1, "to0to1(centre)==0.5 (freq) within 1 ulp");

    // Strict monotonicity on a dense grid (linear and skewed).
    auto mono = [] (const char* nm, const RangeSpec& r)
    {
        float prev = -1.0f;
        bool ok = true;
        for (int k = 0; k <= 1000; ++k)
        {
            const float x = r.start + (r.end - r.start) * (float) k / 1000.0f;
            const float y = convertTo0to1 (r, x);
            if (k > 0 && ! (y > prev)) { ok = false; break; }
            prev = y;
        }
        check (ok, std::string ("to0to1 strictly increasing for ") + nm);
    };
    mono ("depth", rDepth);
    mono ("attack", rAttack);
    mono ("freq", rFreq);

    // Round-trip closeness: from0to1(to0to1(x)) ~= x (real domain).
    auto roundTrip = [] (const char* nm, const RangeSpec& r)
    {
        const double span = (double) (r.end - r.start);
        const double tol = 1.0e-3 + 1.0e-4 * span;
        for (int k = 0; k <= 500; ++k)
        {
            const float x = r.start + (r.end - r.start) * (float) k / 500.0f;
            const float xr = convertFrom0to1 (r, convertTo0to1 (r, x));
            checkNear (xr, x, tol, std::string ("round-trip from0to1(to0to1(x)) for ") + nm);
        }
    };
    roundTrip ("depth", rDepth);
    roundTrip ("attack", rAttack);
    roundTrip ("freq", rFreq);

    // Snapping — stepped {0,100,0.1}.
    checkNear (snapToLegalValue (rDepth, 3.14f),  3.1f, 1.0e-4, "snap depth 3.14 -> 3.1");
    checkNear (snapToLegalValue (rDepth, 50.0f),  50.0f, 1.0e-4, "snap depth 50 -> 50");
    checkNear (snapToLegalValue (rDepth, 3.04f),  3.0f, 1.0e-4, "snap depth 3.04 -> 3.0");
    // Snapping — choice-style {0,3,1}.
    checkNear (snapToLegalValue (rSlope, 1.4f),   1.0f, 1.0e-6, "snap slope 1.4 -> 1");
    checkNear (snapToLegalValue (rSlope, 2.6f),   3.0f, 1.0e-6, "snap slope 2.6 -> 3");

    // Clamping outside [min,max].
    check (bitEqual (convertTo0to1 (rDepth, -5.0f),  0.0f), "to0to1 clamps below min to 0");
    check (bitEqual (convertTo0to1 (rDepth, 105.0f), 1.0f), "to0to1 clamps above max to 1");
    checkNear (snapToLegalValue (rDepth, -5.0f),  0.0f,   1.0e-6, "snap clamps below min");
    checkNear (snapToLegalValue (rDepth, 105.0f), 100.0f, 1.0e-6, "snap clamps above max");
}

// ---------------------------------------------------------------------------
void checkNormalizedDefault()
{
    std::printf ("3. normalizedDefault for Float / Bool / Choice\n");

    // Float linear: (default - min) / (max - min).
    checkNear (normalizedDefault (floatParam ("depth", "Depth", 0.0f, 100.0f, 0.1f, 30.0f, " %", 1)),
               0.30, 1.0e-6, "normDefault depth == 0.30");
    checkNear (normalizedDefault (floatParam ("out", "Output", -24.0f, 24.0f, 0.1f, 0.0f, " dB", 3)),
               0.50, 1.0e-6, "normDefault out == 0.50");
    // Float skewed with default == centre -> 0.5 (within 1 ulp).
    check (ulpDist (normalizedDefault (floatParam ("attack", "Attack", 1.0f, 200.0f, 0.0f, 20.0f, " ms", 1, 20.0f)), 0.5f) <= 1,
           "normDefault attack ~= 0.5 (default == centre)");

    // Bool: default > 0.5 -> 1 else 0.
    check (bitEqual (normalizedDefault (boolParam ("link", "Stereo Link", true, 1)), 1.0f), "normDefault bool true == 1");
    check (bitEqual (normalizedDefault (boolParam ("delta", "Delta", false, 1)), 0.0f), "normDefault bool false == 0");

    // Choice: index / (n-1).
    checkNear (normalizedDefault (choiceParam ("quality", "Quality", { "Fast", "Normal", "High" }, 1, 2)),
               0.5, 1.0e-6, "normDefault choice 1/2 == 0.5");
    checkNear (normalizedDefault (choiceParam ("slope", "Slope",
                                               { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 2, 1)),
               2.0 / 3.0, 1.0e-6, "normDefault choice 2/3");
    checkNear (normalizedDefault (choiceParam ("mode", "Mode", { "Soft", "Hard" }, 0, 1)),
               0.0, 1.0e-6, "normDefault choice 0/1 == 0");
}

// ---------------------------------------------------------------------------
void checkText()
{
    std::printf ("4. Text format/parse round-trips\n");

    const ParamDesc depth = floatParam ("depth", "Depth", 0.0f, 100.0f, 0.1f, 30.0f, " %", 1);
    const ParamDesc out   = floatParam ("out", "Output", -24.0f, 24.0f, 0.1f, 0.0f, " dB", 3);
    const ParamDesc mode  = choiceParam ("mode", "Mode", { "Soft", "Hard" }, 0, 1);
    const ParamDesc dlt   = boolParam ("delta", "Delta", false, 1);

    // Float: fixed decimals + unit suffix.
    check (formatValue (depth, 30.0f, 1) == "30.0 %", "format depth 30.0/1dp");
    check (formatValue (depth, 42.5f, 2) == "42.50 %", "format depth 42.5/2dp");
    check (formatValue (out, -6.0f, 1) == "-6.0 dB", "format out -6.0/1dp");

    float r = 0.0f;
    check (parseValue (depth, "42.5 %", r) && std::abs (r - 42.5f) < 1e-4f, "parse '42.5 %'");
    check (parseValue (depth, "42.5", r) && std::abs (r - 42.5f) < 1e-4f, "parse '42.5' (no suffix)");
    check (parseValue (out, "-6.0 dB", r) && std::abs (r - (-6.0f)) < 1e-4f, "parse '-6.0 dB'");

    // Round-trip: format -> parse.
    for (float v : { 0.0f, 12.3f, 87.6f, 100.0f })
    {
        const std::string s = formatValue (depth, v, 2);
        check (parseValue (depth, s, r) && std::abs (r - v) < 1e-2f, "float round-trip '" + s + "'");
    }

    // Choice: label out, label or index in.
    check (formatValue (mode, 0.0f, 0) == "Soft", "format choice 0 -> Soft");
    check (formatValue (mode, 1.0f, 0) == "Hard", "format choice 1 -> Hard");
    check (parseValue (mode, "Hard", r) && bitEqual (r, 1.0f), "parse choice label 'Hard'");
    check (parseValue (mode, "0", r) && bitEqual (r, 0.0f), "parse choice index '0'");

    // Bool: On/Off out, various forms in.
    check (formatValue (dlt, 1.0f, 0) == "On", "format bool true -> On");
    check (formatValue (dlt, 0.0f, 0) == "Off", "format bool false -> Off");
    check (parseValue (dlt, "On", r) && bitEqual (r, 1.0f), "parse bool 'On'");
    check (parseValue (dlt, "off", r) && bitEqual (r, 0.0f), "parse bool 'off'");
    check (parseValue (dlt, "true", r) && bitEqual (r, 1.0f), "parse bool 'true'");
}

// ---------------------------------------------------------------------------
std::vector<ParamDesc> storeDescs()
{
    return {
        floatParam ("depth", "Depth", 0.0f, 100.0f, 0.1f, 30.0f, " %", 1),
        floatParam ("attack", "Attack", 1.0f, 200.0f, 0.0f, 20.0f, " ms", 1, 20.0f),
        boolParam ("bypass", "Bypass", false, 1, kFlagBypass),
        choiceParam ("quality", "Quality", { "Fast", "Normal", "High" }, 1, 2)
    };
}

void checkParamStore()
{
    std::printf ("5. ParamStore value/epoch/queue/sweeper\n");

    ParamStore store (storeDescs());

    // indexOf.
    check (store.indexOf ("depth") == 0, "indexOf depth == 0");
    check (store.indexOf ("quality") == 3, "indexOf quality == 3");
    check (store.indexOf ("nope") == -1, "indexOf unknown == -1");
    check (store.size() == 4, "store size == 4");

    // Value read-back: defaults, then snapped host write.
    check (bitEqual (store.value (0), 30.0f), "depth default value == 30");
    const RangeSpec rDepth = makeRange (store.desc (0));
    store.setFromHost (0, 3.14159f);
    check (bitEqual (store.value (0), snapToLegalValue (rDepth, 3.14159f)), "setFromHost snaps depth to grid");

    // Epoch bumps: host + ui write bump; gestures do not.
    const std::uint32_t e0 = store.epoch (0);
    store.setFromHost (0, 40.0f);
    check (store.epoch (0) == e0 + 1, "setFromHost bumps epoch");
    store.setFromUi (0, 41.0f);
    check (store.epoch (0) == e0 + 2, "setFromUi bumps epoch");
    const std::uint32_t e2 = store.epoch (0);
    store.beginGesture (0);
    store.endGesture (0);
    check (store.epoch (0) == e2, "gestures do not bump epoch");

    // ChangeSweeper — fresh store/sweeper so all epochs start at 0.
    {
        ParamStore s2 (storeDescs());
        ChangeSweeper sw;
        int c = 0;
        sw.sweep (s2, [&] (int) { ++c; });
        check (c == 0, "fresh sweeper sees nothing when idle");

        s2.setFromHost (2, 1.0f);
        c = 0;
        std::vector<int> seen;
        sw.sweep (s2, [&] (int i) { ++c; seen.push_back (i); });
        check (c == 1 && seen.size() == 1 && seen[0] == 2, "sweeper sees the one change exactly once");

        c = 0;
        sw.sweep (s2, [&] (int) { ++c; });
        check (c == 0, "sweeper sees nothing after (idle)");
    }

    // drainHostWrites preserves FIFO order Value / GestureBegin / GestureEnd.
    {
        ParamStore s3 (storeDescs());
        s3.setFromUi (1, 50.0f);
        s3.beginGesture (1);
        s3.endGesture (1);
        std::vector<HostWrite::Kind> kinds;
        s3.drainHostWrites ([&] (const HostWrite& w) { kinds.push_back (w.kind); });
        check (kinds.size() == 3
               && kinds[0] == HostWrite::Kind::Value
               && kinds[1] == HostWrite::Kind::GestureBegin
               && kinds[2] == HostWrite::Kind::GestureEnd,
               "drainHostWrites preserves Value/GestureBegin/GestureEnd order");
        // Draining again yields nothing.
        int extra = 0;
        s3.drainHostWrites ([&] (const HostWrite&) { ++extra; });
        check (extra == 0, "drainHostWrites empty after drain");
    }

    // Overflow counter increments when the ring is full.
    {
        ParamStore s4 (storeDescs());
        const int slots   = ParamStore::kHostWriteSlots; // ring size; usable = slots - 1
        const int pushed  = slots + 100;
        for (int i = 0; i < pushed; ++i)
            s4.beginGesture (0);
        const std::uint32_t expectedDropped = (std::uint32_t) (pushed - (slots - 1));
        check (s4.droppedHostWrites() == expectedDropped,
               "droppedHostWrites == pushed - usable (" + std::to_string (expectedDropped) + ")");
        int drained = 0;
        s4.drainHostWrites ([&] (const HostWrite&) { ++drained; });
        check (drained == slots - 1, "drained == usable slots (" + std::to_string (slots - 1) + ")");
    }
}

// ---------------------------------------------------------------------------
void checkUndoStack()
{
    std::printf ("6. UndoStack push/undo/redo, coalescing, depth bound, redo clear\n");
    using S = UndoStack::Snapshot;

    // Basic push/undo/redo.
    {
        UndoStack u;
        u.push (S { 1.0f }, 0.0);
        u.push (S { 2.0f }, 10.0); // outside window -> depth 2
        check (u.depth() == 2, "two separate pushes -> depth 2");
        check (u.canUndo() && ! u.canRedo(), "canUndo after two pushes, no redo");
        check (vecEq (u.undo (S { 2.0f }), S { 1.0f }), "undo returns previous snapshot");
        check (! u.canUndo() && u.canRedo(), "at oldest: no undo, redo available");
        check (vecEq (u.redo (S { 1.0f }), S { 2.0f }), "redo returns next snapshot");
        check (! u.canRedo(), "no redo after redoing to the top");
    }

    // Coalescing inside the window vs separate outside it.
    {
        UndoStack u;
        u.push (S { 1.0f }, 0.0);
        u.push (S { 2.0f }, 0.2); // within 0.5 s -> replaces top
        check (u.depth() == 1, "push within window coalesces (depth stays 1)");
        u.push (S { 3.0f }, 1.0); // 0.8 s later -> new entry
        check (u.depth() == 2, "push outside window adds depth");
        check (vecEq (u.undo (S { 3.0f }), S { 2.0f }), "undo returns the coalesced state");
    }

    // Depth bound at 64 (oldest dropped).
    {
        UndoStack u;
        for (int i = 0; i < 100; ++i)
            u.push (S { (float) i }, (double) i); // all outside window
        check (u.depth() == 64, "depth capped at 64");
        // Undo all the way; the last snapshot returned is the oldest KEPT (36).
        S cur { 99.0f };
        S last = cur;
        while (u.canUndo())
            last = u.undo (last);
        check (vecEq (last, S { 36.0f }), "oldest kept entry after 100 pushes is 36 (100-64)");
    }

    // Redo history cleared by a new push.
    {
        UndoStack u;
        u.push (S { 1.0f }, 0.0);
        u.push (S { 2.0f }, 10.0);
        u.push (S { 3.0f }, 20.0);
        u.undo (S { 3.0f });
        check (u.canRedo(), "redo available after an undo");
        u.push (S { 9.0f }, 30.0); // new push discards the redo tail
        check (! u.canRedo(), "new push clears the redo history");
        check (u.depth() == 3, "depth after clearing redo + push");
    }

    // clear().
    {
        UndoStack u;
        u.push (S { 1.0f }, 0.0);
        u.push (S { 2.0f }, 10.0);
        u.clear();
        check (u.depth() == 0 && ! u.canUndo() && ! u.canRedo(), "clear empties history");
    }
}
} // namespace

int main()
{
    std::printf ("factory_params unit tests\n");

    checkFnv();
    checkRange();
    checkNormalizedDefault();
    checkText();
    checkParamStore();
    checkUndoStack();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
