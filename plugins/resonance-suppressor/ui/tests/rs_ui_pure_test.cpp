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
#include "RsFooterLayout.h"
#include "RsNodePanelLayout.h"

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

void checkNear (float actual, float expected, float tol, const std::string& what)
{
    if (! (std::abs (actual - expected) <= tol))
        fail (what + ": got " + std::to_string (actual) + ", expected "
              + std::to_string (expected) + " +/- " + std::to_string (tol));
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
// Footer columns: the round-#4 invariant is that EVERY section-content <->
// divider gap equals the single value P — the native version of the Playwright
// test-19 divider-gap guard. Checked at the design scale (k=1, 1069x747) and at
// the minimum size's fractional scale (k=657/747).
void checkFooterColumns()
{
    std::printf ("2. computeRsFooterColumns (uniform divider gaps)\n");

    for (const float k : { 1.0f, 657.0f / 747.0f })
    {
        auto S = [k] (float v) { return (float) (int) std::round (v * k); }; // the editor's scale idiom
        // The footer inner rect exactly as RsEditor::resized derives it at design
        // width 1069 (mx = S(20), footer card inset S(14)).
        const float w = 1069.0f * k, h = 747.0f * k;
        const float mx = S (20.0f), footerH = S (226.0f);
        const float fx = mx + S (14.0f), fy = ((h - mx) - footerH) + S (14.0f);
        const float fw = (w - 2.0f * mx) - 2.0f * S (14.0f), fh = footerH - 2.0f * S (14.0f);

        const rs_ui::RsFooterColumns c = rs_ui::computeRsFooterColumns (fx, fy, fw, fh, k);
        const std::string at = " (k=" + std::to_string (k) + ")";
        constexpr float tol = 1.0e-3f;

        // All five section<->divider gaps are the SAME P.
        checkNear (c.pairLeft - fx, c.gapP, tol, "left card edge -> DEPTH gap == P" + at);
        checkNear (c.div1 - (c.pairLeft + c.bigPairW), c.gapP, tol, "big pair -> div1 gap == P" + at);
        checkNear (c.trioLeft - c.div1, c.gapP, tol, "div1 -> ATK gap == P" + at);
        checkNear (c.div2 - (c.trioLeft + c.miniTrioW), c.gapP, tol, "mini trio -> div2 gap == P" + at);
        checkNear (c.modeLeft - c.div2, c.gapP, tol, "div2 -> MODE card gap == P" + at);

        // MODE card anchor is the settled fx + 0.60*fw + S(10) (col3 unchanged).
        checkNear (c.modeLeft, fx + fw * 0.60f + S (10.0f), tol, "modeLeft anchor" + at);

        // Ordering + sane dials.
        check (fx < c.div1 && c.div1 < c.div2 && c.div2 < c.modeLeft, "fx < div1 < div2 < modeLeft" + at);
        check (c.bigDia > 0.0f && c.miniDia > 0.0f && c.gapP > 0.0f, "dials + gap positive" + at);
        check (c.cyBig > fy && c.cyMini > fy, "knob cells below the footer top" + at);
    }
}

// ---------------------------------------------------------------------------
// Node panel: the width-guard collision class (RsNodePanel::resized) — at the
// intrinsic widths the right-anchored knob column must not slide over the
// left-anchored TYPE/SLOPE buttons, and the Listen chip keeps its floor width.
void checkNodePanelLayout()
{
    std::printf ("3. computeRsNodePanelLayout (intrinsic widths, no collision)\n");
    constexpr float h = 112.0f; // RsNodePanel::kHeight
    constexpr float tol = 1.0e-3f;

    struct Case { float w; bool isCut; int choices; const char* name; };
    const Case cases[] = { { 350.0f, true, 4, "cut(350)" }, { 500.0f, false, 6, "band(500)" } };
    for (const Case& cs : cases)
    {
        const rs_ui::RsNodePanelLayout L = rs_ui::computeRsNodePanelLayout (cs.w, h, cs.isCut, cs.choices);
        const std::string at = std::string (" [") + cs.name + "]";

        // Knob column right-anchored to the 14px inner inset.
        const rs_ui::RsNodePanelLayout::R& lastKnob = cs.isCut ? L.freqArea : L.widthArea;
        checkNear (lastKnob.x + lastKnob.w, cs.w - 14.0f, tol, "knob column right-anchored" + at);

        // The last choice button must clear the knob column (the reported
        // TYPE-button-under-knob overlap class).
        const rs_ui::RsNodePanelLayout::R& lastBtn = L.choice[cs.choices - 1];
        check (lastBtn.x + lastBtn.w <= L.freqArea.x + tol, "choice row clears the knob column" + at);

        // Listen chip keeps its floor width (the \"Lis…\" clip class) and the
        // header row stays ordered.
        check (L.listenBadge.w >= 40.0f - tol, "listen badge >= 40px" + at);
        check (L.dot.x < L.name.x && L.name.x < L.onBadge.x && L.onBadge.x < L.listenBadge.x,
               "header rects ordered" + at);
        check (L.closeBtn.x + L.closeBtn.w <= cs.w, "close X inside the panel" + at);
    }
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf ("resonance-suppressor pure-UI helpers — headless checks\n");
    checkParseFreqEntry();
    checkFooterColumns();
    checkNodePanelLayout();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
