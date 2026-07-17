#include "GalleryFrame.h"

#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Icons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using factory_params::ParamDesc;
using factory_params::floatParam;
using factory_params::boolParam;
using factory_params::choiceParam;

namespace fuv = factory_ui_visage;

namespace
{
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kFrozenPhase = 0.35; // fixed synthetic frame for deterministic capture
}

std::vector<ParamDesc> GalleryFrame::buildParams()
{
    // P2a surface (linear %, skewed ms, %, two bools) + the P2b additions the new
    // widgets bind to: two Choice params (Segmented, ValueSetting), a Float
    // (LinkSlider) and a bool (the toggle IconButton).
    return {
        floatParam ("depth", "Depth", 0.0f,  100.0f,  0.0f,  40.0f, " %",  1),
        floatParam ("time",  "Time",  1.0f, 2000.0f,  0.0f, 240.0f, " ms", 1, /*skewCentre*/ 120.0f),
        floatParam ("mix",   "Mix",   0.0f,  100.0f,  0.0f,  50.0f, " %",  1),
        boolParam  ("sync",  "Sync",  false, 1),
        boolParam  ("wide",  "Wide",  true,  1),
        choiceParam ("mode",    "Mode",    { "Soft", "Hard", "Clip" },          0, 1),
        choiceParam ("quality", "Quality", { "Draft", "Normal", "High", "Ultra" }, 1, 1),
        floatParam  ("link",    "Link",    0.0f, 100.0f, 0.0f, 75.0f, " %", 1),
        boolParam   ("listen",  "Listen",  false, 1),
    };
}

GalleryFrame::GalleryFrame()
    : store_ (buildParams()),
      theme_ (fuv::Theme::defaults())
{
    spectrumModel_.setOrderForSampleRate (sampleRate_);
    const int depthIdx = store_.indexOf ("depth");
    spectrumPeakGain_ = 0.12f + 0.34f * (store_.value (depthIdx) / 100.0f);
    sweeper_ = factory_params::ChangeSweeper (store_);

    // --- card 1: P2a primitives ----------------------------------------------
    for (const char* id : { "depth", "time", "mix" })
    {
        auto knob = std::make_unique<fuv::Knob> (store_, store_.indexOf (id), theme_, /*decimals*/ 0);
        addChild (knob.get());
        knobs_.push_back (std::move (knob));
    }
    for (const char* id : { "sync", "wide" })
    {
        auto toggle = std::make_unique<fuv::PillToggle> (store_, store_.indexOf (id), theme_);
        addChild (toggle.get());
        toggles_.push_back (std::move (toggle));
    }

    // --- card 2: P2b widget set ----------------------------------------------
    // Preset selector with a fake list: a section header, items, a separator,
    // a second section, and a non-steppable "Save As..." action row.
    presetSelector_ = std::make_unique<fuv::PresetSelectorView> (theme_);
    {
        using Entry = fuv::PresetSelectorView::Entry;
        std::vector<Entry> menu {
            Entry::header ("Factory"),
            Entry::item ("Init"),
            Entry::item ("Warm Pad"),
            Entry::item ("Pluck"),
            Entry::separator(),
            Entry::header ("User"),
            Entry::item ("My Preset"),
            Entry::separator(),
            Entry::item ("Save As...", /*enabled*/ true, /*steppable*/ false),
        };
        presetSelector_->setMenu (std::move (menu), /*selected item-row*/ 1); // "Warm Pad"
    }
    presetSelector_->requestDropdown =
        [this] (std::vector<fuv::Dropdown::Item> items, int sel, visage::Frame* anchor, std::function<void (int)> onSel)
        { presentDropdown (std::move (items), sel, anchor, std::move (onSel)); };
    addChild (presetSelector_.get());

    segmented_ = std::make_unique<fuv::Segmented> (store_, store_.indexOf ("mode"), theme_);
    addChild (segmented_.get());

    // Icon buttons: Undo / Redo (momentary) + a Listen toggle bound to "listen".
    {
        auto undo = std::make_unique<fuv::IconButton> (theme_, fuv::icons::undo(), fuv::IconButton::Mode::momentary);
        auto redo = std::make_unique<fuv::IconButton> (theme_, fuv::icons::redo(), fuv::IconButton::Mode::momentary);
        auto listen = std::make_unique<fuv::IconButton> (theme_, fuv::icons::listen(), fuv::IconButton::Mode::toggle);
        const int li = store_.indexOf ("listen");
        listen->setToggleState (store_.value (li) > 0.5f);
        listen->onToggle = [this, li] (bool on)
        {
            store_.beginGesture (li);
            store_.setFromUi (li, on ? 1.0f : 0.0f);
            store_.endGesture (li);
        };
        for (auto* b : { undo.get(), redo.get(), listen.get() })
            addChild (b);
        iconButtons_.push_back (std::move (undo));
        iconButtons_.push_back (std::move (redo));
        iconButtons_.push_back (std::move (listen));
    }

    valueSetting_ = std::make_unique<fuv::ValueSetting> (store_, store_.indexOf ("quality"), theme_,
                                                         fuv::icons::quality(), "QUALITY");
    valueSetting_->requestDropdown =
        [this] (std::vector<fuv::Dropdown::Item> items, int sel, visage::Frame* anchor, std::function<void (int)> onSel)
        { presentDropdown (std::move (items), sel, anchor, std::move (onSel)); };
    addChild (valueSetting_.get());

    linkSlider_ = std::make_unique<fuv::LinkSlider> (store_, store_.indexOf ("link"), theme_,
                                                     "STEREO LINK", fuv::icons::link(), /*decimals*/ 0);
    addChild (linkSlider_.get());

    spectrumView_ = std::make_unique<fuv::SpectrumView> (theme_, spectrumModel_, sampleRate_);
    spectrumView_->onTick = [this] { spectrumTick(); };
    addChild (spectrumView_.get());

    // Shared Dropdown overlay — added LAST so it sits on top; hidden until opened.
    dropdown_ = std::make_unique<fuv::Dropdown> (theme_);
    addChild (dropdown_.get());
    dropdown_->setVisible (false);

    // See every child mouse-down too, so the harness can read back where visage
    // actually delivered a click (coordinate calibration).
    setReceiveChildMouseEvents (true);
}

