#include "RsEditor.h"

#include "RsIcons.h"
#include "factory_ui_visage/Icons.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace rs_ui
{
    namespace fuv = factory_ui_visage;
    using fuv::regularFont;
    using fuv::boldFont;

    RsEditor::RsEditor (const RsTheme& theme, factory_params::ParamStore& store, RsFeed& feed,
                        RsPresetModel& presets, RsAbModel& ab)
        : rsTheme_ (theme), store_ (store), feed_ (feed), presets_ (presets), ab_ (ab), model_ (store)
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
        nodePanel_->setVisible (false);
        addChild (nodePanel_.get());

        // --- shared dropdown overlay (on top) --------------------------------
        dropdown_ = std::make_unique<fuv::Dropdown> (base);
        dropdown_->setVisible (false);
        addChild (dropdown_.get());

        // --- undo baseline ---------------------------------------------------
        lastSnap_ = snapshotNow();
        undo_.push (lastSnap_, now());
        refreshUndoButtons();
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
        bool sawEnd = false;
        store_.drainHostWrites ([&] (const factory_params::HostWrite& w)
        {
            if (w.kind == factory_params::HostWrite::Kind::GestureEnd)
                sawEnd = true;
        });
        if (sawEnd)
            commitUndo();
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
        const float w = width(), h = height();
        const int mx = S (20);
        const float ix = (float) mx, iw = w - 2.0f * mx;

        // header
        const float headerY = (float) mx, headerH = (float) S (44);
        float hx = ix, hw = iw;

        // right cluster (right-to-left)
        const float bypassBoxW = (float) S (118);
        Rect bypassBox { ix + iw - bypassBoxW, headerY, bypassBoxW, headerH };
        bypass_->setBounds (bypassBox.x, bypassBox.y + (headerH - S (24)) * 0.5f, bypassBox.w, (float) S (24));
        float rx = ix + iw - bypassBoxW - S (14);
        redoBtn_->setBounds (rx - S (30), headerY + (headerH - S (30)) * 0.5f, (float) S (30), (float) S (30)); rx -= S (30) + S (4);
        undoBtn_->setBounds (rx - S (30), headerY + (headerH - S (30)) * 0.5f, (float) S (30), (float) S (30)); rx -= S (30) + S (14);
        copyBtn_->setBounds (rx - S (44), headerY + (headerH - S (30)) * 0.5f, (float) S (44), (float) S (30)); rx -= S (44) + S (10);
        abStrip_ = { rx - (float) S (74), headerY + (headerH - S (26)) * 0.5f, (float) S (74), (float) S (26) }; rx -= S (74) + S (14);

        brandRect_ = { ix, headerY, (float) S (300), headerH };

        // preset pill fills the centre gap between brand and the right cluster.
        const float presetLeft = ix + S (300) + S (14);
        const float presetRight = rx;
        const float presetGap = std::max (0.0f, presetRight - presetLeft);
        const float pw = std::min (presetGap, (float) S (320));
        preset_->setBounds (presetLeft + (presetGap - pw) * 0.5f, headerY + (headerH - S (30)) * 0.5f, pw, (float) S (30));

        // footer
        const float footerH = (float) S (226);
        const float footerY = (h - mx) - footerH;
        footerCard_ = { ix, footerY, iw, footerH };

        // curve (middle)
        const float curveY = headerY + headerH + S (16);
        Rect curveRect { ix, curveY, iw, std::max (1.0f, footerY - curveY) };
        curve_->setBounds (curveRect.x, curveRect.y, curveRect.w, curveRect.h);

        // footer inner columns
        const float fx = footerCard_.x + S (14), fy = footerCard_.y + S (14);
        const float fw = footerCard_.w - 2.0f * S (14), fh = footerCard_.h - 2.0f * S (14);
        // Footer knob layout (settled design). Size-contrast ratio 1.8: big DEPTH/
        // DETAIL dial 104 px, mini ATK/REL/TILT dial 57 px. 8 px vertical label gaps
        // on BOTH groups — big cell = name16 + 8 + dial104 + 8 + value17 = 153, mini
        // cell = name14 + 8 + dial57 + 8 + value14 = 101 (the gap falls out of the
        // dial being width-limited inside the taller cell, so each dial is centred
        // with an 8 px band above/below). Horizontal edge-to-edge dial gaps: DEPTH↔
        // DETAIL 40, ATK↔REL↔TILT 20. Each cell is exactly the dial width so the
        // cell-to-cell gap IS the edge-to-edge dial gap. col1/col2 are redistributed
        // so the wider mini trio fits (col2 = trio + 12 pad, col1 = remainder);
        // footerDiv2 / col3 (MODE) are untouched. All lengths scale with S().
        footerDiv2_ = fx + fw * 0.60f;                          // unchanged col2/col3 boundary
        const float bigDia = (float) S (104), miniDia = (float) S (57);
        const float bigCellH = (float) S (153), miniCellH = (float) S (101);
        const float bigGap = (float) S (40), miniGap = (float) S (20); // edge-to-edge dial gaps
        const float bigPairW = 2.0f * bigDia + bigGap;
        const float miniTrioW = 3.0f * miniDia + 2.0f * miniGap;
        const float col2W = miniTrioW + (float) S (12);
        const float col1W = (footerDiv2_ - fx) - col2W;
        footerDiv1_ = fx + col1W;
        const float chFull = fh - 2.0f * S (6);
        const float cyBig = fy + S (6) + (chFull - bigCellH) * 0.5f;   // stacks vertically centred
        const float cyMini = fy + S (6) + (chFull - miniCellH) * 0.5f;

        // col1: 2 big knobs — pair centred in col1, bigGap between.
        {
            const float pairLeft = fx + (col1W - bigPairW) * 0.5f;
            for (int i = 0; i < 2; ++i)
                knobs_[(std::size_t) i]->setBounds (pairLeft + (float) i * (bigDia + bigGap), cyBig, bigDia, bigCellH);
        }
        // col2: 3 mini knobs — trio centred in col2, miniGap between.
        {
            const float trioLeft = footerDiv1_ + (col2W - miniTrioW) * 0.5f;
            for (int i = 0; i < 3; ++i)
                knobs_[(std::size_t) (2 + i)]->setBounds (trioLeft + (float) i * (miniDia + miniGap), cyMini, miniDia, miniCellH);
        }
        // col3: MODE + 5 setting rows
        {
            const float cx = footerDiv2_ + S (10), cy = fy + S (6);
            const float cw = (fx + fw) - cx - S (10), ch = fh - 2.0f * S (6);
            const float rowGap = (float) S (6), cellGap = (float) std::max (4, S (6));

            modeCell_ = { cx, cy, cw, (float) S (32) };
            {
                const float mx2 = modeCell_.x + S (8) + S (52);
                const float segW = std::min (modeCell_.w - S (8) - S (52) - S (8), (float) S (140));
                modeSeg_->setBounds (modeCell_.x + modeCell_.w - S (8) - segW, modeCell_.y + (modeCell_.h - S (24)) * 0.5f, segW, (float) S (24));
                (void) mx2;
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

        // node panel (reposition if visible) + dropdown overlay
        if (nodePanel_ && nodePanel_->isVisible())
            selectNode (curve_->selectedNode());
        if (dropdown_) dropdown_->setBounds (0.0f, 0.0f, w, h);
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
    }

    void RsEditor::drawBrand (visage::Canvas& canvas)
    {
        const Rect& r = brandRect_;
        const float sq = 30.0f;
        const float lx = r.x, ly = r.y + (r.h - sq) * 0.5f;
        canvas.setColor (visage::Color (rsTheme_.rs.glowPink));
        canvas.roundedRectangleShadow (lx, ly + 4.0f, sq, sq, rsTheme_.rs.radiusBadge, 9.0f);
        canvas.setColor (visage::Brush::vertical (visage::Color (rsTheme_.rs.orange), visage::Color (rsTheme_.rs.pink)));
        canvas.roundedRectangle (lx, ly, sq, sq, rsTheme_.rs.radiusBadge);

        const float tx = r.x + sq + 12.0f, tw = r.w - sq - 12.0f;
        // Two-colour wordmark without measuring: draw the whole "Resonance
        // TatSuppressor" in the text colour, then overdraw the "Resonance" prefix in
        // accent at the same origin so its glyphs align exactly.
        canvas.setColor (visage::Color (rsTheme_.base.palette.text));
        canvas.text ("Resonance TatSuppressor", boldFont (19.0f), visage::Font::kLeft, tx, r.y, tw, r.h);
        canvas.setColor (visage::Color (rsTheme_.base.palette.accent));
        canvas.text ("Resonance", boldFont (19.0f), visage::Font::kLeft, tx, r.y, tw, r.h);
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
        canvas.setColor (visage::Color (rsTheme_.rs.footerBg));
        canvas.roundedRectangle (f.x, f.y, f.w, f.h, rsTheme_.rs.radiusCard);
        canvas.setColor (visage::Color (rsTheme_.base.palette.track));
        canvas.roundedRectangleBorder (f.x + 0.5f, f.y + 0.5f, f.w - 1.0f, f.h - 1.0f, rsTheme_.rs.radiusCard, 1.0f);

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

        const int pi = store_.indexOf (key);
        if (pi < 0) return false;
        for (const auto& kn : knobs_)   if (kn->paramIndex() == pi) return rectOf (kn.get());
        for (const auto& pl : pills_)   if (pl->paramIndex() == pi) return rectOf (pl.get());
        if (bypass_ && bypass_->paramIndex() == pi) return rectOf (bypass_.get());
        if (modeSeg_ && modeSeg_->paramIndex() == pi) return rectOf (modeSeg_.get());
        if (qualitySet_ && qualitySet_->paramIndex() == pi) return rectOf (qualitySet_.get());
        if (chSet_ && chSet_->paramIndex() == pi) return rectOf (chSet_.get());
        if (linkAmt_ && linkAmt_->paramIndex() == pi) return rectOf (linkAmt_.get());
        if (mix_ && mix_->paramIndex() == pi) return rectOf (mix_.get());
        if (out_ && out_->paramIndex() == pi) return rectOf (out_.get());
        return false;
    }
}
