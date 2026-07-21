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

    inline RsNodePanelLayout computeRsNodePanelLayout (float w, float h,
                                                       bool isCut, int choiceCount)
    {
        RsNodePanelLayout L;
        L.closeBtn = { w - 28.0f, 8.0f, 18.0f, 18.0f };

        // inner reduced(14,12)
        float rx = 14.0f, ry = 12.0f, rw = w - 28.0f, rh = h - 24.0f;

        // right knob column
        const float knobW = 52.0f, kgap = 10.0f;
        const float knobsW = isCut ? knobW : knobW * 3 + kgap * 2;
        float knobsX = rx + rw - knobsW;
        rw -= (knobsW + 16.0f);
        const float knobsY = ry + 18.0f, knobsH = rh - 18.0f;
        L.freqArea = { knobsX, knobsY, knobW, knobsH };
        if (! isCut)
        {
            L.sensArea  = { knobsX + knobW + kgap, knobsY, knobW, knobsH };
            L.widthArea = { knobsX + 2.0f * (knobW + kgap), knobsY, knobW, knobsH };
        }

        // header row (26)
        float hx = rx, hy = ry;
        L.dot = { hx, hy + (26.0f - 14.0f) * 0.5f, 14.0f, 14.0f };
        hx += 18.0f + 4.0f;
        L.name = { hx, hy, 76.0f, 26.0f };
        hx += 76.0f + 8.0f;
        L.onBadge = { hx, hy + 2.0f, 40.0f, 22.0f };
        hx += 40.0f + 6.0f;
        const float listenW = std::min (90.0f, rx + rw - hx);
        L.listenBadge = { hx, hy + 2.0f, std::max (40.0f, listenW), 22.0f };

        // caption + choice row (at ry + 26 + 18)
        float cy = ry + 26.0f + 18.0f;
        L.caption = { rx, cy, (isCut ? 52.0f : 38.0f), 30.0f };
        float bx = rx + L.caption.w + 8.0f;
        const float bw = isCut ? 40.0f : 32.0f, bgap = 4.0f, bh = 27.0f;
        for (int i = 0; i < choiceCount; ++i)
        {
            L.choice[i] = { bx, cy + (30.0f - bh) * 0.5f, bw, bh };
            bx += bw + bgap;
        }
        return L;
    }
}
