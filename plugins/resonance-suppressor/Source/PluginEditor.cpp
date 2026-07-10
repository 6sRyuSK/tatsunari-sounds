#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

#include <cmath>

ResonanceSuppressorAudioProcessorEditor::ResonanceSuppressorAudioProcessorEditor (ResonanceSuppressorAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p), curve (p, p.apvts)
{
    setLookAndFeel (&rsLnf);

    // ------------------------------------------------------------ Header
    addAndMakeVisible (brand);
    // presetController added its selector to this editor in its ctor; the shared
    // controller (host/user sync, Save/Overwrite/Delete) is reused verbatim --
    // only restyled, by the RsLookAndFeel set above. Clicking the name opens the
    // preset menu (the added menu affordance).

    abSeg.setSegments ({ "A", "B" });
    abSeg.setAccent (rs::colour::accent());
    abSeg.onSelect = [this] (int i) { processor.setABSlot (i); updateABUI(); };
    addAndMakeVisible (abSeg);

    copyBtn.setDirection (false); // directional A->B glyph; kept in sync with the active slot in updateABUI()
    copyBtn.setColours (rs::colour::accent(), rs::colour::textFaint());
    copyBtn.onClick = [this] { processor.copyActiveToOther(); updateABUI(); };
    addAndMakeVisible (copyBtn);

    undoBtn.setGlyph (rs::icons::undo());
    undoBtn.setColours (rs::colour::text(), rs::colour::textFaint());
    undoBtn.setTooltip ("Undo the last parameter change.");
    undoBtn.onClick = [this] { processor.getUndoManager().undo(); refreshUndoRedoButtons(); };
    addAndMakeVisible (undoBtn);

    redoBtn.setGlyph (rs::icons::redo());
    redoBtn.setColours (rs::colour::text(), rs::colour::textFaint());
    redoBtn.setTooltip ("Redo.");
    redoBtn.onClick = [this] { processor.getUndoManager().redo(); refreshUndoRedoButtons(); };
    addAndMakeVisible (redoBtn);

    bypassToggle.setOnColour (rs::colour::teal());
    bypassToggle.setPillSize (42, 23);
    bypassToggle.setTooltip ("Bypass (bit-transparent; the engine keeps running through its latency).");
    addAndMakeVisible (bypassToggle);
    bypassAtt = std::make_unique<BA> (processor.apvts, "bypass", bypassToggle);

    // ---------------------------------------------------------- Analyzer
    addAndMakeVisible (curve); // placed only; SuppressionCurveComponent internals untouched (P2)

    // ------------------------------------------------------ Footer: knobs
    addKnob (depthK, "DEPTH",  rs::colour::accent(), true,  " %",  "depth",       0);
    addKnob (sharpK, "SHARP",  rs::colour::accent(), true,  " %",  "sharpness",   0);
    addKnob (selK,   "SELECT", rs::colour::accent(), true,  " %",  "selectivity", 0);
    addKnob (mixK,   "MIX",    rs::colour::accent(), true,  " %",  "mix",         0);
    addKnob (atkK,   "ATK",    rs::colour::amber(),  false, " ms", "attack",      2);
    addKnob (relK,   "REL",    rs::colour::amber(),  false, " ms", "release",     2);
    addKnob (tiltK,  "TILT",   rs::colour::mint(),   false, " %",  "tilt",        0);

    // ------------------------------------------ Footer: MODE + settings
    modeSeg.setSegments ({ "Soft", "Hard" }, { rs::icons::modeSoft(), rs::icons::modeHard() });
    modeSeg.setAccent (rs::colour::accent());
    addAndMakeVisible (modeSeg);
    modeAtt = std::make_unique<CA> (processor.apvts, "mode", modeSeg.comboBox());

    struct { rs::RsPillToggle* t; juce::Colour on; const char* tip; } pills[] = {
        { &deltaToggle,    rs::colour::accent(), "Delta: monitor only the removed signal." },
        { &scEnableToggle, rs::colour::teal(),   "Key detection off the Sidechain input bus (falls back to internal when unpatched)." },
        { &scListenToggle, rs::colour::teal(),   "Monitor the raw sidechain (delayed to the plugin latency)." },
        { &linkToggle,     rs::colour::accent(), "Link stereo detection (blend amount set by STEREO LINK)." },
    };
    for (auto& q : pills)
    {
        q.t->setOnColour (q.on);
        q.t->setPillSize (34, 19);
        q.t->setPillRightInset (9); // keep the pill inside the card (fixes overflow past the rounded edge)
        q.t->setTooltip (q.tip);
        addAndMakeVisible (*q.t);
    }
    deltaAtt    = std::make_unique<BA> (processor.apvts, "delta",    deltaToggle);
    scEnableAtt = std::make_unique<BA> (processor.apvts, "scEnable", scEnableToggle);
    scListenAtt = std::make_unique<BA> (processor.apvts, "scListen", scListenToggle);
    linkAtt     = std::make_unique<BA> (processor.apvts, "link",     linkToggle);

    qualitySet.setup (rs::icons::quality(), "QUALITY", { "Fast", "Normal", "High" });
    qualitySet.setTooltip ("Fast: half latency, half low-frequency resolution. High: double resolution, double latency.");
    addAndMakeVisible (qualitySet);
    qualityAtt = std::make_unique<CA> (processor.apvts, "quality", qualitySet.comboBox());

    chSet.setup (rs::icons::channel(), "CH", { "Stereo", "Mid-Side" });
    chSet.setTooltip ("Stereo: process L/R. Mid-Side: process the M/S encode (bypass stays bit-transparent).");
    addAndMakeVisible (chSet);
    channelAtt = std::make_unique<CA> (processor.apvts, "channelMode", chSet.comboBox());

    linkAmtSlider.setTooltip ("Stereo Link amount: per-channel <-> stereo-linked detection blend.");
    addAndMakeVisible (linkAmtSlider);
    linkAmtAtt = std::make_unique<SA> (processor.apvts, "linkAmt", linkAmtSlider);

    updateABUI();
    refreshUndoRedoButtons();
    startTimer (500); // idle-transaction boundary + Undo/Redo enable refresh (Phase 5b-2, preserved)

    // Default to the demo's authored size; fixed aspect so the whole chrome
    // scales as a unit (resized() derives a scale factor from the design height).
    setResizable (true, true);
    setResizeLimits (940, 657, 1320, 922);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) rs::layout::designWidth / (double) rs::layout::designHeight);
    setSize (rs::layout::designWidth, rs::layout::designHeight);
}

