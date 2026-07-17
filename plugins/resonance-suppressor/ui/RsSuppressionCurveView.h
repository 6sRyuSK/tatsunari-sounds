#pragma once

#include "RsTheme.h"
#include "RsFeed.h"
#include "RsProfileModel.h"

#include <visage_ui/frame.h>

#include <functional>
#include <vector>

//
// rs_ui::RsSuppressionCurveView — the resonance-suppressor analyser, ported from
// SuppressionCurveComponent onto a visage::Frame. It draws (bottom-to-top):
//   * the warm plot card + non-uniform log-frequency grid (20-500 Hz / 500-9k /
//     9k-20k get 30% / 55% / 15% of the width — freqToT/tToFreq, verbatim);
//   * the PRE (input, filled area) and POST (output, line) spectra from RsFeed;
//   * the teal reduction "curtain" hanging from the 0 dB line (RsFeed redDb);
//   * the reduction-profile curve — per-node translucent fills under a glowing
//     combined stroke, evaluated with factory_core::reductionProfileDbAt (the same
//     shape functions the audio rasteriser uses) from RsProfileModel;
//   * the node handles: 8 band dots + 2 cut squares, positioned from the params,
//     with the selected node grown, the hovered node highlighted, and the soloed
//     (Listen) node ringed teal;
//   * the Pre/Post/Both mode chip, the Freeze chip and the GR peak-hold badge.
//
// Interactions (mirroring the JUCE component): click a node to select it (opens the
// node panel via onSelectNode) and drag it — x = freq, y = sens (bands); Shift =
// fine (0.15x). Double-click a band resets its sens to 0; double-click empty space
// enables the lowest OFF band there. The wheel adjusts a cut's slope / a band's
// width. Edits go through the ParamStore gesture path (so the editor's undo sees
// them). Listen is written through RsFeed::setListenNode (by the node panel /
// right-click). GUI-thread only; the RsFeed atomics are the audio-thread hand-off.
//
namespace rs_ui
{
    class RsSuppressionCurveView : public visage::Frame
    {
    public:
        RsSuppressionCurveView (const RsTheme& theme, RsProfileModel& model, RsFeed& feed);

        // Node selected on the curve (-1 = deselect). The editor shows/positions
        // the node panel in response.
        std::function<void (int)> onSelectNode;
        // A node's parameters changed via a curve gesture (drag/wheel/double-click)
        // — lets the editor refresh an open node panel + capture undo on gesture end.
        std::function<void (int)> onNodeEdited;
        // A curve gesture ended (mouseUp / one-shot write) — the editor snapshots
        // undo here.
        std::function<void()> onGestureEnd;
        // Called once per animated frame before reading the feed (not when frozen)
        // — the owner advances the synthetic/real feed here (SpectrumView pattern).
        std::function<void()> onTick;

        void setSelectedNode (int id); // editor -> view (keep the grown handle in sync)
        int  selectedNode() const noexcept { return selectedNode_; }

        // Freeze the analyser for deterministic capture (snapshots the current feed
        // frame and holds it; stops the redraw loop).
        void setFrozen (bool frozen);
        bool frozen() const noexcept { return frozen_; }

        // Node handle centre in window px (for the Playwright driver). False if the
        // node is a hidden (off) band.
        bool nodeCentreInWindow (int id, float& x, float& y) const;
        // Analyser plot rect in window px (for region screenshots).
        bool plotRectInWindow (float& x, float& y, float& w, float& h) const;

        void draw (visage::Canvas& canvas) override;
        void resized() override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;
        void mouseMove (const visage::MouseEvent& e) override;
        void mouseExit (const visage::MouseEvent& e) override;
        bool mouseWheel (const visage::MouseEvent& e) override;

    private:
        struct Rect { float x = 0, y = 0, w = 0, h = 0; };

        // --- axes (verbatim from SuppressionCurveComponent) --------------------
        static float freqToT (float f);
        static float tToFreq (float t);
        float freqToX (float f) const;
        float xToFreq (float x) const;
        float dbToY (float db) const;   // analyser dB axis (kMinDb..kMaxDb)
        float sensToY (float s) const;  // profile sens axis (kSensMin..kSensMax)
        float yToSens (float y) const;
        visage::Point nodePos (int id) const;

        // --- layers ------------------------------------------------------------
        void computeLayout();
        void snapshotFeed();
        void drawPlotCard (visage::Canvas&);
        void drawGrid (visage::Canvas&);
        void drawAnalyzer (visage::Canvas&);
        void drawReduction (visage::Canvas&);
        void drawProfile (visage::Canvas&);
        void drawNodes (visage::Canvas&);
        void drawHeaderControls (visage::Canvas&);
        void drawTooltip (visage::Canvas&);

        // --- interaction helpers ----------------------------------------------
        int  nodeAt (visage::Point pos) const;
        void beginGesture (int id);
        void endGesture (int id);
        void setParam (int paramIndex, float value);           // held-drag write (setFromUi)
        void setParamGestured (int paramIndex, float value);   // one-shot begin/set/end
        void addBandAt (visage::Point pos);
        void updateGrHold (float instantDb);

        // band colour for node id (id>=2) from the theme band palette.
        std::uint32_t bandColour (int id) const;

        enum class AnalyzerMode { Both, Pre, Post };
        static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;

        const RsTheme&  theme_;
        RsProfileModel& model_;
        RsFeed&         feed_;

        // layout (frame-local)
        Rect plot_, controlsRow_, modeChip_, freezeChip_, grBadge_;

        int  selectedNode_ = -1;
        int  dragging_ = -1;
        int  hoverNode_ = -1;
        visage::Point dragAnchor_, dragVirtual_;

        AnalyzerMode analyzerMode_ = AnalyzerMode::Both;
        bool frozen_ = false;

        // frozen snapshot of the feed (held image)
        std::vector<float> snapPre_, snapPost_, snapRed_;
        int    snapBins_ = 0;
        double snapSr_ = 48000.0;

        // GR peak-hold badge
        float grPeakDb_ = 0.0f;
        int   grHold_ = 0;
    };
}
