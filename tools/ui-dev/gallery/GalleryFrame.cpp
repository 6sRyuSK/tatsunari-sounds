#include "GalleryFrame.h"

#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Fonts.h"

#include <algorithm>

using factory_params::ParamDesc;
using factory_params::floatParam;
using factory_params::boolParam;

std::vector<ParamDesc> GalleryFrame::buildParams()
{
    // A small but representative surface: a linear %, a skewed ms, another %, and
    // two booleans. Built with the factory_params terse constructors.
    return {
        floatParam ("depth", "Depth", 0.0f,  100.0f,  0.0f,  40.0f, " %",  1),                  // linear %
        floatParam ("time",  "Time",  1.0f, 2000.0f,  0.0f, 240.0f, " ms", 1, /*skewCentre*/ 120.0f), // skewed ms
        floatParam ("mix",   "Mix",   0.0f,  100.0f,  0.0f,  50.0f, " %",  1),                  // linear %
        boolParam  ("sync",  "Sync",  false, 1),
        boolParam  ("wide",  "Wide",  true,  1),
    };
}

GalleryFrame::GalleryFrame()
    : store_ (buildParams()),
      theme_ (factory_ui_visage::Theme::defaults())
{
    // Three knobs (depth %, time ms, mix %) — 0 decimals reads cleanly here.
    for (const char* id : { "depth", "time", "mix" })
    {
        const int idx = store_.indexOf (id);
        auto knob = std::make_unique<factory_ui_visage::Knob> (store_, idx, theme_, /*decimals*/ 0);
        addChild (knob.get());
        knobs_.push_back (std::move (knob));
    }

    // Two pill toggles.
    for (const char* id : { "sync", "wide" })
    {
        const int idx = store_.indexOf (id);
        auto toggle = std::make_unique<factory_ui_visage::PillToggle> (store_, idx, theme_);
        addChild (toggle.get());
        toggles_.push_back (std::move (toggle));
    }
}

void GalleryFrame::resized()
{
    const float margin = 20.0f;
    cardX_ = margin;
    cardY_ = margin;
    cardW_ = std::max (0.0f, width() - 2.0f * margin);
    cardH_ = std::max (0.0f, height() - 2.0f * margin);

    const float pad = 22.0f;
    const float titleH = 46.0f;
    const float contentX = cardX_ + pad;
    const float contentW = std::max (0.0f, cardW_ - 2.0f * pad);

    // Row of knobs under the title.
    const float knobRowY = cardY_ + titleH;
    const float knobH = 156.0f;
    const float knobW = knobs_.empty() ? 0.0f : contentW / static_cast<float> (knobs_.size());
    for (std::size_t i = 0; i < knobs_.size(); ++i)
        knobs_[i]->setBounds (contentX + static_cast<float> (i) * knobW, knobRowY, knobW, knobH);

    // Row of toggles under the knobs.
    const float toggleRowY = knobRowY + knobH + 10.0f;
    const float toggleH = 40.0f;
    const float toggleW = toggles_.empty() ? 0.0f : contentW / static_cast<float> (toggles_.size());
    for (std::size_t i = 0; i < toggles_.size(); ++i)
        toggles_[i]->setBounds (contentX + static_cast<float> (i) * toggleW + 8.0f, toggleRowY,
                                toggleW - 8.0f, toggleH);
}

void GalleryFrame::draw (visage::Canvas& canvas)
{
    // Background chrome + centred card (children draw on top).
    factory_ui_visage::paintBackground (canvas, theme_, 0.0f, 0.0f, width(), height());
    factory_ui_visage::paintCard (canvas, theme_, cardX_, cardY_, cardW_, cardH_);

    // Title, top-left inside the card.
    canvas.setColor (visage::Color (theme_.palette.text));
    canvas.text ("Factory UI · Visage", factory_ui_visage::boldFont (theme_.font.title),
                 visage::Font::kLeft, cardX_ + 22.0f, cardY_ + 8.0f, cardW_ - 44.0f, 34.0f);

    // Small coral caption, top-right inside the card.
    canvas.setColor (visage::Color (theme_.palette.accent));
    canvas.text ("P2a", factory_ui_visage::boldFont (theme_.font.callout),
                 visage::Font::kRight, cardX_ + 22.0f, cardY_ + 12.0f, cardW_ - 44.0f, 28.0f);
}

bool GalleryFrame::reloadTheme (const std::string& jsonText, std::string& error)
{
    factory_ui_visage::Theme parsed;
    if (! factory_ui_visage::Theme::tryParse (jsonText, parsed, error))
        return false;

    theme_ = parsed;   // widgets hold a reference to theme_, so this re-themes them
    redrawAll();
    return true;
}

bool GalleryFrame::widgetCentreInWindow (int paramIndex, float& outX, float& outY) const
{
    const visage::Frame* found = nullptr;
    for (const auto& k : knobs_)
        if (k->paramIndex() == paramIndex) { found = k.get(); break; }
    if (found == nullptr)
        for (const auto& t : toggles_)
            if (t->paramIndex() == paramIndex) { found = t.get(); break; }
    if (found == nullptr)
        return false;

    const visage::Point p = found->positionInWindow();
    outX = p.x + found->width() * 0.5f;
    outY = p.y + found->height() * 0.5f;
    return true;
}
