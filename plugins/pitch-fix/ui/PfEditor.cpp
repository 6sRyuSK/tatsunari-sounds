//
// plugins/pitch-fix/ui/PfEditor.cpp — the Pitch TatFixer Visage editor. See
// PfEditor.h for the shape; all look-and-feel comes from the shared
// factory_ui_visage design system (theme colours/fonts/chrome only — no local
// hex, per the house rule).
//
#include "PfEditor.h"

#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Icons.h"

#include <visage_graphics/canvas.h>

#include <cmath>
#include <cstdio>
#include <utility>

namespace pf_ui
{
    using namespace factory_ui_visage;

    // ---- PfStatusBadge --------------------------------------------------------
    void PfStatusBadge::draw (visage::Canvas& canvas)
    {
        if (onTick)
            onTick();

        const float w = width(), h = height();
        paintCardShell (canvas, 0.0f, 0.0f, w, h, theme_.card.cornerRadius,
                        visage::Color (theme_.palette.panel),
                        visage::Color (theme_.palette.track));

        const auto rd = [] (std::atomic<float>* p) { return p != nullptr ? p->load (std::memory_order_relaxed) : 0.0f; };
        const int   latency = feed_.latencySamples != nullptr
                                ? feed_.latencySamples->load (std::memory_order_relaxed) : 0;
        const float fs      = rd (feed_.sampleRateHz);
        const float ms      = fs > 0.0f ? 1000.0f * (float) latency / fs : 0.0f;
        const float det     = rd (feed_.detectedHz);
        const float tgt     = rd (feed_.targetHz);
        const float shift   = rd (feed_.shiftCents);

        char line1[96];
        std::snprintf (line1, sizeof (line1), "%d smp  |  %.1f ms @ %.1f kHz",
                       latency, (double) ms, (double) (fs / 1000.0f));
        char line2[96];
        if (det > 0.0f)
            std::snprintf (line2, sizeof (line2), "%.1f Hz > %.1f Hz   %+.1f ct",
                           (double) det, (double) tgt, (double) shift);
        else
            std::snprintf (line2, sizeof (line2), "unvoiced");

        const float pad = 12.0f;
        const float rowH = h * 0.5f;
        const float capW = 78.0f;
        canvas.setColor (theme_.palette.textSecondary);
        canvas.text ("LATENCY", boldFont (theme_.font.caption),
                     visage::Font::kLeft, pad, 0.0f, capW, rowH);
        canvas.text ("PITCH", boldFont (theme_.font.caption),
                     visage::Font::kLeft, pad, rowH, capW, rowH);
        canvas.setColor (theme_.palette.text);
        canvas.text (line1, regularFont (theme_.font.label),
                     visage::Font::kLeft, pad + capW, 0.0f, w - pad * 2 - capW, rowH);
        canvas.text (line2, regularFont (theme_.font.label),
                     visage::Font::kLeft, pad + capW, rowH, w - pad * 2 - capW, rowH);

        redraw();   // self-driving: the one animated frame in this editor
    }

    // ---- PfEditor ---------------------------------------------------------------
    PfEditor::PfEditor (const Theme& theme, factory_params::ParamStore& store,
                        const PfUiFeed& feed, PfPresetModel& presets)
        : theme_ (theme), store_ (store), presets_ (presets)
    {
        const auto knob = [&] (const char* id, const char* caption, int decimals)
        {
            auto k = std::make_unique<Knob> (store_, store_.indexOf (id), theme_, decimals);
            k->setNameOverride (caption);
            k->requestValueEntry = [this] (const ValueEntryRequest& r) { openValueEntry (r); };
            addChild (k.get());
            return k;
        };

        amount_     = knob ("amount",     "AMOUNT",     0);
        retune_     = knob ("retune",     "RETUNE",     0);
        glide_      = knob ("glide",      "GLIDE",      0);
        tolerance_  = knob ("tolerance",  "TOLERANCE",  0);
        hysteresis_ = knob ("hysteresis", "HYSTERESIS", 0);
        minPitch_   = knob ("min_pitch",  "MIN PITCH",  0);
        maxPitch_   = knob ("max_pitch",  "MAX PITCH",  0);
        threshold_  = knob ("threshold",  "THRESHOLD",  0);
        mix_        = knob ("mix",        "MIX",        0);
        out_        = knob ("out",        "OUT",        1);

        const auto big   = [] (Knob& k) { k.setDialProfile (16, 17, 12, 13, 0); };
        const auto small = [] (Knob& k) { k.setDialProfile (14, 14, 10, 11, 0); };
        big (*amount_); big (*retune_); big (*glide_); big (*tolerance_); big (*hysteresis_);
        small (*minPitch_); small (*maxPitch_); small (*threshold_); small (*mix_); small (*out_);

        key_ = std::make_unique<ValueSetting> (store_, store_.indexOf ("key"), theme_,
                                               icons::caret(), "KEY");
        key_->requestDropdown = [this] (auto items, int sel, visage::Frame* a, auto onSel)
        { presentDropdown (std::move (items), sel, a, std::move (onSel)); };
        addChild (key_.get());

        scale_ = std::make_unique<Segmented> (store_, store_.indexOf ("scale"), theme_);
        scale_->setLabelFontPx (12.0f);
        addChild (scale_.get());

        a4_ = std::make_unique<LinkSlider> (store_, store_.indexOf ("a4"), theme_, "A4", 1);
        a4_->setCaptionColumnPx (34.0f);
        a4_->requestValueEntry = [this] (const ValueEntryRequest& r) { openValueEntry (r); };
        addChild (a4_.get());

        buffer_ = std::make_unique<Segmented> (store_, store_.indexOf ("buffer"), theme_);
        buffer_->setLabelFontPx (12.0f);
        addChild (buffer_.get());

        status_ = std::make_unique<PfStatusBadge> (theme_, feed);
        addChild (status_.get());

        presetView_ = std::make_unique<PresetSelectorView> (theme_);
        presetView_->requestDropdown = [this] (auto items, int sel, visage::Frame* a, auto onSel)
        { presentDropdown (std::move (items), sel, a, std::move (onSel)); };
        presetView_->onChange = [this] (int row)
        {
            if (! presets_.load (row))
                presetView_->setSelectedIndex (presets_.currentIndex());
            rebuildPresetMenu();
            redrawAll();
        };
        addChild (presetView_.get());
        rebuildPresetMenu();

        // Shared overlays last == frontmost.
        dropdown_ = std::make_unique<Dropdown> (theme_);
        addChild (dropdown_.get());
        valueEntry_ = std::make_unique<ValueEntry> (theme_);
        addChild (valueEntry_.get());
    }

