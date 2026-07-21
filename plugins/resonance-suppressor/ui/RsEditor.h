#pragma once

#include "RsTheme.h"
#include "RsFeed.h"
#include "RsFooterLayout.h"
#include "RsModels.h"
#include "RsProfileModel.h"
#include "RsPillCell.h"
#include "RsSuppressionCurveView.h"
#include "RsNodePanel.h"

#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/Segmented.h"
#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/LinkSlider.h"
#include "factory_ui_visage/IconButton.h"
#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_ui_visage/ValueEntry.h"
#include "factory_params/ParamStore.h"
#include "factory_params/UndoStack.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

//
// rs_ui::RsEditor — the resonance-suppressor Visage editor, the full port of the
// JUCE ResonanceSuppressorAudioProcessorEditor. It composes factory_ui_visage
// widgets (Knob / Segmented / ValueSetting / LinkSlider / IconButton /
// PresetSelectorView / Dropdown) plus the RS-specific views (RsSuppressionCurveView,
// RsNodePanel, RsPillCell) and reproduces the shipped layout: a header row
// (brand / preset pill / A|B / Copy / Undo / Redo / Bypass), the big analyser in
// the middle, and a three-column footer card (Depth/Detail big knobs · Atk/Rel/Tilt
// small knobs · MODE + settings). Every control binds by string id from
// buildRsParams(); the legacy sharpness/selectivity params get no UI (as today).
//
// It is JUCE-free: it binds to a factory_params::ParamStore + an RsFeed + the
// RsPresetModel / RsAbModel callback interfaces (harness mocks now, real in P4).
// Undo/redo is wired for real against factory_params::UndoStack: the editor drains
// the store's host-write queue each frame and snapshots on gesture-end (500 ms
// coalescing, time injected from a frame clock); a preset load or A/B switch clears
// the timeline, matching JUCE's apvts.replaceState() semantics.
//
namespace rs_ui
{
    // Bottom-right drag handle (JUCE editor parity: setResizable's corner
    // resizer). Hosts that give a plugin window no OS resize edge — Logic's AU
    // window — can only resize through an affordance INSIDE the view, so the
    // editor draws its own. It reports raw drag-proposed sizes (editor-logical
    // px); the editor snaps/clamps them and relays via onResizeRequest.
    class RsResizeGrip : public visage::Frame
    {
    public:
        std::function<void (float w, float h)> onDragResize; // raw proposal, logical px

        void setLineColour (std::uint32_t argb) noexcept { lineColour_ = argb; }

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;

    private:
        visage::Point downPos_ {};
        float startW_ = 0.0f, startH_ = 0.0f;
        std::uint32_t lineColour_ = 0;
    };

    class RsEditor : public visage::Frame
    {
    public:
        // Design geometry (logical px): 1069x747 reference layout (k() scales off
        // the height), resize limits 940x657..1320x922 — the JUCE editor's
        // setResizeLimits, also mirrored by the CLAP shell's adjustSize.
        static constexpr float kDesignW = 1069.0f, kDesignH = 747.0f;
        static constexpr float kMinW = 940.0f, kMinH = 657.0f, kMaxW = 1320.0f, kMaxH = 922.0f;

        RsEditor (const RsTheme& theme, factory_params::ParamStore& store, RsFeed& feed,
                  RsPresetModel& presets, RsAbModel& ab);

        // Host-window resize relay (editor-logical px, already aspect-snapped and
        // clamped to the limits above). Set by the CLAP shell; when unset (the
        // ui-dev harness) the corner grip is hidden.
        std::function<void (float w, float h)> onResizeRequest;

        // Snap a proposed size onto the 1069:747 aspect (height-driven) and clamp
        // it into [kMin .. kMax] — the same maths as the shell's adjustSize.
        static void snapWindowSize (float& w, float& h);

