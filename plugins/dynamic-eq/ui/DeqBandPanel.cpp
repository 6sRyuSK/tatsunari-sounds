//
// DeqBandPanel.cpp — the Visage port of the JUCE BandControlPanel (see DeqBandPanel.h).
//
#include "DeqBandPanel.h"
#include "DeqIcons.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Icons.h"

#include "factory_core/Filters.h"

#include <cmath>
#include <string>

namespace deq_ui
{
    namespace
    {
        constexpr int kNumBands = 24;
        std::string bp (int band, const char* s) { return "b" + std::to_string (band) + "_" + s; }
    }

    DeqBandPanel::Ix DeqBandPanel::indicesFor (int band) const
    {
        Ix ix {};
        ix.byp   = store_.indexOf (bp (band, "byp"));
        ix.lsn   = store_.indexOf (bp (band, "lsn"));
        ix.dyn   = store_.indexOf (bp (band, "dyn"));
        ix.type  = store_.indexOf (bp (band, "type"));
        ix.slope = store_.indexOf (bp (band, "slope"));
        ix.chan  = store_.indexOf (bp (band, "chan"));
        ix.freq  = store_.indexOf (bp (band, "freq"));
        ix.gain  = store_.indexOf (bp (band, "gain"));
        ix.q     = store_.indexOf (bp (band, "q"));
        ix.thr   = store_.indexOf (bp (band, "thr"));
        ix.rng   = store_.indexOf (bp (band, "rng"));
        ix.atk   = store_.indexOf (bp (band, "atk"));
        ix.rel   = store_.indexOf (bp (band, "rel"));
        ix.knee  = store_.indexOf (bp (band, "knee"));
        return ix;
    }

    DeqBandPanel::DeqBandPanel (const factory_ui_visage::Theme& theme, factory_params::ParamStore& store)
        : theme_ (theme), store_ (store)
    {
        using namespace factory_ui_visage;
        ix_ = indicesFor (0);

        bypass_ = std::make_unique<PillToggle> (store_, ix_.byp, theme_);
        listen_ = std::make_unique<PillToggle> (store_, ix_.lsn, theme_);
        dyn_    = std::make_unique<PillToggle> (store_, ix_.dyn, theme_);
        // Short captions (the param names are "Band N Bypass" etc.).
        bypass_->setCaption ("Bypass");
        listen_->setCaption ("Listen");
        dyn_->setCaption ("Dynamics");

        // Listen is exclusive across bands (mirrors the JUCE BandControlPanel): the DSP
        // solos only the lowest-numbered listening band, so turning one on must clear
        // the other 23 — otherwise a band can light up "Listen" yet stay inaudible.
        listen_->onToggle = [this] (bool on)
        {
            if (! on) return;
            for (int b = 0; b < kNumBands; ++b)
                if (b != band_)
                {
                    const int idx = store_.indexOf (bp (b, "lsn"));
                    if (idx >= 0 && store_.value (idx) > 0.5f)
                        store_.setFromUiGestured (idx, 0.0f);
                }
        };

        // Value-only combo boxes (empty caption -> choice + caret, no leading icon) to
        // match the JUCE ComboBoxes; the ctor glyph is unused in combo mode.
        type_  = std::make_unique<ValueSetting> (store_, ix_.type,  theme_, factory_ui_visage::icons::caret(), "");
        slope_ = std::make_unique<ValueSetting> (store_, ix_.slope, theme_, factory_ui_visage::icons::caret(), "");
        chan_  = std::make_unique<ValueSetting> (store_, ix_.chan,  theme_, factory_ui_visage::icons::caret(), "");

        // Band type reads as its filter shape (Bell / shelves / HP / LP) rather than
        // text; the slope combo enables only for HP/LP cut bands.
        type_->setChoiceIcons (deq_ui::icons::bandTypeIcons());
        type_->onChange = [this] { updateTypeDependent(); };

        // JUCE-matched dial: a small name row (top) + a fixed-diameter dial + a value row
        // (bottom), with a bounds inset so the dial does not swell to fill a wide cell (so
        // the left FREQ/GAIN/Q and the right THRESH.. dials stay the same size — the layout
        // keeps the dial height-limited below the narrowest cell width).
        auto mkKnob = [&] (int idx, const char* name, int decimals)
        {
            auto k = std::make_unique<Knob> (store_, idx, theme_, decimals);
            k->setNameOverride (name);
            k->setDialProfile (16.0f, 18.0f, 11.0f, 12.0f, 6.0f);
            return k;
        };
        freq_ = mkKnob (ix_.freq, "FREQ", 0);
        gain_ = mkKnob (ix_.gain, "GAIN", 2);
        q_    = mkKnob (ix_.q,    "Q",    2);
        thr_  = mkKnob (ix_.thr,  "THRESH", 2);
        rng_  = mkKnob (ix_.rng,  "RANGE",  2);
        atk_  = mkKnob (ix_.atk,  "ATTACK", 2);
        rel_  = mkKnob (ix_.rel,  "RELEASE", 2);
        knee_ = mkKnob (ix_.knee, "KNEE",   2);

        for (visage::Frame* f : { (visage::Frame*) bypass_.get(), (visage::Frame*) listen_.get(),
                                  (visage::Frame*) dyn_.get(), (visage::Frame*) type_.get(),
                                  (visage::Frame*) slope_.get(), (visage::Frame*) chan_.get(),
                                  (visage::Frame*) freq_.get(), (visage::Frame*) gain_.get(),
                                  (visage::Frame*) q_.get(), (visage::Frame*) thr_.get(),
                                  (visage::Frame*) rng_.get(), (visage::Frame*) atk_.get(),
                                  (visage::Frame*) rel_.get(), (visage::Frame*) knee_.get() })
            addChild (*f);

        updateTypeDependent();
    }

