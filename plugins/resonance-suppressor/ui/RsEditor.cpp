#include "RsEditor.h"

#include "RsIcons.h"
#include "factory_ui_visage/Chrome.h" // paintCardShell / paintHairline
#include "factory_ui_visage/Icons.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rs_ui
{
    namespace fuv = factory_ui_visage;
    using fuv::regularFont;
    using fuv::boldFont;

    RsEditor::RsEditor (const RsTheme& theme, factory_params::ParamStore& store, RsFeed& feed,
                        RsPresetModel& presets, RsAbModel& ab)
        : rsTheme_ (theme), store_ (store), feed_ (feed), presets_ (presets), ab_ (ab), model_ (store),
          sweeper_ (store)
    {
        const fuv::Theme& base = rsTheme_.base;

        // --- knobs -----------------------------------------------------------
        struct KnobDef { const char* id; const char* label; std::uint32_t accent; int decimals; };
        const KnobDef kdefs[] = {
            { "depth",   "DEPTH",  0,             0 },
            { "detail",  "DETAIL", 0,             0 },
            { "attack",  "ATK",    rsTheme_.rs.amber, 2 },
            { "release", "REL",    rsTheme_.rs.amber, 2 },
            { "tilt",    "TILT",   rsTheme_.rs.mint,  0 },
        };
        int ki = 0;
        for (const KnobDef& d : kdefs)
        {
            auto knob = std::make_unique<fuv::Knob> (store_, idx (d.id), base, d.decimals);
            knob->setNameOverride (d.label);
            if (d.accent != 0) knob->setAccentColour (d.accent);
            // Match the shipped JUCE RsKnob proportions (round-3 fixes 2 + 7): the
            // first two (DEPTH / DETAIL) are the big knobs, the rest (ATK/REL/TILT)
            // are the minis. big = name-row 16 / value-row 17, fonts 12 / 13; small =
            // rows 14 / 14, fonts 10 / 11; both fill the cell (0 dial inset) so the
            // dial diameters equal the JUCE ones and the minis stop reading too small.
            if (ki < 2) knob->setDialProfile (16.0f, 17.0f, 12.0f, 13.0f, 0.0f);
            else        knob->setDialProfile (14.0f, 14.0f, 10.0f, 11.0f, 0.0f);
            knob->requestValueEntry = [this] (const fuv::ValueEntryRequest& r) { onValueEntryRequest (r); };
            addChild (knob.get());
            knobs_.push_back (std::move (knob));
            ++ki;
        }

        // --- MODE + settings -------------------------------------------------
        modeSeg_ = std::make_unique<fuv::Segmented> (store_, idx ("mode"), base);
        modeSeg_->setGlyphs ({ fuv::icons::modeSoft(), fuv::icons::modeHard() }); // Soft bell / Hard square
        modeSeg_->setLabelFontPx (12.0f); // JUCE RsSegmented text is 12 px (round-3 fix 2)
        addChild (modeSeg_.get());

        qualitySet_ = std::make_unique<fuv::ValueSetting> (store_, idx ("quality"), base, fuv::icons::quality(), "QUALITY");
        qualitySet_->requestDropdown = [this] (std::vector<fuv::Dropdown::Item> it, int s, visage::Frame* a, std::function<void (int)> f)
        { presentDropdown (std::move (it), s, a, std::move (f)); };
        addChild (qualitySet_.get());

        chSet_ = std::make_unique<fuv::ValueSetting> (store_, idx ("channelMode"), base, fuv::icons::channel(), "CH");
        chSet_->requestDropdown = [this] (std::vector<fuv::Dropdown::Item> it, int s, visage::Frame* a, std::function<void (int)> f)
        { presentDropdown (std::move (it), s, a, std::move (f)); };
        addChild (chSet_.get());

        linkAmt_ = std::make_unique<fuv::LinkSlider> (store_, idx ("linkAmt"), base, "STEREO LINK", fuv::icons::link(), 0);
        mix_     = std::make_unique<fuv::LinkSlider> (store_, idx ("mix"), base, "MIX", 0);
        out_     = std::make_unique<fuv::LinkSlider> (store_, idx ("out"), base, "OUT", 1);
        // Caption-column widths per the JUCE RsLinkSlider (round-3 fix 3): STEREO
        // LINK 86, MIX / OUT 34. The shared 76 px theme default was too wide for the
        // short MIX / OUT captions, eating into their track (the user's "narrower
        // than the original" report); 34 restores the JUCE track width.
        linkAmt_->setCaptionColumnPx (86.0f);
        mix_->setCaptionColumnPx (34.0f);
        out_->setCaptionColumnPx (34.0f);
        for (fuv::LinkSlider* s : { linkAmt_.get(), mix_.get(), out_.get() })
            s->requestValueEntry = [this] (const fuv::ValueEntryRequest& r) { onValueEntryRequest (r); };
        addChild (linkAmt_.get()); addChild (mix_.get()); addChild (out_.get());

        // --- pill cells ------------------------------------------------------
        struct PillDef { const char* id; fuv::icons::Glyph glyph; const char* cap; std::uint32_t on; };
        pills_.push_back (std::make_unique<RsPillCell> (store_, idx ("delta"),    rsTheme_, rs_ui::icons::delta(),     "DELTA",     rsTheme_.base.palette.accent, true));
        pills_.push_back (std::make_unique<RsPillCell> (store_, idx ("scEnable"), rsTheme_, rs_ui::icons::sidechain(), "S-CHAIN",   rsTheme_.base.palette.positive, true));
        pills_.push_back (std::make_unique<RsPillCell> (store_, idx ("scListen"), rsTheme_, fuv::icons::listen(),      "SC LISTEN", rsTheme_.base.palette.positive, true));
        pills_.push_back (std::make_unique<RsPillCell> (store_, idx ("link"),     rsTheme_, fuv::icons::link(),        "LINK",      rsTheme_.base.palette.accent, true));
        for (auto& p : pills_) addChild (p.get());

        bypass_ = std::make_unique<RsPillCell> (store_, idx ("bypass"), rsTheme_, fuv::icons::Glyph {}, "Bypass",
                                                rsTheme_.base.palette.positive, /*card*/ false, /*hasGlyph*/ false);
        addChild (bypass_.get());

        // --- header buttons --------------------------------------------------
        undoBtn_ = std::make_unique<fuv::IconButton> (base, fuv::icons::undo(), fuv::IconButton::Mode::momentary);
        undoBtn_->onClick = [this] { doUndo(); };
        redoBtn_ = std::make_unique<fuv::IconButton> (base, fuv::icons::redo(), fuv::IconButton::Mode::momentary);
        redoBtn_->onClick = [this] { doRedo(); };
        copyBtn_ = std::make_unique<fuv::IconButton> (base, fuv::icons::copy(), fuv::IconButton::Mode::momentary);
        copyBtn_->onClick = [this] { copyAb(); };
        addChild (undoBtn_.get()); addChild (redoBtn_.get()); addChild (copyBtn_.get());
        updateAbUi(); // put the copy button in directional "A->B" mode, synced to the slot (fix 4)

        // --- preset selector -------------------------------------------------
        preset_ = std::make_unique<fuv::PresetSelectorView> (base);
        preset_->requestDropdown = [this] (std::vector<fuv::Dropdown::Item> it, int s, visage::Frame* a, std::function<void (int)> f)
        { presentDropdown (std::move (it), s, a, std::move (f)); };
        preset_->onChange = [this] (int i) { loadPreset (i); };
        addChild (preset_.get());
        rebuildPresetMenu();

        // --- analyser + node panel ------------------------------------------
        curve_ = std::make_unique<RsSuppressionCurveView> (rsTheme_, model_, feed_);
        curve_->onSelectNode = [this] (int id) { selectNode (id); };
        curve_->onNodeEdited = [this] (int id) { if (nodePanel_ && nodePanel_->isVisible() && nodePanel_->currentNode() == id) nodePanel_->redraw(); };
        addChild (curve_.get());

        nodePanel_ = std::make_unique<RsNodePanel> (rsTheme_, model_, feed_);
        nodePanel_->onCloseRequested = [this] { selectNode (-1); };
        nodePanel_->onNodeEdited = [this] (int) { if (curve_) curve_->redraw(); };
        nodePanel_->requestValueEntry = [this] (const fuv::ValueEntryRequest& r) { onValueEntryRequest (r); };
        nodePanel_->setVisible (false);
        addChild (nodePanel_.get());

        // --- resize grip (bottom-right; hidden when no shell relay is set) ---
        grip_ = std::make_unique<RsResizeGrip>();
        grip_->setLineColour (base.palette.textDim);
        grip_->setCursorStyle (visage::MouseCursor::MultiDirectionalResize);
        grip_->onDragResize = [this] (float w, float h)
        {
            if (! onResizeRequest) return;
            // The grip drags in editor-DESIGN units (the logical plane is fixed at
            // 1069x747 and uniform-zoomed by the window), so a design-space proposal
            // maps to a WINDOW size by the current zoom (windowScale_ = window px per
            // design unit). Snap in window space against [kMin .. kMax], then relay.
            w *= windowScale_;
            h *= windowScale_;
            snapWindowSize (w, h);
            onResizeRequest (w, h);
        };
        grip_->setVisible (false); // resized() shows it once a relay is wired
        addChild (grip_.get());

        // --- shared overlays (on top) ----------------------------------------
        dropdown_ = std::make_unique<fuv::Dropdown> (base);
        dropdown_->setVisible (false);
        addChild (dropdown_.get());

        // Shared direct-text-entry overlay (topmost) — RsKnob / RsLinkSlider / node-
        // panel mini value read-outs open it via their requestValueEntry hook.
        valueEntry_ = std::make_unique<fuv::ValueEntry> (base);
        valueEntry_->setVisible (false);
        addChild (valueEntry_.get());

        // --- undo baseline ---------------------------------------------------
        lastSnap_ = snapshotNow();
        undo_.push (lastSnap_, now());
        lastGestureEnd_ = store_.gestureEndCount(); // seed the observer at the current count
        refreshUndoButtons();

        // --- host-change sweep baseline (fix F1) -----------------------------
        // Precompute the node-parameter mask (lc_/hc_/b0_..b7_ prefixes) so the
        // per-tick sweep never does string compares, then run one empty sweep to
        // baseline sweeper_ at the CURRENT epochs. Any parameter change made before
        // the editor existed is already reflected in the widgets' first draw (they
        // read store.value()), so replaying it as a redraw storm on the first tick is
        // pure waste — the baseline sweep swallows it.
        nodeParamMask_.assign ((std::size_t) store_.size(), 0);
        for (int i = 0; i < store_.size(); ++i)
        {
            const std::string& id = store_.desc (i).id;
            const bool isNode = id.rfind ("lc_", 0) == 0 || id.rfind ("hc_", 0) == 0
                                || (id.size() >= 3 && id[0] == 'b' && id[1] >= '0' && id[1] <= '9' && id[2] == '_');
            nodeParamMask_[(std::size_t) i] = isNode ? 1 : 0;
        }
        sweeper_.sweep (store_, [] (int) {});
    }

    // ---- clock + snapshots --------------------------------------------------

    double RsEditor::now() const
    {
        if (clockOverride_ >= 0.0) return clockOverride_;
        using namespace std::chrono;
        return duration<double> (steady_clock::now().time_since_epoch()).count();
    }

    std::vector<float> RsEditor::snapshotNow() const
    {
        std::vector<float> v ((std::size_t) store_.size());
        for (int i = 0; i < store_.size(); ++i)
            v[(std::size_t) i] = store_.value (i);
        return v;
    }

    void RsEditor::pumpGestures()
    {
        // Coalesce a drag into one undo step on gesture-end. We OBSERVE the store's
        // gesture-end counter rather than draining its host-write queue: in the
        // shipping CLAP build the shell is the queue's single consumer (it relays
        // every enqueued edit to the host as a param/gesture output event), so the
        // editor must not steal those events. The counter is a non-consuming signal
        // any number of observers can watch.
        const std::uint32_t g = store_.gestureEndCount();
        if (g != lastGestureEnd_)
        {
            lastGestureEnd_ = g;
            commitUndo();
        }

        // Host-driven change sweep (fix F1): redraw every widget whose param changed
        // since the last tick. This catches automation / host-generic-UI / MIDI-learn
        // edits (setFromHost, which touches no widget). The UI's OWN edits bump the
        // same epochs, so the widget being dragged also gets a redraw() here — but
        // redraw() is idempotent (it just marks the frame dirty), so the double mark is
        // harmless. The curve view self-drives every frame and needs no nudge.
        bool nodeChanged = false;
        sweeper_.sweep (store_, [&] (int i)
        {
            if (visage::Frame* w = widgetForParam (i))
                w->redraw();
            if (i >= 0 && (std::size_t) i < nodeParamMask_.size() && nodeParamMask_[(std::size_t) i])
                nodeChanged = true;
        });
        if (nodeChanged && nodePanel_ && nodePanel_->isVisible())
            nodePanel_->redraw();
    }

    void RsEditor::commitUndo()
    {
        if (applyingHistory_) return;
        std::vector<float> snap = snapshotNow();
        if (snap == lastSnap_) return;
        undo_.push (snap, now());
        lastSnap_ = std::move (snap);
        refreshUndoButtons();
    }

    void RsEditor::applyHistory (const std::vector<float>& snap)
    {
        applyingHistory_ = true;
        for (int i = 0; i < store_.size() && i < (int) snap.size(); ++i)
            store_.setFromHost (i, snap[(std::size_t) i]);
        lastSnap_ = snap;
        applyingHistory_ = false;
        refreshUndoButtons();
        redrawAll();
        // Relay the bulk change to the host (rescan + mark-dirty) — the CLAP shell
        // wires this so undo/redo reaches the DAW. Unset in the harness.
        if (onHistoryApplied) onHistoryApplied();
    }

    void RsEditor::doUndo() { if (undo_.canUndo()) applyHistory (undo_.undo (snapshotNow())); }
    void RsEditor::doRedo() { if (undo_.canRedo()) applyHistory (undo_.redo (snapshotNow())); }

    void RsEditor::onStateReplaced()
    {
        // Preset load / A-B switch: a different state context wipes undo history
        // (mirrors apvts.replaceState() -> undoManager.clearUndoHistory()).
        undo_.clear();
        lastSnap_ = snapshotNow();
        undo_.push (lastSnap_, now());
        refreshUndoButtons();
        rebuildPresetMenu();
        redrawAll();
    }

    void RsEditor::refreshUndoButtons()
    {
        if (undoBtn_) undoBtn_->setDimmed (! undo_.canUndo());
        if (redoBtn_) redoBtn_->setDimmed (! undo_.canRedo());
    }

    // ---- preset / A-B -------------------------------------------------------

    void RsEditor::rebuildPresetMenu()
    {
        using Entry = fuv::PresetSelectorView::Entry;
        std::vector<Entry> menu;
        for (const RsPresetItem& it : presets_.items())
            menu.push_back (Entry::item (it.name, /*enabled*/ true, /*steppable*/ it.steppable && ! it.isAction));
        preset_->setMenu (std::move (menu), presets_.currentIndex());
    }

    void RsEditor::loadPreset (int itemIndex)
    {
        const bool loaded = presets_.load (itemIndex);
        preset_->setSelectedIndex (presets_.currentIndex());
        if (loaded)
            onStateReplaced();
        else
            redrawAll();
    }

    void RsEditor::setAbSlot (int slot)
    {
        ab_.setActiveSlot (slot);
        updateAbUi();       // flip the copy arrow to reflect the new active slot (fix 4)
        onStateReplaced();
    }

    void RsEditor::updateAbUi()
    {
        // Directional copy affordance: A active => "A->B" (arrow right); B active =>
        // "B->A" (arrow left). Letters are fixed; only the arrow flips — matching the
        // shipped JUCE RsIconButton directional Copy button (round-3 fix 4).
        if (copyBtn_) copyBtn_->setDirection (ab_.activeSlot() != 0);
    }

    void RsEditor::copyAb() { ab_.copyActiveToOther(); redrawAll(); }
    int  RsEditor::abSlot() const { return ab_.activeSlot(); }
    int  RsEditor::presetIndex() const { return presets_.currentIndex(); }

    bool RsEditor::openNamedDropdown (int which)
    {
        if (which == 0 && qualitySet_) { qualitySet_->openMenu(); return true; }
        if (which == 1 && chSet_)      { chSet_->openMenu();      return true; }
        if (which == 2 && preset_)     { preset_->openMenu();     return true; }
        return false;
    }

    // ---- node selection -----------------------------------------------------

    void RsEditor::selectNode (int id)
    {
        if (! curve_) return;
        // A pending value edit belongs to the node/control being left — discard it
        // (mirrors NodePanel::setNode -> closeValueEditor). A click that switches
        // nodes first drops focus from the entry, which COMMITS it (focus-loss),
        // so this only fires for a still-open (e.g. programmatic) edit.
        if (valueEntry_) valueEntry_->cancelEntry();
        if (id != curve_->selectedNode())
            feed_.setListenNode (-1); // switching drops Listen
        curve_->setSelectedNode (id);
        if (id >= 0)
        {
            // Oracle order (SuppressionCurveComponent::selectNode -> positionPanel):
            // bind the node FIRST so preferredWidth() reflects THIS node (cut 350 /
            // band 500), THEN centre + clamp x for that width. Reading the width
            // before setNode() used the PREVIOUSLY bound node's width — a band opened
            // right after a cut got the cut's stale 350; and because the cut panel is
            // also 350 at the same centred x, the follow-up setBounds no-ops (bounds
            // unchanged), so even RsNodePanel's width-guard was skipped — reflowing
            // the panel into the TYPE-button-under-knob overlap + "Lis…" clip the
            // user reported.
            nodePanel_->setNode (id);
            const int w = nodePanel_->preferredWidth();
            const int h = RsNodePanel::kHeight;
            float px, py, pw, ph;
            plotRectInWindowLocal (px, py, pw, ph); // frame-local plot rect
            float x = px + (pw - (float) w) * 0.5f;
            x = std::clamp (x, px, std::max (px, px + pw - (float) w));
            const float y = py + ph - (float) h - 6.0f;
            nodePanel_->setBounds (x, y, (float) w, (float) h);
            nodePanel_->setVisible (true);
        }
        else
        {
            nodePanel_->setVisible (false);
        }
        redrawAll();
    }

    // ---- dropdown -----------------------------------------------------------

    void RsEditor::presentDropdown (std::vector<fuv::Dropdown::Item> items, int selected,
                                    visage::Frame* anchor, std::function<void (int)> onSelect)
    {
        if (! dropdown_ || anchor == nullptr) return;
        const visage::Point a = anchor->positionInWindow();
        const visage::Point self = positionInWindow();
        dropdown_->onSelect = std::move (onSelect);
        dropdown_->open (std::move (items), selected, a.x - self.x, a.y - self.y, anchor->width(), anchor->height());
    }

    // ---- direct text entry --------------------------------------------------

    // A control asked to edit its value read-out. The request carries the rect in
    // WINDOW px; convert to this editor's frame-local coords and open the shared
    // ValueEntry there (it renders above every other child).
    void RsEditor::onValueEntryRequest (const fuv::ValueEntryRequest& req)
    {
        if (! valueEntry_) return;
        const visage::Point o = positionInWindow();
        valueEntry_->open (req.x - o.x, req.y - o.y, req.w, req.h, req.prefill, req.fontPx, req.commit);
    }

    bool RsEditor::valueEntryOpen() const { return valueEntry_ && valueEntry_->isOpen(); }
    std::string RsEditor::valueEntryText() const { return valueEntry_ ? valueEntry_->currentText() : std::string(); }

    // ---- theme --------------------------------------------------------------

    bool RsEditor::reloadTheme (const std::string& overlayJson, std::string& error)
    {
        RsTheme parsed;
        if (! RsTheme::load (overlayJson, parsed, error))
            return false;
        rsTheme_ = parsed;
        redrawAll();
        return true;
    }

    void RsEditor::setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (curve_) curve_->setFrozen (frozen);
        redrawAll();
    }

    // ---- layout -------------------------------------------------------------

    void RsEditor::resized()
    {
        // A window resize moves every control — discard any in-flight value edit so
        // the overlay can't linger over a stale rect.
        if (valueEntry_) valueEntry_->cancelEntry();

        const float w = width(), h = height();
        const int mx = S (20);
        const float ix = (float) mx, iw = w - 2.0f * mx;
        const float headerY = (float) mx, headerH = (float) S (44);

        const float presetRight = layoutHeaderRight (ix, iw, headerY, headerH);
        layoutHeaderBrandAndPreset (ix, headerY, headerH, presetRight);
        layoutCurveAndFooterCard (ix, iw, headerY, headerH, h, mx);

        // footer inner columns — the three-column algebra is the pure
        // computeRsFooterColumns (headless-tested); see RsFooterLayout.h for the
        // settled design + the round-#4 uniform-gap derivation.
        const float fx = footerCard_.x + S (14), fy = footerCard_.y + S (14);
        const float fw = footerCard_.w - 2.0f * S (14), fh = footerCard_.h - 2.0f * S (14);
        const RsFooterColumns cols = computeRsFooterColumns (fx, fy, fw, fh, k());
        footerDiv1_ = cols.div1;
        footerDiv2_ = cols.div2;
        layoutFooterKnobs (cols);
        layoutFooterSettings (fx, fy, fw, fh, cols.modeLeft);

        // node panel (reposition if visible) + resize grip + dropdown overlay
        if (nodePanel_ && nodePanel_->isVisible())
            selectNode (curve_->selectedNode());
        if (grip_)
        {
            const float gs = (float) S (18);
            grip_->setBounds (w - gs, h - gs, gs, gs);
            grip_->setVisible (onResizeRequest != nullptr);
        }
        if (dropdown_) dropdown_->setBounds (0.0f, 0.0f, w, h);
    }

    // header right cluster (right-to-left); returns the right bound left for the
    // preset pill.
    float RsEditor::layoutHeaderRight (float ix, float iw, float headerY, float headerH)
    {
        const float bypassBoxW = (float) S (118);
        Rect bypassBox { ix + iw - bypassBoxW, headerY, bypassBoxW, headerH };
        bypass_->setBounds (bypassBox.x, bypassBox.y + (headerH - S (24)) * 0.5f, bypassBox.w, (float) S (24));
        float rx = ix + iw - bypassBoxW - S (14);
        redoBtn_->setBounds (rx - S (30), headerY + (headerH - S (30)) * 0.5f, (float) S (30), (float) S (30)); rx -= S (30) + S (4);
        undoBtn_->setBounds (rx - S (30), headerY + (headerH - S (30)) * 0.5f, (float) S (30), (float) S (30)); rx -= S (30) + S (14);
        copyBtn_->setBounds (rx - S (44), headerY + (headerH - S (30)) * 0.5f, (float) S (44), (float) S (30)); rx -= S (44) + S (10);
        abStrip_ = { rx - (float) S (74), headerY + (headerH - S (26)) * 0.5f, (float) S (74), (float) S (26) }; rx -= S (74) + S (14);
        return rx;
    }

    void RsEditor::layoutHeaderBrandAndPreset (float ix, float headerY, float headerH, float presetRight)
    {
        brandRect_ = { ix, headerY, (float) S (300), headerH };

        // preset pill fills the centre gap between brand and the right cluster.
        const float presetLeft = ix + S (300) + S (14);
        const float presetGap = std::max (0.0f, presetRight - presetLeft);
        const float pw = std::min (presetGap, (float) S (320));
        preset_->setBounds (presetLeft + (presetGap - pw) * 0.5f, headerY + (headerH - S (30)) * 0.5f, pw, (float) S (30));
    }

    void RsEditor::layoutCurveAndFooterCard (float ix, float iw, float headerY, float headerH, float h, int mx)
    {
        // footer
        const float footerH = (float) S (226);
        const float footerY = (h - mx) - footerH;
        footerCard_ = { ix, footerY, iw, footerH };

        // curve (middle)
        const float curveY = headerY + headerH + S (16);
        Rect curveRect { ix, curveY, iw, std::max (1.0f, footerY - curveY) };
        curve_->setBounds (curveRect.x, curveRect.y, curveRect.w, curveRect.h);
    }

    void RsEditor::layoutFooterKnobs (const RsFooterColumns& c)
    {
        // col1: 2 big knobs — pair centred in col1 (gapP from the left card edge + gapP to footerDiv1).
        for (int i = 0; i < 2; ++i)
            knobs_[(std::size_t) i]->setBounds (c.pairLeft + (float) i * (c.bigDia + c.bigGap), c.cyBig, c.bigDia, c.bigCellH);
        // col2: 3 mini knobs — trio centred in col2 (gapP from footerDiv1 + gapP to footerDiv2).
        for (int i = 0; i < 3; ++i)
            knobs_[(std::size_t) (2 + i)]->setBounds (c.trioLeft + (float) i * (c.miniDia + c.miniGap), c.cyMini, c.miniDia, c.miniCellH);
    }

    // col3: MODE + 5 setting rows (anchored at modeLeft == the old col3 cx).
    void RsEditor::layoutFooterSettings (float fx, float fy, float fw, float fh, float modeLeft)
    {
        const float cx = modeLeft, cy = fy + S (6);
        const float cw = (fx + fw) - cx - S (10), ch = fh - 2.0f * S (6);
        const float rowGap = (float) S (6), cellGap = (float) std::max (4, S (6));

        modeCell_ = { cx, cy, cw, (float) S (32) };
        {
            const float segW = std::min (modeCell_.w - S (8) - S (52) - S (8), (float) S (140));
            modeSeg_->setBounds (modeCell_.x + modeCell_.w - S (8) - segW, modeCell_.y + (modeCell_.h - S (24)) * 0.5f, segW, (float) S (24));
        }
        float ry = cy + S (32) + rowGap;
        const float rowsTotal = ch - (S (32) + rowGap);
        const float rowH = std::max ((float) S (20), (rowsTotal - 4.0f * rowGap) / 5.0f);
        auto splitRow = [&] (float y, visage::Frame* left, visage::Frame* right)
        {
            const float half = (cw - cellGap) / 2.0f;
            left->setBounds (cx, y, half, rowH);
            right->setBounds (cx + half + cellGap, y, half, rowH);
        };
        splitRow (ry, pills_[0].get(), pills_[1].get()); ry += rowH + rowGap; // DELTA | S-CHAIN
        splitRow (ry, qualitySet_.get(), chSet_.get());  ry += rowH + rowGap; // QUALITY | CH
        splitRow (ry, pills_[2].get(), pills_[3].get());  ry += rowH + rowGap; // SC LISTEN | LINK
        linkAmt_->setBounds (cx, ry, cw, rowH);           ry += rowH + rowGap; // STEREO LINK
        splitRow (ry, mix_.get(), out_.get());                                 // MIX | OUT
    }

    bool RsEditor::plotRectInWindow (float& x, float& y, float& w, float& h) const
    {
        return curve_ && curve_->plotRectInWindow (x, y, w, h);
    }

    // frame-local plot rect (for positioning the node panel).
    void RsEditor::plotRectInWindowLocal (float& x, float& y, float& w, float& h) const
    {
        curve_->plotRectInWindow (x, y, w, h); // window px
        const visage::Point o = positionInWindow();
        x -= o.x; y -= o.y;                     // -> editor-frame-local
    }

    // ---- draw ---------------------------------------------------------------

    void RsEditor::draw (visage::Canvas& canvas)
    {
        // Dirty-region (A3): the editor chrome is STATIC — it repaints only on a
        // real change (resize / theme / node select / undo / A-B / preset, each of
        // which calls redrawAll()), NOT every frame. The live analyser animation
        // AND the per-frame gesture pump now ride the CURVE's own redraw loop
        // (curve().onTick, wired by the harness/shell), so per tick only the
        // analyser region repaints — the whole-editor repaint (a full-window
        // gradient fill + all chrome, every frame) that used to gate the frame
        // rate is gone.
        const float w = width(), h = height();
        canvas.setColor (visage::Brush::vertical (visage::Color (rsTheme_.base.palette.background),
                                                  visage::Color (rsTheme_.base.palette.backgroundLo)));
        canvas.fill (0.0f, 0.0f, w, h);

        drawFooterChrome (canvas);
        drawHeaderChrome (canvas);
        drawBrand (canvas);

        // TEMP geometry diagnostic (ALWAYS ON in this debug build): draws the editor's
        // own view of its logical/native size + a bottom-right child's native rect, so
        // ONE Logic screenshot pins whether the shell feeds pixels or points. Remove
        // once the resize geometry is settled. GUI-thread only.
        {
            // Bright border on the editor's own logical bounds + a filled banner so the
            // text is legible over any background.
            canvas.setColor (visage::Color (0xff00ff00));
            canvas.rectangleBorder (0.0f, 0.0f, w, h, 2.0f);
            // Only ~one line of vertical room before the analyser child covers the
            // parent draw, so put the DECISIVE shell size-negotiation trace on the top
            // visible line and the editor-native summary just under the brand.
            char line[256];
            const int oR = out_ ? (out_->nativeX() + out_->nativeWidth()) : -1;
            std::snprintf (line, sizeof line,
                           "%s | ed.nat %dx%d dpi%.3f OUTr%d",
                           debugShell_.empty() ? "(no shell trace)" : debugShell_.c_str(),
                           nativeWidth(), nativeHeight(), dpiScale(), oR);
            canvas.setColor (visage::Color (0xcc000000));
            canvas.fill (6.0f, 56.0f, w - 12.0f, 20.0f);
            canvas.setColor (visage::Color (0xff00ff88));
            canvas.text (line, fuv::boldFont (11.0f), visage::Font::kLeft, 12.0f, 58.0f, w, 16.0f);
        }
    }

    void RsEditor::drawBrand (visage::Canvas& canvas)
    {
        // Everything scales by k(): brandRect_ is S()-scaled in the layout, so a
        // fixed badge/font would overflow it and clip the wordmark's tail whenever
        // the window is below the 1069x747 design size (the Cubase-on-a-laptop
        // report — the host opens the window smaller than the design default).
        const Rect& r = brandRect_;
        const float sq = 30.0f * k();
        const float lx = r.x, ly = r.y + (r.h - sq) * 0.5f;
        canvas.setColor (visage::Color (rsTheme_.rs.glowPink));
        canvas.roundedRectangleShadow (lx, ly + 4.0f * k(), sq, sq, rsTheme_.rs.radiusBadge, 9.0f);
        canvas.setColor (visage::Brush::vertical (visage::Color (rsTheme_.rs.orange), visage::Color (rsTheme_.rs.pink)));
        canvas.roundedRectangle (lx, ly, sq, sq, rsTheme_.rs.radiusBadge);

        const float tx = r.x + sq + 12.0f * k(), tw = r.w - sq - 12.0f * k();
        // Two-colour wordmark without measuring: draw the whole "Resonance
        // TatSuppressor" in the text colour, then overdraw the "Resonance" prefix in
        // accent at the same origin so its glyphs align exactly.
        canvas.setColor (visage::Color (rsTheme_.base.palette.text));
        canvas.text ("Resonance TatSuppressor", boldFont (19.0f * k()), visage::Font::kLeft, tx, r.y, tw, r.h);
        canvas.setColor (visage::Color (rsTheme_.base.palette.accent));
        canvas.text ("Resonance", boldFont (19.0f * k()), visage::Font::kLeft, tx, r.y, tw, r.h);
    }

    void RsEditor::drawHeaderChrome (visage::Canvas& canvas)
    {
        // A|B strip (2 segments, manual — reflects the A/B model).
        const Rect& r = abStrip_;
        canvas.setColor (visage::Color (rsTheme_.rs.segTrackBg));
        canvas.roundedRectangle (r.x, r.y, r.w, r.h, rsTheme_.rs.radiusBadge);
        const float segW = r.w / 2.0f;
        const int active = ab_.activeSlot();
        const char* labels[] = { "A", "B" };
        for (int i = 0; i < 2; ++i)
        {
            const float sx = r.x + i * segW;
            if (i == active)
            {
                canvas.setColor (visage::Color (rsTheme_.base.palette.accent));
                canvas.roundedRectangle (sx + 3.0f, r.y + 3.0f, segW - 6.0f, r.h - 6.0f, rsTheme_.rs.radiusBadge - 2.0f);
            }
            canvas.setColor (visage::Color (i == active ? 0xffffffff : rsTheme_.base.palette.textDim));
            canvas.text (labels[i], boldFont (12.0f), visage::Font::kCenter, sx, r.y, segW, r.h);
        }
    }

    void RsEditor::drawFooterChrome (visage::Canvas& canvas)
    {
        // footer card + hairline
        const Rect& f = footerCard_;
        canvas.setColor (visage::Color (rsTheme_.rs.nodeShadow));
        canvas.roundedRectangleShadow (f.x, f.y + 5.0f, f.w, f.h, rsTheme_.rs.radiusCard, 16.0f);
        fuv::paintCardShell (canvas, f.x, f.y, f.w, f.h, rsTheme_.rs.radiusCard,
                             visage::Color (rsTheme_.rs.footerBg),
                             visage::Color (rsTheme_.base.palette.track));

        // column dividers
        const float dy = f.y + S (14), dh = f.h - 2.0f * S (14);
        canvas.setColor (visage::Color (rsTheme_.base.palette.track));
        canvas.segment (footerDiv1_, dy, footerDiv1_, dy + dh, 1.0f, false);
        canvas.segment (footerDiv2_, dy, footerDiv2_, dy + dh, 1.0f, false);

        // MODE cell
        const Rect& m = modeCell_;
        canvas.setColor (visage::Brush::vertical (visage::Color (rsTheme_.rs.modeBoxTop), visage::Color (0xffffffff)));
        canvas.roundedRectangle (m.x, m.y, m.w, m.h, rsTheme_.rs.radiusBox);
        canvas.setColor (visage::Color (rsTheme_.rs.modeBoxBorder));
        canvas.roundedRectangleBorder (m.x + 0.75f, m.y + 0.75f, m.w - 1.5f, m.h - 1.5f, rsTheme_.rs.radiusBox, 1.5f);
        canvas.setColor (visage::Color (rsTheme_.base.palette.text));
        canvas.text ("MODE", boldFont (12.0f), visage::Font::kLeft, m.x + S (12), m.y, m.w - S (12), m.h);
    }

    void RsEditor::mouseDown (const visage::MouseEvent& e)
    {
        if (abStrip_.contains (e.position))
        {
            const int slot = (e.position.x - abStrip_.x) < abStrip_.w * 0.5f ? 0 : 1;
            if (slot != ab_.activeSlot())
                setAbSlot (slot);
            return;
        }
    }

    // ---- driver lookups -----------------------------------------------------

    bool RsEditor::nodeCentreInWindow (int id, float& x, float& y) const
    {
        return curve_ && curve_->nodeCentreInWindow (id, x, y);
    }

    bool RsEditor::miniKnobTipInWindow (int which, float& cx, float& cy, float& tx, float& ty) const
    {
        return nodePanel_ && nodePanel_->isVisible()
               && nodePanel_->miniKnobTipInWindow (which, cx, cy, tx, ty);
    }

    // The single param-index -> footer widget map, shared by widgetRectInWindow and
    // pumpGestures's host-change sweep (fix F1), so the two can never diverge.
    visage::Frame* RsEditor::widgetForParam (int pi) const
    {
        if (pi < 0) return nullptr;
        for (const auto& kn : knobs_) if (kn->paramIndex() == pi) return kn.get();
        for (const auto& pl : pills_) if (pl->paramIndex() == pi) return pl.get();
        if (bypass_     && bypass_->paramIndex()     == pi) return bypass_.get();
        if (modeSeg_    && modeSeg_->paramIndex()    == pi) return modeSeg_.get();
        if (qualitySet_ && qualitySet_->paramIndex() == pi) return qualitySet_.get();
        if (chSet_      && chSet_->paramIndex()      == pi) return chSet_.get();
        if (linkAmt_    && linkAmt_->paramIndex()    == pi) return linkAmt_.get();
        if (mix_        && mix_->paramIndex()        == pi) return mix_.get();
        if (out_        && out_->paramIndex()        == pi) return out_.get();
        return nullptr;
    }

    bool RsEditor::widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const
    {
        auto rectOf = [&] (const visage::Frame* fr) -> bool
        {
            if (fr == nullptr) return false;
            const visage::Point p = fr->positionInWindow();
            x = p.x; y = p.y; w = fr->width(); h = fr->height();
            return true;
        };
        const visage::Point o = positionInWindow();
        if (key == "preset") return rectOf (preset_.get());
        if (key == "curve")  return rectOf (curve_.get());
        if (key == "undo")   return rectOf (undoBtn_.get());
        if (key == "redo")   return rectOf (redoBtn_.get());
        if (key == "copy")   return rectOf (copyBtn_.get());
        if (key == "nodePanel") return nodePanel_ && nodePanel_->isVisible() ? rectOf (nodePanel_.get()) : false;
        if (key == "ab")     { x = o.x + abStrip_.x; y = o.y + abStrip_.y; w = abStrip_.w; h = abStrip_.h; return true; }
        // Footer chrome (window px) — the divider-gap uniformity guard (test 19).
        if (key == "footerDiv1") { x = o.x + footerDiv1_; y = o.y + footerCard_.y; w = 1.0f; h = footerCard_.h; return true; }
        if (key == "footerDiv2") { x = o.x + footerDiv2_; y = o.y + footerCard_.y; w = 1.0f; h = footerCard_.h; return true; }
        if (key == "modeCard")   { x = o.x + modeCell_.x; y = o.y + modeCell_.y; w = modeCell_.w; h = modeCell_.h; return true; }
        if (key == "footerCard") { x = o.x + footerCard_.x; y = o.y + footerCard_.y; w = footerCard_.w; h = footerCard_.h; return true; }

        const int pi = store_.indexOf (key);
        if (visage::Frame* fr = widgetForParam (pi)) return rectOf (fr);
        return false;
    }

    // ---- window resizing (grip + snap) --------------------------------------

    void RsEditor::snapWindowSize (float& w, float& h)
    {
        // Height-driven aspect snap + limit clamp — the same maths as the CLAP
        // shell's adjustSize, so a grip drag and a host-edge drag land on
        // identical sizes.
        float sh = h < kMinH ? kMinH : (h > kMaxH ? kMaxH : h);
        float sw = sh * kDesignW / kDesignH;
        if (sw < kMinW) { sw = kMinW; sh = sw * kDesignH / kDesignW; }
        if (sw > kMaxW) { sw = kMaxW; sh = sw * kDesignH / kDesignW; }
        w = std::round (sw);
        h = std::round (sh);
    }

    void RsResizeGrip::draw (visage::Canvas& canvas)
    {
        // Three diagonal hairlines in the corner (the JUCE corner-resizer glyph).
        canvas.setColor (visage::Color (lineColour_));
        const float w = width(), h = height();
        for (int i = 1; i <= 3; ++i)
        {
            const float o = (float) i * w * 0.25f;
            canvas.segment (w - o, h - 2.0f, w - 2.0f, h - o, 1.5f, true);
        }
    }

    void RsResizeGrip::mouseDown (const visage::MouseEvent& e)
    {
        downPos_ = e.windowPosition();
        if (visage::Frame* p = parent())
        {
            startW_ = p->width();
            startH_ = p->height();
        }
    }

    void RsResizeGrip::mouseDrag (const visage::MouseEvent& e)
    {
        if (! onDragResize || startW_ <= 0.0f) return;
        const visage::Point p = e.windowPosition();
        const float dw = p.x - downPos_.x, dh = p.y - downPos_.y;
        // Corner drag: let whichever axis pulls larger drive the (height-led)
        // aspect snap, so dragging mostly-right still grows the window.
        const float hFromW = (startW_ + dw) * RsEditor::kDesignH / RsEditor::kDesignW;
        onDragResize (startW_ + dw, std::max (startH_ + dh, hFromW));
    }
}