void GalleryFrame::mouseDown (const visage::MouseEvent& e)
{
    lastMouseX_ = e.windowPosition().x;
    lastMouseY_ = e.windowPosition().y;
}

void GalleryFrame::resized()
{
    const float margin = 20.0f;
    const float gap = 16.0f;

    card1X_ = margin;
    card1Y_ = margin;
    card1W_ = std::max (0.0f, width() - 2.0f * margin);
    card1H_ = 276.0f;

    card2X_ = margin;
    card2Y_ = card1Y_ + card1H_ + gap;
    card2W_ = card1W_;
    card2H_ = std::max (0.0f, height() - card2Y_ - margin);

    // --- card 1 layout (P2a) -------------------------------------------------
    {
        const float pad = 22.0f, titleH = 46.0f;
        const float cx = card1X_ + pad, cw = std::max (0.0f, card1W_ - 2.0f * pad);
        const float knobRowY = card1Y_ + titleH, knobH = 156.0f;
        const float knobW = knobs_.empty() ? 0.0f : cw / (float) knobs_.size();
        for (std::size_t i = 0; i < knobs_.size(); ++i)
            knobs_[i]->setBounds (cx + (float) i * knobW, knobRowY, knobW, knobH);

        const float toggleRowY = knobRowY + knobH + 10.0f, toggleH = 40.0f;
        const float toggleW = toggles_.empty() ? 0.0f : cw / (float) toggles_.size();
        for (std::size_t i = 0; i < toggles_.size(); ++i)
            toggles_[i]->setBounds (cx + (float) i * toggleW + 8.0f, toggleRowY, toggleW - 8.0f, toggleH);
    }

    // --- card 2 layout (P2b) -------------------------------------------------
    {
        const float pad = 22.0f, titleH = 34.0f, rowH = 30.0f, rowGap = 12.0f;
        const float cx = card2X_ + pad, cw = std::max (0.0f, card2W_ - 2.0f * pad);

        const float rowAY = card2Y_ + titleH + 6.0f;
        presetSelector_->setBounds (cx, rowAY, 280.0f, rowH);
        // Undo / Redo / Listen at the right edge.
        const float btnW = 34.0f, btnGap = 8.0f;
        for (std::size_t i = 0; i < iconButtons_.size(); ++i)
        {
            const float right = cx + cw - (float) (iconButtons_.size() - i) * (btnW + btnGap) + btnGap;
            iconButtons_[i]->setBounds (right, rowAY, btnW, rowH);
        }

        const float rowBY = rowAY + rowH + rowGap;
        segmented_->setBounds (cx, rowBY, 210.0f, rowH);
        valueSetting_->setBounds (cx + 222.0f, rowBY, 190.0f, rowH);
        const float lsX = cx + 424.0f;
        linkSlider_->setBounds (lsX, rowBY, std::max (0.0f, cx + cw - lsX), rowH);

        const float rowCY = rowBY + rowH + rowGap;
        const float specBottom = card2Y_ + card2H_ - pad;
        spectrumView_->setBounds (cx, rowCY, cw, std::max (0.0f, specBottom - rowCY));
    }

    // The Dropdown overlay covers the whole gallery so its panel can overflow the
    // small control that triggered it, and so an outside click dismisses it.
    if (dropdown_)
        dropdown_->setBounds (0.0f, 0.0f, width(), height());
}