ResonanceSuppressorAudioProcessorEditor::~ResonanceSuppressorAudioProcessorEditor()
{
    stopTimer();
    processor.setListenNode (-1); // never leave a node soloed after the editor closes (preserved)
    setLookAndFeel (nullptr);
}

void ResonanceSuppressorAudioProcessorEditor::addKnob (rs::RsKnob& k, const juce::String& name, juce::Colour accent,
                                                       bool big, const juce::String& suffix, const juce::String& id,
                                                       int decimals)
{
    k.setup (name, accent, big, suffix);
    addAndMakeVisible (k);
    knobAtts.push_back (std::make_unique<SA> (processor.apvts, id, k.slider()));
    factory_ui::setSliderDecimals (k.slider(), decimals); // after the attachment (#23)
}

void ResonanceSuppressorAudioProcessorEditor::timerCallback()
{
    processor.getUndoManager().beginNewTransaction();
    refreshUndoRedoButtons();
}

void ResonanceSuppressorAudioProcessorEditor::refreshUndoRedoButtons()
{
    auto& um = processor.getUndoManager();
    undoBtn.setEnabled (um.canUndo());
    redoBtn.setEnabled (um.canRedo());
}

void ResonanceSuppressorAudioProcessorEditor::updateABUI()
{
    const bool isA = processor.getABSlot() == 0;
    abSeg.setSelectedIndex (isA ? 0 : 1, juce::dontSendNotification);
    copyBtn.setTooltip (juce::String ("Copy slot ") + (isA ? "A" : "B") + " onto slot " + (isA ? "B" : "A") + ".");
    copyBtn.setDirection (! isA); // A active => "A>B"; B active => "B>A"
}

void ResonanceSuppressorAudioProcessorEditor::layoutPillRow (juce::Rectangle<int> row,
                                                             rs::RsPillToggle& left,  rs::icons::Glyph lg, const juce::String& lcap,
                                                             rs::RsPillToggle& right, rs::icons::Glyph rg, const juce::String& rcap)
{
    const float k = getHeight() / (float) rs::layout::designHeight;
    const int gap = juce::jmax (4, (int) std::round (6.0f * k));
    auto l = row.removeFromLeft ((row.getWidth() - gap) / 2);
    left.setBounds (l);
    pillCells.push_back ({ l, std::move (lg), lcap });
    row.removeFromLeft (gap);
    right.setBounds (row);
    pillCells.push_back ({ row, std::move (rg), rcap });
}

void ResonanceSuppressorAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float k = getHeight() / (float) rs::layout::designHeight;

    // Warm-white panel gradient over the whole editor (the demo's outer 22px
    // radius / drop shadow only reads against a desktop backdrop, so a plugin
    // editor fills its full rect -- intentional deviation, see report).
    auto b = getLocalBounds();
    juce::ColourGradient bg (rs::colour::panelTop(), 0.0f, (float) b.getY(),
                             rs::colour::panelBottom(), 0.0f, (float) b.getBottom(), false);
    g.setGradientFill (bg);
    g.fillRect (b);

    // Soft warm shadows behind the analyser + footer cards.
    factory_ui::dropShadowFor (g, curve.getBounds(), rs::radius::card);
    factory_ui::dropShadowFor (g, footerCardBounds, rs::radius::card);

    // Footer card + inset hairline.
    auto fc = footerCardBounds.toFloat();
    g.setColour (rs::colour::footerBg());
    g.fillRoundedRectangle (fc, rs::radius::card);
    g.setColour (rs::colour::border());
    g.drawRoundedRectangle (fc.reduced (0.5f), rs::radius::card, 1.0f);

    // Column dividers.
    g.setColour (rs::colour::border());
    const int dy = footerCardBounds.getY() + (int) std::round (14.0f * k);
    const int dh = footerCardBounds.getHeight() - (int) std::round (28.0f * k);
    g.fillRect (juce::Rectangle<int> (footerDivX1, dy, 1, dh));
    g.fillRect (juce::Rectangle<int> (footerDivX2, dy, 1, dh));

    // MODE cell: distinct gradient card + coral border + "MODE" caption.
    {
        auto m = modeCellBounds.toFloat();
        juce::ColourGradient mg (rs::colour::modeBoxTop(), m.getTopLeft(),
                                 rs::colour::white(), m.getBottomLeft(), false);
        g.setGradientFill (mg);
        g.fillRoundedRectangle (m, rs::radius::box);
        g.setColour (rs::colour::modeBoxBorder());
        g.drawRoundedRectangle (m.reduced (0.75f), rs::radius::box, 1.5f);
        g.setColour (rs::colour::text());
        g.setFont (rs::font (rs::FontKind::Ui, 12.0f, 800, 0.04f));
        g.drawText ("MODE", modeCellBounds.reduced ((int) std::round (12.0f * k), 0), juce::Justification::centredLeft);
    }

    // Pill-toggle cells: white card + icon + caption (the pill itself is drawn
    // by the RsPillToggle child that fills the cell, so a whole-cell click
    // toggles). Reserve the pill's right-hand strip so a long caption never
    // slides under it.
    const int pillReserve = (int) std::round (46.0f * k);
    for (auto& c : pillCells)
    {
        auto cb = c.bounds.toFloat();
        g.setColour (rs::colour::white());
        g.fillRoundedRectangle (cb, rs::radius::badge);
        g.setColour (rs::colour::border());
        g.drawRoundedRectangle (cb.reduced (0.5f), rs::radius::badge, 1.0f);

        auto inner = cb.reduced (9.0f, 0.0f);
        inner.removeFromRight ((float) pillReserve);
        auto gi = inner.removeFromLeft (inner.getHeight());
        rs::icons::paintGlyph (g, c.glyph, gi.reduced (inner.getHeight() * 0.30f), rs::colour::textSecondary());
        inner.removeFromLeft (6.0f);
        g.setColour (rs::colour::textSecondary());
        g.setFont (rs::font (rs::FontKind::Ui, 11.0f, 800, 0.03f));
        g.drawText (c.caption, inner, juce::Justification::centredLeft);
    }

    // Header Bypass caption (the pill is a child; the label sits to its left).
    g.setColour (rs::colour::textSecondary());
    g.setFont (rs::font (rs::FontKind::Ui, 12.0f, 700));
    g.drawText ("Bypass", bypassLabelBounds, juce::Justification::centredRight);
}

