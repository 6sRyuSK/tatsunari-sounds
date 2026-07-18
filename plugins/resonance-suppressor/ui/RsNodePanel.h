#pragma once

#include "RsTheme.h"
#include "RsFeed.h"
#include "RsProfileModel.h"

#include "factory_params/Range.h"

#include <visage_ui/frame.h>
#include <visage_graphics/path.h>

#include <array>
#include <functional>
#include <string>

//
// rs_ui::RsNodePanel — the small, bright inline editor for the selected reduction
// node, ported from NodePanel onto a visage::Frame. It floats over the analyser
// (positioned by the editor) and rebinds to the selected node (Pro-Q style). Fixed
// size (kHeight × preferredWidth), it lays out:
//   * a header row: node-identity dot + name + ON badge + Listen badge + close X;
//   * a TYPE row (6 filter-type glyph buttons, bands) or SLOPE row (4 dB/oct
//     labels, cuts);
//   * FREQ [+ SENS + WIDTH] compact mini-knobs (SENS/WIDTH bands only) in the
//     right column.
// Everything is drawn + hit-tested in this one frame (the badges/buttons are the
// same hand-painted idiom as the shipped editor's bespoke Button subclasses).
// Edits ride the ParamStore gesture path; Listen writes RsFeed::setListenNode.
// GUI-thread only.
//
namespace rs_ui
{
    class RsNodePanel : public visage::Frame
    {
    public:
        RsNodePanel (const RsTheme& theme, RsProfileModel& model, RsFeed& feed);

        static constexpr int kHeight = 112;
        int preferredWidth() const noexcept { return isCut_ ? 350 : 500; }
        int currentNode() const noexcept { return nodeId_; }

        // The X was pressed (the editor deselects — which also drops Listen).
        std::function<void()> onCloseRequested;
        // A node param changed via the panel (the editor refreshes the curve).
        std::function<void (int)> onNodeEdited;
        // A discrete edit finished (the editor snapshots undo).
        std::function<void()> onGestureEnd;

        // Rebind to node `id` (0 = low cut, 1 = high cut, 2.. = band).
        void setNode (int id);
        // Re-sync the Listen toggle to the feed's live solo target.
        void refreshListen();

        // Needle centre + tip (WINDOW px) of a mini-knob (0 = FREQ, 1 = SENS,
        // 2 = WIDTH), computed through the SAME shared value->angle->tip helpers
        // the draw path uses — the Playwright driver asserts a known value's
        // needle-tip angle against an independent oracle (A2). False if hidden.
        bool miniKnobTipInWindow (int which, float& cx, float& cy, float& tx, float& ty) const;

        // Mini-knob value-ring geometry in WINDOW px (centre + ring centreline
        // radius) — the driver pixel-samples the ring at the needle angle to assert
        // the accent arc END lines up with the needle (catches an arc-vs-needle
        // angular divergence like the old 90°-off mini arc; round-3 fix 6).
        bool miniKnobDialInWindow (int which, float& cx, float& cy, float& arcR) const;

        void draw (visage::Canvas& canvas) override;
        void resized() override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;

    private:
        struct Rect { float x = 0, y = 0, w = 0, h = 0; bool contains (visage::Point p) const
        { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; } };

        struct MiniKnob
        {
            Rect area;
            int  paramIndex = -1;
            std::string label;
            int  decimals = 0;
            bool freq = false;         // kHz/Hz formatting
            bool visible = false;
            factory_params::RangeSpec range;
        };

        // Shared mini-knob dial geometry (centre / radii / needle length + angle),
        // so drawMiniKnob() and miniKnobTipInWindow() can never disagree on where
        // the needle points.
        struct MiniDial { float cx, cy, R, band, arcR, bodyR, len, toAng; };
        MiniDial miniKnobDial (const MiniKnob& k) const;

        void computeLayout();
        void drawMiniKnob (visage::Canvas& canvas, const MiniKnob& k);
        std::string valueText (const MiniKnob& k) const;
        void writeKnob (MiniKnob& k, float norm);

        std::string nodeName() const;
        std::uint32_t nodeColour() const;

        const RsTheme&  theme_;
        RsProfileModel& model_;
        RsFeed&         feed_;

        int  nodeId_ = 0;
        bool isCut_  = true;
        bool listenOn_ = false;

        // layout rects (frame-local)
        Rect dotRect_, nameRect_, onBadge_, listenBadge_, closeBtn_, captionRect_;
        std::array<Rect, 6> choiceBtns_ {}; // 6 type (bands) or first 4 = slope (cuts)
        int  choiceCount_ = 6;
        MiniKnob freqK_, sensK_, widthK_;

        // active drag target: 0=freq,1=sens,2=width,-1=none
        int  dragKnob_ = -1;
        float dragNorm_ = 0.0f;
        visage::Point dragLast_;
    };
}
