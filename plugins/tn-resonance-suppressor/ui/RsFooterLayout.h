#pragma once

#include <cmath>

//
// rs_ui::RsFooterColumns — the footer's three-column algebra as a pure function of
// the footer inner rect + the uniform design scale k (= height/747), extracted
// from RsEditor::resized so the headless test (rs_ui_pure_test.cpp) can assert
// the divider-gap invariants natively (the Playwright test-19 guard). visage-free
// and JUCE-free; RsEditor::resized consumes the struct.
//
// Footer knob layout (settled design). Size-contrast ratio 1.8: big DEPTH/
// DETAIL dial 104 px, mini ATK/REL/TILT dial 57 px. 8 px vertical label gaps
// on BOTH groups — big cell = name16 + 8 + dial104 + 8 + value17 = 153, mini
// cell = name14 + 8 + dial57 + 8 + value14 = 101 (the gap falls out of the
// dial being width-limited inside the taller cell, so each dial is centred
// with an 8 px band above/below). Horizontal edge-to-edge dial gaps: DEPTH↔
// DETAIL 40, ATK↔REL↔TILT 20 (LOCKED). Each cell is exactly the dial width so
// the cell-to-cell gap IS the edge-to-edge dial gap.
//
// Round #4 fix (uniform section↔divider spacing): every section-content↔
// divider gap is a SINGLE value P — left-card-edge↔DEPTH, DEPTH/DETAIL-right↔
// footerDiv1, footerDiv1↔ATK, TILT↔footerDiv2 and footerDiv2↔MODE-card-left
// all equal — instead of the old col2 = trio + 12 pad that left the minis
// hugging both dividers at only 6 px while the big pair had ~65 px to div1.
// The MODE card (col3) is UNCHANGED: its left edge stays at the old
// fx + 0.60*fw + S(10), so only the two dividers move to equalise. P falls out
// of the width budget (bigs centred in col1 with P margins, minis in col2):
//   modeLeft = fx + P + bigPairW + P + P + miniTrioW + P + P
//   => P = ((modeLeft - fx) - bigPairW - miniTrioW) / 5.
// All lengths scale with S().
//
namespace rs_ui
{
    struct RsFooterColumns
    {
        float bigDia = 0, miniDia = 0;       // dial diameters (== cell widths)
        float bigCellH = 0, miniCellH = 0;   // knob cell heights
        float bigGap = 0, miniGap = 0;       // edge-to-edge dial gaps (locked)
        float bigPairW = 0, miniTrioW = 0;   // group widths
        float modeLeft = 0;                  // MODE card left edge (col3 anchor)
        float gapP = 0;                      // THE single uniform section<->divider gap
        float div1 = 0, div2 = 0;            // column dividers
        float pairLeft = 0, trioLeft = 0;    // group left edges
        float cyBig = 0, cyMini = 0;         // knob cell tops (vertically centred)
    };

    inline RsFooterColumns computeRsFooterColumns (float fx, float fy, float fw, float fh, float k)
    {
        auto S = [k] (float v) { return (float) (int) std::round (v * k); }; // == RsEditor::S

        RsFooterColumns c;
        c.bigDia    = S (104.0f);
        c.miniDia   = S (57.0f);
        c.bigCellH  = S (153.0f);
        c.miniCellH = S (101.0f);
        c.bigGap    = S (40.0f);
        c.miniGap   = S (20.0f);
        c.bigPairW  = 2.0f * c.bigDia + c.bigGap;
        c.miniTrioW = 3.0f * c.miniDia + 2.0f * c.miniGap;
        c.modeLeft  = fx + fw * 0.60f + S (10.0f);              // MODE card left — unchanged (== old col3 cx)
        c.gapP      = ((c.modeLeft - fx) - c.bigPairW - c.miniTrioW) / 5.0f;
        c.div1      = fx + c.gapP + c.bigPairW + c.gapP;        // bigs centred in col1 with P margins
        c.div2      = c.div1 + c.gapP + c.miniTrioW + c.gapP;   // minis centred in col2 (== modeLeft - P)
        c.pairLeft  = fx + c.gapP;
        c.trioLeft  = c.div1 + c.gapP;
        const float chFull = fh - 2.0f * S (6.0f);
        c.cyBig  = fy + S (6.0f) + (chFull - c.bigCellH) * 0.5f;  // stacks vertically centred
        c.cyMini = fy + S (6.0f) + (chFull - c.miniCellH) * 0.5f;
        return c;
    }
}