        // --- harness / bridge surface -----------------------------------------
        factory_params::ParamStore&    store() noexcept { return store_; }
        const factory_ui_visage::Theme& theme() const noexcept { return rsTheme_.base; }
        RsSuppressionCurveView&        curve() noexcept { return *curve_; }
        RsNodePanel&                   nodePanel() noexcept { return *nodePanel_; }
        factory_ui_visage::Dropdown*   dropdown() noexcept { return dropdown_.get(); }

        // Merge a fresh theme-rs.json overlay onto the shared default and re-apply.
        bool reloadTheme (const std::string& overlayJson, std::string& error);

        // Freeze the analyser for deterministic capture.
        void setFrozen (bool frozen);
        bool frozen() const noexcept { return frozen_; }

        // Drain the store's gesture queue and snapshot undo on gesture-end. Called
        // each frame (draw) and explicitly by the bridge after a simulated edit.
        void pumpGestures();

        // Undo / redo (message-thread; the buttons + the bridge call these).
        void doUndo();
        void doRedo();
        bool canUndo() const noexcept { return undo_.canUndo(); }
        bool canRedo() const noexcept { return undo_.canRedo(); }

        // Inject a fixed clock value (seconds) for deterministic undo coalescing in
        // tests; pass a negative value to restore the real monotonic clock.
        void setClockOverride (double seconds) noexcept { clockOverride_ = seconds; }

        // Preset / A-B (the selector + A|B strip call these; also the bridge).
        void loadPreset (int itemIndex);
        void setAbSlot (int slot);
        void copyAb();
        int  abSlot() const;
        int  presetIndex() const;

        // Node selection (curve <-> panel) is internal, but exposed for the driver.
        int  selectedNode() const noexcept { return curve_ ? curve_->selectedNode() : -1; }
        void openNode (int id) { selectNode (id); }                 // driver: select a node
        bool openNamedDropdown (int which);                          // 0=quality,1=channel,2=preset

        // Rect (window px) of a control keyed by param id or a special name
        // ("preset" / "curve" / "ab" / "undo" / "redo" / "copy" / "bypass").
        bool widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const;
        bool nodeCentreInWindow (int id, float& x, float& y) const;
        // Needle centre + tip (window px) of an OPEN node panel's mini-knob
        // (0=FREQ, 1=SENS, 2=WIDTH) — the A2 needle-angle assert hook.
        bool miniKnobTipInWindow (int which, float& cx, float& cy, float& tx, float& ty) const;
        // Mini-knob ring geometry (window px) for the arc-vs-needle assert (fix 6).
        bool miniKnobDialInWindow (int which, float& cx, float& cy, float& arcR) const
        { return nodePanel_ && nodePanel_->isVisible() && nodePanel_->miniKnobDialInWindow (which, cx, cy, arcR); }
        // Active Pre/Post/Both segment (0/1/2) for the per-segment assert (fix 8).
        int  analyzerModeSegment() const { return curve_ ? curve_->analyzerModeSegment() : 2; }
        // Direct text-entry overlay state (for the driver): is it open + its text +
        // a node-panel mini's value read-out rect (window px) to double-click.
        bool valueEntryOpen() const;
        std::string valueEntryText() const;
        bool miniValueRectInWindow (int which, float& x, float& y, float& w, float& h) const
        { return nodePanel_ && nodePanel_->isVisible() && nodePanel_->miniValueRectInWindow (which, x, y, w, h); }
        bool plotRectInWindow (float& x, float& y, float& w, float& h) const;
        void plotRectInWindowLocal (float& x, float& y, float& w, float& h) const; // editor-frame-local

        void draw (visage::Canvas& canvas) override;
        void resized() override;
        void mouseDown (const visage::MouseEvent& e) override;

    private:
        struct Rect { float x = 0, y = 0, w = 0, h = 0; bool contains (visage::Point p) const
        { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; } };