    PfEditor::~PfEditor() = default;

    void PfEditor::onStateReplaced()
    {
        if (valueEntry_)
            valueEntry_->cancelEntry();
        if (dropdown_ && dropdown_->isOpen())
            dropdown_->close();
        rebuildPresetMenu();
        redrawAll();
    }

    void PfEditor::setFrameTick (std::function<void()> fn)
    {
        if (status_)
            status_->onTick = std::move (fn);
    }

    void PfEditor::rebuildPresetMenu()
    {
        if (presetView_)
            presetView_->setItems (presets_.names(), presets_.currentIndex());
    }

    float PfEditor::k() const
    {
        const float kw = width()  / (float) kDesignW;
        const float kh = height() / (float) kDesignH;
        return kw < kh ? kw : kh;
    }

    void PfEditor::presentDropdown (std::vector<Dropdown::Item> items, int selected,
                                    visage::Frame* anchor, std::function<void (int)> onSelect)
    {
        if (dropdown_ == nullptr || anchor == nullptr)
            return;
        dropdown_->setBounds (0.0f, 0.0f, width(), height());
        const auto ap = anchor->positionInWindow();
        const auto mp = positionInWindow();
        dropdown_->onSelect = std::move (onSelect);
        dropdown_->open (std::move (items), selected,
                         ap.x - mp.x, ap.y - mp.y, anchor->width(), anchor->height());
    }

    void PfEditor::openValueEntry (const ValueEntryRequest& req)
    {
        if (valueEntry_ == nullptr)
            return;
        const auto mp = positionInWindow();
        valueEntry_->open (req.x - mp.x, req.y - mp.y, req.w, req.h,
                           req.prefill, req.fontPx, req.commit);
    }

    void PfEditor::draw (visage::Canvas& canvas)
    {
        paintBackground (canvas, theme_, 0.0f, 0.0f, width(), height());
        paintCard (canvas, theme_, S (18), S (18), width() - S (36), height() - S (36));

        canvas.setColor (theme_.palette.text);
        canvas.text ("PITCH TATFIXER", boldFont (theme_.font.title * k()),
                     visage::Font::kLeft, S (44), S (34), S (360), S (30));
        canvas.setColor (theme_.palette.textDim);
        canvas.text ("REAL-TIME PITCH CORRECTION", regularFont (theme_.font.caption * k()),
                     visage::Font::kLeft, S (46), S (62), S (360), S (16));
    }

    void PfEditor::resized()
    {
        if (valueEntry_)
            valueEntry_->cancelEntry();

        // Header: preset selector right-aligned.
        presetView_->setBounds (S (600), S (36), S (276), S (30));

        // Musical context row.
        key_->setBounds   (S (44),  S (92), S (200), S (44));
        scale_->setBounds (S (264), S (100), S (216), S (28));
        a4_->setBounds    (S (500), S (100), S (200), S (28));

        // Big correction knobs.
        const float bigY = S (150), bigH = S (158), bigW = S (152);
        const char* _ = nullptr; (void) _;
        Knob* bigs[5] = { amount_.get(), retune_.get(), glide_.get(), tolerance_.get(), hysteresis_.get() };
        for (int i = 0; i < 5; ++i)
            bigs[i]->setBounds (S (48) + (float) i * S (166), bigY, bigW, bigH);

        // Detector / output knobs.
        const float smallY = S (318), smallH = S (132), smallW = S (136);
        Knob* smalls[5] = { minPitch_.get(), maxPitch_.get(), threshold_.get(), mix_.get(), out_.get() };
        for (int i = 0; i < 5; ++i)
            smalls[i]->setBounds (S (66) + (float) i * S (160), smallY, smallW, smallH);

        // Footer: buffer mode + status badge.
        buffer_->setBounds (S (44), S (478), S (352), S (28));
        status_->setBounds (S (420), S (462), S (456), S (58));

        // Overlays cover the whole editor.
        dropdown_->setBounds (0.0f, 0.0f, width(), height());

        redrawAll();
    }
} // namespace pf_ui
