#pragma once

#include <algorithm>

//
// rs_ui::RsNodePanelLayout — the node panel's rect arithmetic as a pure function
// of (w, h, isCut, choiceCount), extracted from RsNodePanel::computeLayout so the
// headless test (rs_ui_pure_test.cpp) can assert the layout invariants (right-
// anchored knob column, no TYPE-button collision, Listen width floor) without
// visage. The member computeLayout() copies these results into the frame's rects.
// visage-free and JUCE-free.
//
namespace rs_ui
{
    struct RsNodePanelLayout
    {
        struct R { float x = 0, y = 0, w = 0, h = 0; };
        R closeBtn, dot, name, onBadge, listenBadge, caption;
        R choice[6];                    // first `choiceCount` valid
        R freqArea, sensArea, widthArea; // sens/width valid for bands only (!isCut)
    };

    // `scale` (== the editor's k() = height/747) multiplies every intrinsic px so
    // the panel's inner rects shrink with the window; the caller passes the panel's
    // ALREADY-SCALED (w, h). At scale == 1.0 this is the design-size layout.
    inline RsNodePanelLayout computeRsNodePanelLayout (float w, float h,
                                                       bool isCut, int choiceCount,
                                                       float scale = 1.0f)
    {
        const float s = scale > 0.0f ? scale : 1.0f;
        RsNodePanelLayout L;
        L.closeBtn = { w - 28.0f * s, 8.0f * s, 18.0f * s, 18.0f * s };

        // inner reduced(14,12)
        float rx = 14.0f * s, ry = 12.0f * s, rw = w - 28.0f * s, rh = h - 24.0f * s;

        // right knob column
        const float knobW = 52.0f * s, kgap = 10.0f * s;
        const float knobsW = isCut ? knobW : knobW * 3 + kgap * 2;
        float knobsX = rx + rw - knobsW;
        rw -= (knobsW + 16.0f * s);
        const float knobsY = ry + 18.0f * s, knobsH = rh - 18.0f * s;
        L.freqArea = { knobsX, knobsY, knobW, knobsH };
        if (! isCut)
        {
            L.sensArea  = { knobsX + knobW + kgap, knobsY, knobW, knobsH };
            L.widthArea = { knobsX + 2.0f * (knobW + kgap), knobsY, knobW, knobsH };
        }

        // header row (26)
        float hx = rx, hy = ry;
        L.dot = { hx, hy + (26.0f - 14.0f) * 0.5f * s, 14.0f * s, 14.0f * s };
        hx += (18.0f + 4.0f) * s;
        L.name = { hx, hy, 76.0f * s, 26.0f * s };
        hx += (76.0f + 8.0f) * s;
        L.onBadge = { hx, hy + 2.0f * s, 40.0f * s, 22.0f * s };
        hx += (40.0f + 6.0f) * s;
        const float listenW = std::min (90.0f * s, rx + rw - hx);
        L.listenBadge = { hx, hy + 2.0f * s, std::max (40.0f * s, listenW), 22.0f * s };

        // caption + choice row (at ry + 26 + 18)
        float cy = ry + (26.0f + 18.0f) * s;
        L.caption = { rx, cy, (isCut ? 52.0f : 38.0f) * s, 30.0f * s };
        float bx = rx + L.caption.w + 8.0f * s;
        const float bw = (isCut ? 40.0f : 32.0f) * s, bgap = 4.0f * s, bh = 27.0f * s;
        for (int i = 0; i < choiceCount; ++i)
        {
            L.choice[i] = { bx, cy + (30.0f * s - bh) * 0.5f, bw, bh };
            bx += bw + bgap;
        }
        return L;
    }
}
