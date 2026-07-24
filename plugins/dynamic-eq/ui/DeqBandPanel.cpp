//
// DeqBandPanel.cpp — the Visage port of the JUCE BandControlPanel (see DeqBandPanel.h).
//
#include "DeqBandPanel.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Icons.h"

#include <cmath>
#include <string>

namespace deq_ui
{
    namespace { std::string bp (int band, const char* s) { return "b" + std::to_string (band) + "_" + s; } }

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

        type_  = std::make_unique<ValueSetting> (store_, ix_.type,  theme_, icons::modeSoft(), "TYPE");
        slope_ = std::make_unique<ValueSetting> (store_, ix_.slope, theme_, icons::quality(),  "SLOPE");
        chan_  = std::make_unique<ValueSetting> (store_, ix_.chan,  theme_, icons::channel(),  "CH");

        auto mkKnob = [&] (int idx, const char* name, int decimals)
        {
            auto k = std::make_unique<Knob> (store_, idx, theme_, decimals);
            k->setNameOverride (name);
            k->setDialProfile (14.0f, 14.0f, 10.0f, 11.0f, 0.0f);
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
        canvas.text ("BAND " + std::to_string (band_ + 1), titleFont, visage::Font::kLeft, 12.0f, 10.0f, 120.0f, 20.0f);

        if (dividerX_ >= 0.0f)
        {
            canvas.setColor (visage::Color (theme_.palette.track));
            canvas.segment (dividerX_, 14.0f, dividerX_, height() - 14.0f, 1.0f, false);
        }
    }

    void DeqBandPanel::resized()
    {
        const float pad = 10.0f;
        const float x = pad, y = pad;
        const float w = std::max (0.0f, width() - 2.0f * pad);
        const float h = std::max (0.0f, height() - 2.0f * pad);

        const float leftW = w * 0.45f;
        const float gap = 14.0f;
        dividerX_ = x + leftW + gap * 0.5f;
        const float rightX = x + leftW + gap;
        const float rightW = w - leftW - gap;

        // ---- left: EQ ----
        const float row1Y = y + 26.0f; // below the BAND title
        const float row1H = 24.0f;
        // bypass / listen at the right of row 1.
        bypass_->setBounds (x + leftW - 84.0f, row1Y, 84.0f, row1H);
        listen_->setBounds (x + leftW - 84.0f - 4.0f - 76.0f, row1Y, 76.0f, row1H);

        const float row2Y = row1Y + row1H + 6.0f;
        const float row2H = 26.0f;
        const float cw = (leftW - 12.0f) / 3.0f;
        type_->setBounds (x, row2Y, cw, row2H);
        slope_->setBounds (x + cw + 6.0f, row2Y, cw, row2H);
        chan_->setBounds (x + 2.0f * (cw + 6.0f), row2Y, cw, row2H);

        const float knobY = row2Y + row2H + 8.0f;
        const float knobH = std::max (0.0f, y + h - knobY);
        const float lkw = leftW / 3.0f;
        freq_->setBounds (x,               knobY, lkw, knobH);
        gain_->setBounds (x + lkw,         knobY, lkw, knobH);
        q_->setBounds    (x + 2.0f * lkw,  knobY, lkw, knobH);

        // ---- right: Dynamics ----
        dyn_->setBounds (rightX, row1Y, 140.0f, row1H);
        const float rkw = rightW / 5.0f;
        thr_->setBounds  (rightX,               knobY, rkw, knobH);
        rng_->setBounds  (rightX + rkw,         knobY, rkw, knobH);
        atk_->setBounds  (rightX + 2.0f * rkw,  knobY, rkw, knobH);
        rel_->setBounds  (rightX + 3.0f * rkw,  knobY, rkw, knobH);
        knee_->setBounds (rightX + 4.0f * rkw,  knobY, rkw, knobH);
    }
} // namespace deq_ui