void ResonanceSuppressorAudioProcessorEditor::resized()
{
    const float kf = getHeight() / (float) rs::layout::designHeight;
    auto S = [kf] (float v) { return (int) std::round (v * kf); };

    pillCells.clear();

    auto r = getLocalBounds().reduced (S (20));

    // ---- Header: brand (left) / preset pill (centre) / A|B + Copy + Undo/Redo
    //      + Bypass (right). ----
    auto header = r.removeFromTop (S (44));
    r.removeFromTop (S (16));

    auto centreV = [] (juce::Rectangle<int> box, int w, int h)
    { return box.withSizeKeepingCentre (w, h); };

    brand.setBounds (header.removeFromLeft (S (300)));

    // Right cluster, laid out right-to-left.
    auto bypassBox = header.removeFromRight (S (118));
    bypassToggle.setBounds (centreV (bypassBox.removeFromRight (S (48)), S (48), S (24)));
    bypassLabelBounds = bypassBox;
    header.removeFromRight (S (14));
    redoBtn.setBounds (centreV (header.removeFromRight (S (30)), S (30), S (30)));
    header.removeFromRight (S (4));
    undoBtn.setBounds (centreV (header.removeFromRight (S (30)), S (30), S (30)));
    header.removeFromRight (S (14));
    copyBtn.setBounds (centreV (header.removeFromRight (S (44)), S (44), S (30)));
    header.removeFromRight (S (10));
    abSeg.setBounds (centreV (header.removeFromRight (S (74)), S (74), S (26)));
    header.removeFromRight (S (14));

    // Preset pill fills the centre gap (capped width, centred).
    {
        const int pw = juce::jmin (header.getWidth(), S (320));
        presetController.selector().setBounds (centreV (header, pw, S (30)));
    }

    // ---- Footer card: three columns (big knobs / small knobs / settings). ----
    // Tall enough that col3's five stacked rows (MODE / DELTA|S-CHAIN /
    // QUALITY|CH / SC LISTEN|LINK / STEREO LINK) all fit with margin; the
    // analyser gives up the extra height (acceptable per review).
    auto footer = r.removeFromBottom (S (198));
    footerCardBounds = footer;

    auto inner = footer.reduced (S (14));
    const int w = inner.getWidth();
    auto col1 = inner.removeFromLeft ((int) (w * 0.40f));
    footerDivX1 = inner.getX();
    auto col2 = inner.removeFromLeft ((int) (w * 0.20f));
    footerDivX2 = inner.getX();
    auto col3 = inner;

    // Analyser fills whatever remains in the middle (after header + footer).
    curve.setBounds (r);

    // Column 1: four big knobs.
    {
        auto c = col1.reduced (S (8), S (6));
        rs::RsKnob* big[] = { &depthK, &sharpK, &selK, &mixK };
        const int n = (int) std::size (big);
        const int cw = c.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            auto cell = (i == n - 1) ? c : c.removeFromLeft (cw);
            big[i]->setBounds (cell.reduced (S (4), 0));
        }
    }

    // Column 2: three small knobs.
    {
        auto c = col2.reduced (S (6), S (6));
        rs::RsKnob* small[] = { &atkK, &relK, &tiltK };
        const int n = (int) std::size (small);
        const int cw = c.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            auto cell = (i == n - 1) ? c : c.removeFromLeft (cw);
            small[i]->setBounds (cell.reduced (S (3), 0));
        }
    }

    // Column 3: MODE (full) / DELTA|S-CHAIN / QUALITY|CH / SC LISTEN|LINK /
    //           STEREO LINK (full).
    {
        auto c = col3.reduced (S (10), S (6));
        const int rowGap = S (6);
        const int cellGap = juce::jmax (4, S (6));

        modeCellBounds = c.removeFromTop (S (32));
        {
            auto m = modeCellBounds.reduced (S (8), S (5));
            m.removeFromLeft (S (52)); // "MODE" caption sits to the left
            const int segW = juce::jmin (m.getWidth(), S (140));
            modeSeg.setBounds (m.removeFromRight (segW).withSizeKeepingCentre (segW, S (24)));
        }
        c.removeFromTop (rowGap);

        // The four remaining rows (DELTA|S-CHAIN, QUALITY|CH, SC LISTEN|LINK, and
        // the full-width STEREO LINK slider) SHARE whatever height is left, so
        // the bottom slider can never be clipped regardless of window size or
        // rounding (4*rowH + 3*rowGap <= remaining by construction).
        const int rowH = juce::jmax (S (20), (c.getHeight() - 3 * rowGap) / 4);

        layoutPillRow (c.removeFromTop (rowH),
                       deltaToggle,    rs::icons::delta(),     "DELTA",
                       scEnableToggle, rs::icons::sidechain(), "S-CHAIN");
        c.removeFromTop (rowGap);

        {
            auto row = c.removeFromTop (rowH);
            auto l = row.removeFromLeft ((row.getWidth() - cellGap) / 2);
            qualitySet.setBounds (l);
            row.removeFromLeft (cellGap);
            chSet.setBounds (row);
        }
        c.removeFromTop (rowGap);

        layoutPillRow (c.removeFromTop (rowH),
                       scListenToggle, rs::icons::listen(), "SC LISTEN",
                       linkToggle,     rs::icons::link(),   "LINK");
        c.removeFromTop (rowGap);

        linkAmtSlider.setBounds (c.removeFromTop (rowH));
    }
}