    void DeqBandPanel::setDropdownRequest (factory_ui_visage::DropdownRequest req)
    {
        type_->requestDropdown  = req;
        slope_->requestDropdown = req;
        chan_->requestDropdown  = req;
    }

    void DeqBandPanel::setBand (int band)
    {
        if (band < 0) return;
        band_ = band;
        ix_ = indicesFor (band);
        rebind();
        updateTypeDependent();
        redraw();
    }

    void DeqBandPanel::refresh()
    {
        for (visage::Frame* f : { (visage::Frame*) bypass_.get(), (visage::Frame*) listen_.get(),
                                  (visage::Frame*) dyn_.get(), (visage::Frame*) type_.get(),
                                  (visage::Frame*) slope_.get(), (visage::Frame*) chan_.get(),
                                  (visage::Frame*) freq_.get(), (visage::Frame*) gain_.get(),
                                  (visage::Frame*) q_.get(), (visage::Frame*) thr_.get(),
                                  (visage::Frame*) rng_.get(), (visage::Frame*) atk_.get(),
                                  (visage::Frame*) rel_.get(), (visage::Frame*) knee_.get() })
            f->redraw();

        updateTypeDependent();
    }

    void DeqBandPanel::updateTypeDependent()
    {
        const int t = (int) store_.value (ix_.type);
        const bool cut = (t == (int) factory_core::BandType::HighPass
                          || t == (int) factory_core::BandType::LowPass);
        slope_->setEnabled (cut);
    }

    void DeqBandPanel::rebind()
    {
        bypass_->rebind (ix_.byp);
        listen_->rebind (ix_.lsn);
        dyn_->rebind (ix_.dyn);
        type_->rebind (ix_.type);
        slope_->rebind (ix_.slope);
        chan_->rebind (ix_.chan);
        freq_->rebind (ix_.freq);
        gain_->rebind (ix_.gain);
        q_->rebind (ix_.q);
        thr_->rebind (ix_.thr);
        rng_->rebind (ix_.rng);
        atk_->rebind (ix_.atk);
        rel_->rebind (ix_.rel);
        knee_->rebind (ix_.knee);
    }