void GalleryFrame::draw (visage::Canvas& canvas)
{
    fuv::paintBackground (canvas, theme_, 0.0f, 0.0f, width(), height());
    fuv::paintCard (canvas, theme_, card1X_, card1Y_, card1W_, card1H_);
    fuv::paintCard (canvas, theme_, card2X_, card2Y_, card2W_, card2H_);

    // Card 1 title + P2a caption.
    canvas.setColor (visage::Color (theme_.palette.text));
    canvas.text ("Factory UI · Visage", fuv::boldFont (theme_.font.title), visage::Font::kLeft,
                 card1X_ + 22.0f, card1Y_ + 8.0f, card1W_ - 44.0f, 34.0f);
    canvas.setColor (visage::Color (theme_.palette.accent));
    canvas.text ("P2a", fuv::boldFont (theme_.font.callout), visage::Font::kRight,
                 card1X_ + 22.0f, card1Y_ + 12.0f, card1W_ - 44.0f, 28.0f);

    // Card 2 title + P2b caption.
    canvas.setColor (visage::Color (theme_.palette.text));
    canvas.text ("Widget set", fuv::boldFont (theme_.font.callout), visage::Font::kLeft,
                 card2X_ + 22.0f, card2Y_ + 8.0f, card2W_ - 44.0f, 22.0f);
    canvas.setColor (visage::Color (theme_.palette.accent));
    canvas.text ("P2b", fuv::boldFont (theme_.font.callout), visage::Font::kRight,
                 card2X_ + 22.0f, card2Y_ + 8.0f, card2W_ - 44.0f, 22.0f);
}

bool GalleryFrame::reloadTheme (const std::string& jsonText, std::string& error)
{
    fuv::Theme parsed;
    if (! fuv::Theme::tryParse (jsonText, parsed, error))
        return false;
    theme_ = parsed;
    redrawAll();
    return true;
}

void GalleryFrame::presentDropdown (std::vector<fuv::Dropdown::Item> items, int selected,
                                    visage::Frame* anchor, std::function<void (int)> onSelect)
{
    if (! dropdown_ || anchor == nullptr)
        return;
    const visage::Point a = anchor->positionInWindow();
    const visage::Point self = positionInWindow();
    dropdown_->onSelect = std::move (onSelect);
    dropdown_->open (std::move (items), selected, a.x - self.x, a.y - self.y, anchor->width(), anchor->height());
}

bool GalleryFrame::openNamedDropdown (int which)
{
    if (which == 0 && presetSelector_) { presetSelector_->openMenu(); return true; }
    if (which == 1 && valueSetting_)   { valueSetting_->openMenu();   return true; }
    return false;
}