        double now() const;
        std::vector<float> snapshotNow() const;
        void   commitUndo();
        void   applyHistory (const std::vector<float>& snap);
        void   onStateReplaced();      // clear undo + refresh after preset / A-B load
        void   updateAbUi();           // sync the directional copy button to the active slot (fix 4)
        void   refreshUndoButtons();
        void   selectNode (int id);    // curve -> panel
        void   rebuildPresetMenu();
        void   presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items, int sel,
                                visage::Frame* anchor, std::function<void (int)> onSelect);
        void   onValueEntryRequest (const factory_ui_visage::ValueEntryRequest& req); // window->local, open

        // resized() steps, in call order (each moves the same code it replaced —
        // no arithmetic changes). layoutHeaderRight returns the running right
        // bound the preset pill may extend to.
        float  layoutHeaderRight (float ix, float iw, float headerY, float headerH);
        void   layoutHeaderBrandAndPreset (float ix, float headerY, float headerH, float presetRight);
        void   layoutCurveAndFooterCard (float ix, float iw, float headerY, float headerH, float h, int mx);
        void   layoutFooterKnobs (const RsFooterColumns& c);
        void   layoutFooterSettings (float fx, float fy, float fw, float fh, float modeLeft);

        void   drawBrand (visage::Canvas& canvas);
        void   drawHeaderChrome (visage::Canvas& canvas);
        void   drawFooterChrome (visage::Canvas& canvas);

        int    idx (const char* id) const { return store_.indexOf (id); }
        float  k() const { return height() / kDesignH; }  // uniform design scale
        int    S (float v) const { return (int) std::round (v * k()); }

        RsTheme rsTheme_;                 // owned; mutated in place on hot reload
        factory_params::ParamStore& store_;
        RsFeed& feed_;
        RsPresetModel& presets_;
        RsAbModel& ab_;
        RsProfileModel model_;

        // children
        std::vector<std::unique_ptr<factory_ui_visage::Knob>> knobs_;
        std::unique_ptr<factory_ui_visage::Segmented>   modeSeg_;
        std::unique_ptr<factory_ui_visage::ValueSetting> qualitySet_, chSet_;
        std::unique_ptr<factory_ui_visage::LinkSlider>  linkAmt_, mix_, out_;
        std::vector<std::unique_ptr<RsPillCell>>        pills_; // delta, scEnable, scListen, link
        std::unique_ptr<RsPillCell>                     bypass_;
        std::unique_ptr<factory_ui_visage::IconButton>  undoBtn_, redoBtn_, copyBtn_;
        std::unique_ptr<factory_ui_visage::PresetSelectorView> preset_;
        std::unique_ptr<factory_ui_visage::Dropdown>    dropdown_;
        std::unique_ptr<factory_ui_visage::ValueEntry>  valueEntry_; // shared direct-text-entry overlay
        std::unique_ptr<RsSuppressionCurveView>         curve_;
        std::unique_ptr<RsNodePanel>                    nodePanel_;
        std::unique_ptr<RsResizeGrip>                   grip_;

        // undo
        factory_params::UndoStack undo_;
        std::vector<float> lastSnap_;
        bool   applyingHistory_ = false;
        bool   inGesture_ = false;
        double clockOverride_ = -1.0;
        // Last-seen gesture-end count (a non-consuming observer of the store): the
        // undo coalescer must NOT drain the store's host-write queue, because the
        // CLAP shell is that queue's single consumer (it relays GUI edits to the
        // host as automation). pumpGestures() snapshots on each new gesture-end.
        std::uint32_t lastGestureEnd_ = 0;

        bool frozen_ = false;

        // chrome rects (frame-local, computed in resized()/draw()).
        Rect brandRect_, abStrip_, bypassLabel_, footerCard_, modeCell_;
        float footerDiv1_ = 0, footerDiv2_ = 0;
        struct PillChrome { Rect bounds; };
        std::vector<Rect> pillCellRects_;
    };
}