    void DeqBandPanel::draw (visage::Canvas& canvas)
    {
        using namespace factory_ui_visage;
        paintCard (canvas, theme_, 0.0f, 0.0f, width(), height());

        const visage::Font titleFont = boldFont (14.0f);
        const std::uint32_t bandCol = theme_.palette.bandColours[(size_t) (band_ % (int) theme_.palette.bandColours.size())];
        canvas.setColor (visage::Color (bandCol));
        canvas.text ("BAND " + std::to_string (band_ + 1), titleFont, visage::Font::kLeft, 10.0f, 8.0f, 110.0f, 24.0f);

        if (dividerX_ >= 0.0f)
        {
            canvas.setColor (visage::Color (theme_.palette.track));
            canvas.segment (dividerX_, 12.0f, dividerX_, height() - 12.0f, 1.0f, false);
        }
    }

    void DeqBandPanel::resized()
    {
        // Tightened layout mirroring the JUCE BandControlPanel: the "BAND N" title shares
        // row 1 with the Listen/Bypass pills (left) and the Dynamics pill (right); row 2 is
        // the type/slope/channel combos (left); the rest is the knob row (freq/gain/q left,
        // thresh/range/attack/release/knee right). The dial is height-limited (see the
        // setDialProfile note) so every knob is the same diameter regardless of cell width.
        const float pad = 8.0f;
        const float x = pad;
        const float w = std::max (0.0f, width() - 2.0f * pad);

        const float leftW = w * 0.44f;
        const float gap = 12.0f;
        dividerX_ = x + leftW + gap * 0.5f;
        const float rightX = x + leftW + gap;
        const float rightW = w - leftW - gap;

        // ---- row 1: title (left, drawn in draw) + Listen/Bypass pills (right) | Dynamics --
        // The pills are pill(≈34px) + gap + caption, so give each cell enough width for
        // the full "Listen"/"Bypass" caption (they were clipping at 70/76px).
        const float row1Y = pad;
        const float row1H = 24.0f;
        const float bypassW = 88.0f, listenW = 88.0f;
        bypass_->setBounds (x + leftW - bypassW, row1Y, bypassW, row1H);
        listen_->setBounds (x + leftW - bypassW - 6.0f - listenW, row1Y, listenW, row1H);
        dyn_->setBounds (rightX, row1Y, 110.0f, row1H);

        // ---- row 2: type / slope / channel combos (left half) ------------------
        // Type is widened (it carries an icon + text); slope/channel take the rest.
        const float row2Y = row1Y + row1H + 6.0f;
        const float row2H = 24.0f;
        const float cellGap = 5.0f;
        const float usable = leftW - 2.0f * cellGap;
        const float typeW  = usable * 0.42f;
        const float slopeW = usable * 0.31f;
        const float chanW  = std::max (0.0f, usable - typeW - slopeW);
        type_->setBounds  (x,                                    row2Y, typeW,  row2H);
        slope_->setBounds (x + typeW + cellGap,                 row2Y, slopeW, row2H);
        chan_->setBounds  (x + typeW + slopeW + 2.0f * cellGap, row2Y, chanW,  row2H);

        // ---- knob row -----------------------------------------------------------
        const float knobY = row2Y + row2H + 8.0f;
        const float knobH = std::max (0.0f, (float) height() - knobY - pad);
        const float lkw = leftW / 3.0f;
        freq_->setBounds (x,               knobY, lkw, knobH);
        gain_->setBounds (x + lkw,         knobY, lkw, knobH);
        q_->setBounds    (x + 2.0f * lkw,  knobY, lkw, knobH);

        const float rkw = rightW / 5.0f;
        thr_->setBounds  (rightX,               knobY, rkw, knobH);
        rng_->setBounds  (rightX + rkw,         knobY, rkw, knobH);
        atk_->setBounds  (rightX + 2.0f * rkw,  knobY, rkw, knobH);
        rel_->setBounds  (rightX + 3.0f * rkw,  knobY, rkw, knobH);
        knee_->setBounds (rightX + 4.0f * rkw,  knobY, rkw, knobH);
    }
} // namespace deq_ui