void GalleryFrame::buildSynthFrame (std::vector<float>& out, double phase) const
{
    const int N = spectrumModel_.size();
    out.resize ((std::size_t) N);
    if (N == 0)
        return;

    // Deterministic pink-ish noise floor via a fixed-seed LCG keyed by the phase
    // bucket (so a fixed phase gives a fixed frame — needed for frozen capture).
    std::uint32_t seed = 0x9e3779b9u ^ (std::uint32_t) (std::int64_t) (phase * 1024.0);
    auto rnd = [&seed]
    {
        seed = seed * 1664525u + 1013904223u;
        return (float) ((int) ((seed >> 9) & 0x7fffu) - 16384) / 16384.0f;
    };

    // Two peaks sliding in frequency with the phase.
    const float f1 = 150.0f  * std::pow (10.0f, (float) (0.5 + 0.5 * std::sin (phase)));            // 150..1500
    const float f2 = 1200.0f * std::pow (10.0f, 0.77f * (float) (0.5 + 0.5 * std::sin (phase * 0.63 + 1.0))); // 1.2k..7k
    const float g = spectrumPeakGain_;

    float pink = 0.0f;
    for (int n = 0; n < N; ++n)
    {
        pink = 0.97f * pink + 0.03f * rnd();
        float s = pink * 0.5f;
        s += g * (float) std::sin (kTwoPi * f1 * n / sampleRate_);
        s += g * 0.75f * (float) std::sin (kTwoPi * f2 * n / sampleRate_);
        out[(std::size_t) n] = s;
    }
}

void GalleryFrame::spectrumTick()
{
    // Params drive visuals: when "depth" changes, rescale the peak gain (only on
    // change — ChangeSweeper visits each advanced epoch exactly once).
    const int depthIdx = store_.indexOf ("depth");
    sweeper_.sweep (store_, [this, depthIdx] (int i)
    {
        if (i == depthIdx)
            spectrumPeakGain_ = 0.12f + 0.34f * (store_.value (depthIdx) / 100.0f);
    });

    buildSynthFrame (synthScratch_, animPhase_);
    spectrumModel_.writeSamples (synthScratch_.data(), (int) synthScratch_.size());
    spectrumModel_.update (sampleRate_);
    animPhase_ += 0.05;
}

void GalleryFrame::feedSpectrum (double phase)
{
    buildSynthFrame (synthScratch_, phase);
    spectrumModel_.reset();
    for (int k = 0; k < 4; ++k) // converge the smoother onto the fixed frame
    {
        spectrumModel_.writeSamples (synthScratch_.data(), (int) synthScratch_.size());
        spectrumModel_.update (sampleRate_);
    }
    if (spectrumView_)
        spectrumView_->redraw();
}

void GalleryFrame::setFrozen (bool frozen)
{
    frozen_ = frozen;
    if (spectrumView_)
        spectrumView_->setFrozen (frozen);
    if (frozen)
        feedSpectrum (kFrozenPhase); // deterministic held image
    redrawAll();
}

visage::Frame* GalleryFrame::frameForParam (int paramIndex) const
{
    for (const auto& k : knobs_)   if (k->paramIndex() == paramIndex) return k.get();
    for (const auto& t : toggles_) if (t->paramIndex() == paramIndex) return t.get();
    if (segmented_   && segmented_->paramIndex()   == paramIndex) return segmented_.get();
    if (linkSlider_  && linkSlider_->paramIndex()  == paramIndex) return linkSlider_.get();
    if (valueSetting_ && valueSetting_->paramIndex() == paramIndex) return valueSetting_.get();
    return nullptr;
}

bool GalleryFrame::widgetCentreInWindow (int paramIndex, float& outX, float& outY) const
{
    const visage::Frame* f = frameForParam (paramIndex);
    if (f == nullptr)
        return false;
    const visage::Point p = f->positionInWindow();
    outX = p.x + f->width() * 0.5f;
    outY = p.y + f->height() * 0.5f;
    return true;
}

bool GalleryFrame::widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const
{
    const visage::Frame* f = nullptr;
    if      (key == "preset")       f = presetSelector_.get();
    else if (key == "spectrum")     f = spectrumView_.get();
    else if (key == "valueSetting") f = valueSetting_.get();
    else                            f = frameForParam (store_.indexOf (key));
    if (f == nullptr)
        return false;
    const visage::Point p = f->positionInWindow();
    x = p.x; y = p.y; w = f->width(); h = f->height();
    return true;
}
